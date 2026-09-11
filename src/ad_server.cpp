#include "adserve/ad_server.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "adserve/auction.h"

namespace adserve {

const char* to_string(NoFillReason reason) {
    switch (reason) {
        case NoFillReason::None: return "none";
        case NoFillReason::NoRelevantAds: return "no_relevant_ads";
        case NoFillReason::BelowFloor: return "below_floor";
        case NoFillReason::CappedOrNoBudget: return "capped_or_no_budget";
    }
    return "unknown";
}

AdServer::AdServer(std::vector<Ad> ads, const std::vector<Campaign>& campaigns,
                   ServerConfig config)
    : config_(config),
      ads_(std::move(ads)),
      index_(ads_),
      context_cache_(config.cache_capacity, config.cache_shards),
      budgets_(campaigns),
      freq_caps_(config.freq_cap, config.freq_window_ms, config.freq_shards),
      ctr_(ads_.size()) {
    for (std::size_t i = 0; i < ads_.size(); ++i) {
        if (!ad_position_.emplace(ads_[i].id, i).second) {
            throw std::invalid_argument("duplicate ad id " + std::to_string(ads_[i].id));
        }
    }
}

const Ad* AdServer::find_ad(AdId ad_id) const {
    auto it = ad_position_.find(ad_id);
    return it == ad_position_.end() ? nullptr : &ads_[it->second];
}

std::shared_ptr<const SparseVector> AdServer::page_context(const AdRequest& request,
                                                           bool& cache_hit) {
    cache_hit = false;
    const bool cacheable = config_.use_context_cache && !request.page_url.empty();

    if (cacheable) {
        if (auto cached = context_cache_.get(request.page_url)) {
            cache_hit = true;
            return *cached;
        }
    }

    // Cache miss: tokenising and weighting the page is the most expensive step for long
    // pages, and popular pages are requested over and over, which is why it is cached.
    // shared_ptr lets a reader keep using a vector even if another thread evicts it.
    auto vec = std::make_shared<const SparseVector>(index_.vectorize(request.page_text));
    if (cacheable) context_cache_.put(request.page_url, vec);
    return vec;
}

AdResponse AdServer::serve(const AdRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    AdResponse response;

    auto finish = [&]() -> AdResponse {
        response.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        metrics_.record(response);
        return response;
    };

    // 1. Page context.
    bool cache_hit = false;
    const auto page = page_context(request, cache_hit);
    response.cache_hit = cache_hit;

    // 2. Candidate retrieval.
    const std::vector<Candidate> candidates =
        config_.match_mode == MatchMode::InvertedIndex
            ? index_.top_k(*page, config_.top_k, config_.min_relevance)
            : index_.top_k_brute_force(*page, config_.top_k, config_.min_relevance);
    if (candidates.empty()) {
        response.reason = NoFillReason::NoRelevantAds;
        return finish();
    }

    // 3. Eligibility filters. These checks are cheap pre-filters; the authoritative,
    //    race-free checks happen when the winner is charged in step 5.
    std::vector<Bid> bids;
    std::vector<float> relevance;
    bool any_above_floor = false;
    for (const Candidate& c : candidates) {
        const Ad& ad = ads_[c.ad_index];
        if (ad.bid_cpm < request.floor_cpm) continue;
        any_above_floor = true;
        if (!freq_caps_.allowed(request.user_id, ad.campaign, request.now_ms)) continue;
        if (!budgets_.can_afford(ad.campaign, cpm_to_micros_per_impression(ad.bid_cpm))) continue;

        const double quality = static_cast<double>(c.relevance) * ctr_.quality_factor(c.ad_index);
        bids.push_back({c.ad_index, ad.bid_cpm, quality});
        relevance.push_back(c.relevance);
    }
    if (!any_above_floor) {
        response.reason = NoFillReason::BelowFloor;
        return finish();
    }

    // 4 + 5. Auction, then commit. Between the pre-filter and now, a concurrent request
    // may have spent the winner's last budget or used its last frequency-cap slot. The
    // commit therefore re-checks atomically: reserve budget (CAS), then record the
    // impression (under the shard lock), refunding the reservation if the second step
    // fails. On failure the winner is dropped and the auction re-run on the rest.
    while (!bids.empty()) {
        const AuctionResult result = run_second_price_auction(bids, request.floor_cpm);
        if (!result.has_winner) break;

        const Bid& winner = bids[result.winner];
        const Ad& ad = ads_[winner.ad_index];
        const Micros charge = cpm_to_micros_per_impression(result.price_cpm);

        if (budgets_.try_spend(ad.campaign, charge)) {
            if (freq_caps_.try_record(request.user_id, ad.campaign, request.now_ms)) {
                ctr_.record_impression(winner.ad_index);
                response.filled = true;
                response.ad_id = ad.id;
                response.campaign = ad.campaign;
                response.price_cpm = result.price_cpm;
                response.relevance = relevance[result.winner];
                return finish();
            }
            budgets_.refund(ad.campaign, charge);
        }
        bids.erase(bids.begin() + static_cast<std::ptrdiff_t>(result.winner));
        relevance.erase(relevance.begin() + static_cast<std::ptrdiff_t>(result.winner));
    }

    response.reason = NoFillReason::CappedOrNoBudget;
    return finish();
}

bool AdServer::record_click(AdId ad_id) {
    auto it = ad_position_.find(ad_id);
    if (it == ad_position_.end()) return false;
    ctr_.record_click(it->second);
    metrics_.record_click();
    return true;
}

}  // namespace adserve

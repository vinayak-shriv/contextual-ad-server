#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adserve/ad_server.h"
#include "mini_test.h"
#include "synthetic.h"

using namespace adserve;

namespace {

std::vector<Ad> small_inventory() {
    return {
        {1, 1, 5.0, {"running shoes", "marathon"}, "StrideMax"},
        {2, 2, 3.0, {"running shoes", "trail"}, "TrailFox"},
        {3, 3, 8.0, {"credit card", "cashback"}, "ZenCard"},
    };
}

std::vector<Campaign> big_budgets() {
    return {{1, 1'000 * kMicrosPerUnit}, {2, 1'000 * kMicrosPerUnit}, {3, 1'000 * kMicrosPerUnit}};
}

AdRequest request(const std::string& user, const std::string& url, const std::string& text,
                  double floor = 0.0) {
    AdRequest r;
    r.user_id = user;
    r.page_url = url;
    r.page_text = text;
    r.floor_cpm = floor;
    r.now_ms = 0;
    return r;
}

const char* kRunningPage = "How to train for a marathon and choose running shoes";

}  // namespace

TEST(server_serves_the_most_relevant_high_value_ad) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});
    const auto r = server.serve(request("u1", "/run", kRunningPage));
    CHECK(r.filled);
    CHECK_EQ(r.ad_id, 1u);                 // matches more terms and bids more than ad 2
    CHECK(r.price_cpm > 0.0 && r.price_cpm <= 5.0);  // second price, never above the bid
    CHECK(r.relevance > 0.0);
    CHECK(r.reason == NoFillReason::None);
}

TEST(server_reports_no_relevant_ads_for_unrelated_page) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});
    const auto r = server.serve(request("u1", "/x", "medieval poetry and cathedral architecture"));
    CHECK(!r.filled);
    CHECK(r.reason == NoFillReason::NoRelevantAds);
}

TEST(server_respects_publisher_floor) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});
    const auto r = server.serve(request("u1", "/run", kRunningPage, 6.0));  // both shoe ads bid < 6
    CHECK(!r.filled);
    CHECK(r.reason == NoFillReason::BelowFloor);

    const auto lone = server.serve(request("u2", "/run", kRunningPage, 4.0));  // only ad 1 qualifies
    CHECK(lone.filled);
    CHECK_EQ(lone.ad_id, 1u);
    CHECK_NEAR(lone.price_cpm, 4.0, 1e-9);  // lone bidder pays the floor
}

// Found by reading the filter, not by a crash: a NaN reserve passes every
// comparison (`bid < NaN` is false), so it used to reach the price conversion
// and turn a NaN into an integer, which is undefined behaviour. On x86 that
// happened to produce INT64_MIN, try_spend rejected the negative amount, and the
// request quietly reported capped_or_no_budget -- a wrong answer with no symptom.
TEST(server_treats_a_nonsense_floor_as_no_reserve) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});

    const auto nan_floor = server.serve(
        request("u1", "/run", kRunningPage, std::numeric_limits<double>::quiet_NaN()));
    CHECK(nan_floor.filled);
    CHECK(std::isfinite(nan_floor.price_cpm));
    CHECK(nan_floor.price_cpm >= 0.0);

    const auto negative = server.serve(request("u2", "/run", kRunningPage, -5.0));
    CHECK(negative.filled);
    CHECK(negative.price_cpm >= 0.0);

    // Infinity is different: it is a coherent reserve that nothing can clear.
    const auto infinite = server.serve(
        request("u3", "/run", kRunningPage, std::numeric_limits<double>::infinity()));
    CHECK(!infinite.filled);
    CHECK(infinite.reason == NoFillReason::BelowFloor);
}

TEST(server_frequency_cap_rotates_to_next_campaign_then_stops) {
    ServerConfig cfg;
    cfg.freq_cap = 1;
    AdServer server(small_inventory(), big_budgets(), cfg);

    CHECK_EQ(server.serve(request("u1", "/run", kRunningPage)).ad_id, 1u);
    CHECK_EQ(server.serve(request("u1", "/run", kRunningPage)).ad_id, 2u);  // campaign 1 capped
    const auto third = server.serve(request("u1", "/run", kRunningPage));
    CHECK(!third.filled);
    CHECK(third.reason == NoFillReason::CappedOrNoBudget);

    CHECK_EQ(server.serve(request("u2", "/run", kRunningPage)).ad_id, 1u);  // other user unaffected
}

TEST(server_skips_campaigns_without_budget) {
    std::vector<Campaign> budgets = big_budgets();
    budgets[0].daily_budget = 0;  // campaign 1 is out of money
    AdServer server(small_inventory(), budgets, ServerConfig{});
    const auto r = server.serve(request("u1", "/run", kRunningPage));
    CHECK(r.filled);
    CHECK_EQ(r.ad_id, 2u);
    CHECK_EQ(server.budgets().spent(1), 0);
    CHECK_EQ(server.budgets().spent(2), cpm_to_micros_per_impression(r.price_cpm));
}

TEST(server_caches_page_context_by_url) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});
    CHECK(!server.serve(request("u1", "/run", kRunningPage)).cache_hit);
    CHECK(server.serve(request("u2", "/run", kRunningPage)).cache_hit);
    CHECK(!server.serve(request("u3", "", kRunningPage)).cache_hit);  // no URL -> not cacheable

    ServerConfig no_cache;
    no_cache.use_context_cache = false;
    AdServer uncached(small_inventory(), big_budgets(), no_cache);
    uncached.serve(request("u1", "/run", kRunningPage));
    CHECK(!uncached.serve(request("u2", "/run", kRunningPage)).cache_hit);

    const auto s = server.stats();
    CHECK_EQ(s.requests, 3u);
    CHECK_EQ(s.cache_hits, 1u);
}

TEST(server_clicks_feed_the_ctr_model) {
    AdServer server(small_inventory(), big_budgets(), ServerConfig{});
    const double before = server.ctr().predicted_ctr(1);
    CHECK(server.record_click(2));
    CHECK(server.ctr().predicted_ctr(1) > before);  // ad id 2 lives at position 1
    CHECK(!server.record_click(999));
    CHECK_EQ(server.stats().clicks, 1u);
}

// The two retrieval strategies must produce identical serving decisions end to end.
TEST(server_brute_force_and_indexed_modes_agree) {
    synthetic::Config cfg;
    cfg.num_ads = 1'500;
    cfg.num_campaigns = 60;
    cfg.num_topics = 30;
    cfg.num_pages = 400;
    cfg.words_per_page = 100;
    const auto w = synthetic::generate(cfg);

    ServerConfig indexed_cfg;
    ServerConfig brute_cfg;
    brute_cfg.match_mode = MatchMode::BruteForce;
    AdServer indexed(w.ads, w.campaigns, indexed_cfg);
    AdServer brute(w.ads, w.campaigns, brute_cfg);

    int disagreements = 0;
    for (std::size_t i = 0; i < w.pages.size(); ++i) {
        const auto req = request("user" + std::to_string(i % 37), w.pages[i].url, w.pages[i].text);
        const auto a = indexed.serve(req);
        const auto b = brute.serve(req);
        if (a.filled != b.filled || a.ad_id != b.ad_id || a.price_cpm != b.price_cpm) ++disagreements;
    }
    CHECK_EQ(disagreements, 0);
}

// Hammer the server from many threads with tiny budgets and a tight frequency cap, then
// check the invariants that must hold no matter how requests interleave.
TEST(server_invariants_hold_under_concurrent_load) {
    synthetic::Config cfg;
    cfg.num_ads = 800;
    cfg.num_campaigns = 40;
    cfg.num_topics = 20;
    cfg.num_pages = 300;
    cfg.words_per_page = 80;
    cfg.campaign_budget = 0.05;  // 50,000 micros: only a handful of impressions each
    const auto w = synthetic::generate(cfg);

    ServerConfig sc;
    sc.freq_cap = 2;
    AdServer server(w.ads, w.campaigns, sc);

    std::mutex mu;
    std::map<CampaignId, Micros> charged;                       // sum of prices we were told
    std::map<std::pair<std::string, CampaignId>, int> seen;     // impressions per user+campaign

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            synthetic::ZipfSampler pages(w.pages.size(), 1.0, 1000 + t);
            for (int i = 0; i < 3'000; ++i) {
                const auto& p = w.pages[pages.next()];
                const std::string user = "u" + std::to_string((i * 13 + t) % 25);
                const auto r = server.serve(request(user, p.url, p.text));
                if (!r.filled) continue;
                std::lock_guard<std::mutex> lock(mu);
                charged[r.campaign] += cpm_to_micros_per_impression(r.price_cpm);
                ++seen[{user, r.campaign}];
            }
        });
    }
    for (auto& th : threads) th.join();

    int over_budget = 0, ledger_mismatch = 0, over_cap = 0;
    for (const auto& c : w.campaigns) {
        const Micros spent = server.budgets().spent(c.id);
        if (spent > c.daily_budget) ++over_budget;
        const auto it = charged.find(c.id);
        if (spent != (it == charged.end() ? 0 : it->second)) ++ledger_mismatch;
    }
    for (const auto& [key, count] : seen) {
        if (count > sc.freq_cap) ++over_cap;
    }
    CHECK_EQ(over_budget, 0);
    CHECK_EQ(ledger_mismatch, 0);  // every refund balanced every failed reservation
    CHECK_EQ(over_cap, 0);
    CHECK(server.stats().filled > 0u);
    CHECK_EQ(server.stats().requests, 8u * 3'000u);
}

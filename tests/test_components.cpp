#include <atomic>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adserve/auction.h"
#include "adserve/budget.h"
#include "adserve/context_index.h"
#include "adserve/ctr_estimator.h"
#include "adserve/frequency_cap.h"
#include "adserve/inventory_loader.h"
#include "adserve/lru_cache.h"
#include "adserve/metrics.h"
#include "adserve/tokenizer.h"
#include "mini_test.h"
#include "synthetic.h"

using namespace adserve;

// ---------------------------------------------------------------- tokenizer

TEST(tokenizer_lowercases_and_splits_on_punctuation) {
    const auto t = tokenize("Best RUNNING-shoes, 2026!");
    CHECK_EQ(t.size(), 4u);
    if (t.size() == 4) {
        CHECK_EQ(t[0], std::string("best"));
        CHECK_EQ(t[1], std::string("running"));
        CHECK_EQ(t[2], std::string("shoes"));
        CHECK_EQ(t[3], std::string("2026"));
    }
}

TEST(tokenizer_drops_stopwords_and_single_characters) {
    const auto t = tokenize("The a I of and x marathon");
    CHECK_EQ(t.size(), 1u);
    if (!t.empty()) CHECK_EQ(t[0], std::string("marathon"));
    CHECK(tokenize("").empty());
    CHECK(tokenize("   ...  !!! ").empty());
}

// ---------------------------------------------------------------- context index

static std::vector<Ad> sample_ads() {
    return {
        {1, 1, 5.0, {"running shoes", "marathon"}, "shoes"},
        {2, 1, 5.0, {"credit card", "cashback"}, "card"},
        {3, 2, 5.0, {"hotels", "beach", "vacation"}, "hotel"},
    };
}

TEST(index_ranks_the_topically_matching_ad_first) {
    ContextIndex index(sample_ads());
    const auto page = index.vectorize("Training plan for your first marathon: pick running shoes");
    const auto top = index.top_k(page, 3, 0.01f);
    CHECK(!top.empty());
    if (!top.empty()) CHECK_EQ(top[0].ad_index, 0u);
    for (const auto& c : top) CHECK(c.relevance > 0.0f && c.relevance <= 1.0001f);
}

TEST(index_returns_nothing_for_an_unrelated_page) {
    ContextIndex index(sample_ads());
    const auto page = index.vectorize("quantum chromodynamics lecture notes");
    CHECK(page.empty());  // no vocabulary overlap at all
    CHECK(index.top_k(page, 5, 0.0f).empty());
}

TEST(index_identical_text_has_cosine_similarity_one) {
    ContextIndex index(sample_ads());
    const auto page = index.vectorize("credit card cashback");
    const auto top = index.top_k(page, 1, 0.0f);
    CHECK_EQ(top.size(), 1u);
    if (!top.empty()) CHECK_NEAR(top[0].relevance, 1.0, 1e-5);
}

// Differential test: the optimised inverted-index path must return exactly what the
// simple brute-force scan returns, on thousands of randomly generated pages.
TEST(inverted_index_matches_brute_force_oracle) {
    synthetic::Config cfg;
    cfg.num_ads = 2'000;
    cfg.num_campaigns = 50;
    cfg.num_topics = 40;
    cfg.num_pages = 1'000;
    cfg.words_per_page = 120;
    const auto w = synthetic::generate(cfg);
    ContextIndex index(w.ads);

    int mismatches = 0;
    for (const auto& p : w.pages) {
        const auto vec = index.vectorize(p.text);
        const auto fast = index.top_k(vec, 20, 0.02f);
        const auto slow = index.top_k_brute_force(vec, 20, 0.02f);
        if (fast.size() != slow.size()) {
            ++mismatches;
            continue;
        }
        for (std::size_t i = 0; i < fast.size(); ++i) {
            if (fast[i].ad_index != slow[i].ad_index || fast[i].relevance != slow[i].relevance) {
                ++mismatches;
                break;
            }
        }
    }
    CHECK_EQ(mismatches, 0);
}

TEST(top_k_respects_k_and_min_relevance) {
    ContextIndex index(sample_ads());
    const auto page = index.vectorize("marathon running shoes hotels beach cashback");
    CHECK_EQ(index.top_k(page, 2, 0.0f).size(), 2u);
    CHECK_EQ(index.top_k(page, 0, 0.0f).size(), 0u);
    CHECK(index.top_k(page, 10, 0.99f).empty());
}

// ---------------------------------------------------------------- LRU cache

TEST(lru_evicts_least_recently_used) {
    ShardedLruCache<std::string, int> cache(2, 1);  // one shard => exact LRU
    cache.put("a", 1);
    cache.put("b", 2);
    CHECK(cache.get("a").has_value());  // "a" is now most recent
    cache.put("c", 3);                  // evicts "b"
    CHECK(!cache.get("b").has_value());
    CHECK_EQ(cache.get("a").value_or(-1), 1);
    CHECK_EQ(cache.get("c").value_or(-1), 3);
    CHECK_EQ(cache.size(), 2u);
}

TEST(lru_put_existing_key_updates_value_without_growing) {
    ShardedLruCache<std::string, int> cache(2, 1);
    cache.put("a", 1);
    cache.put("a", 10);
    CHECK_EQ(cache.size(), 1u);
    CHECK_EQ(cache.get("a").value_or(-1), 10);
}

TEST(lru_is_safe_under_concurrent_access) {
    ShardedLruCache<int, int> cache(1'000, 8);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 20'000; ++i) {
                const int key = (i * 7 + t) % 3'000;
                if (auto v = cache.get(key)) {
                    if (*v != key * 2) std::abort();  // a torn/mixed-up value would be a bug
                } else {
                    cache.put(key, key * 2);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(cache.size() <= 1'000u + 8u);  // per-shard capacity is rounded up
}

// ---------------------------------------------------------------- auction

TEST(auction_winner_pays_just_enough_to_beat_runner_up) {
    // scores: A = 10*0.5 = 5.0, B = 8*0.5 = 4.0  -> A wins, pays 4.0/0.5 = 8.0
    const auto r = run_second_price_auction({{0, 10.0, 0.5}, {1, 8.0, 0.5}}, 0.0);
    CHECK(r.has_winner);
    CHECK_EQ(r.winner, 0u);
    CHECK_NEAR(r.price_cpm, 8.0, 1e-9);
}

TEST(auction_quality_can_beat_a_higher_bid) {
    // A: 10 * 0.2 = 2.0, B: 5 * 0.8 = 4.0 -> B wins, pays 2.0 / 0.8 = 2.5 (less than its bid)
    const auto r = run_second_price_auction({{0, 10.0, 0.2}, {1, 5.0, 0.8}}, 0.0);
    CHECK_EQ(r.winner, 1u);
    CHECK_NEAR(r.price_cpm, 2.5, 1e-9);
}

TEST(auction_single_bidder_pays_floor) {
    const auto r = run_second_price_auction({{0, 6.0, 0.9}}, 1.5);
    CHECK(r.has_winner);
    CHECK_NEAR(r.price_cpm, 1.5, 1e-9);
}

TEST(auction_excludes_bids_below_floor_and_handles_empty) {
    CHECK(!run_second_price_auction({}, 0.0).has_winner);
    CHECK(!run_second_price_auction({{0, 1.0, 0.9}}, 2.0).has_winner);
    const auto r = run_second_price_auction({{0, 1.0, 0.9}, {1, 3.0, 0.1}}, 2.0);
    CHECK_EQ(r.winner, 1u);
}

TEST(auction_price_never_exceeds_winning_bid) {
    // runner_up_score <= winner_score = bid * quality, so runner_up_score / quality <= bid;
    // the clamp in the implementation only guards against floating-point rounding.
    const auto r = run_second_price_auction({{0, 5.0, 1.0}, {1, 10.0, 0.45}}, 0.0);
    CHECK_EQ(r.winner, 0u);
    CHECK_NEAR(r.price_cpm, 4.5, 1e-9);
    CHECK(r.price_cpm <= 5.0);
    // Equal scores: the earlier bid wins and pays its own bid.
    const auto tie = run_second_price_auction({{0, 4.0, 0.5}, {1, 4.0, 0.5}}, 0.0);
    CHECK_EQ(tie.winner, 0u);
    CHECK_NEAR(tie.price_cpm, 4.0, 1e-9);
}

// ---------------------------------------------------------------- budget

TEST(budget_blocks_spend_past_the_limit) {
    BudgetLedger ledger({{7, 1'000}});
    CHECK(ledger.try_spend(7, 600));
    CHECK(!ledger.try_spend(7, 500));  // would reach 1100
    CHECK(ledger.try_spend(7, 400));   // exactly 1000 is allowed
    CHECK_EQ(ledger.remaining(7), 0);
    CHECK(!ledger.try_spend(99, 1));   // unknown campaign
    ledger.refund(7, 400);
    CHECK_EQ(ledger.spent(7), 600);
}

TEST(budget_is_never_overspent_under_contention) {
    const Micros budget = 100'000;
    BudgetLedger ledger({{1, budget}});
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50'000; ++i) {
                if (ledger.try_spend(1, 7)) successes.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    // 8 threads x 50k attempts of 7 micros each far exceeds the budget; exactly
    // floor(100000 / 7) = 14285 charges may succeed, never one more.
    CHECK_EQ(successes.load(), 14'285);
    CHECK_EQ(ledger.spent(1), 14'285 * 7);
    CHECK(ledger.spent(1) <= budget);
}

// ---------------------------------------------------------------- frequency cap

TEST(frequency_cap_blocks_after_limit_and_resets_after_window) {
    FrequencyCapper caps(2, 1'000, 4);
    CHECK(caps.try_record("u1", 5, 0));
    CHECK(caps.try_record("u1", 5, 100));
    CHECK(!caps.allowed("u1", 5, 200));
    CHECK(!caps.try_record("u1", 5, 200));
    CHECK(caps.allowed("u1", 6, 200));    // other campaign unaffected
    CHECK(caps.allowed("u2", 5, 200));    // other user unaffected
    CHECK(caps.try_record("u1", 5, 1'000));  // window [0, 1000) expired
}

TEST(frequency_cap_is_exact_under_contention) {
    FrequencyCapper caps(100, 1'000'000, 16);
    std::atomic<int> recorded{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1'000; ++i) {
                if (caps.try_record("same-user", 1, 10)) recorded.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(recorded.load(), 100);
}

TEST(frequency_cap_evicts_expired_entries) {
    FrequencyCapper caps(3, 1'000, 4);
    caps.try_record("a", 1, 0);
    caps.try_record("b", 1, 900);
    CHECK_EQ(caps.size(), 2u);
    CHECK_EQ(caps.evict_expired(1'500), 1u);  // only "a" has expired
    CHECK_EQ(caps.size(), 1u);
}

// ---------------------------------------------------------------- CTR estimator

TEST(ctr_starts_at_prior_and_learns_from_feedback) {
    CtrEstimator ctr(2, 0.01, 200.0);
    CHECK_NEAR(ctr.predicted_ctr(0), 0.01, 1e-12);
    CHECK_NEAR(ctr.quality_factor(0), 1.0, 1e-12);

    for (int i = 0; i < 10'000; ++i) ctr.record_impression(0);
    for (int i = 0; i < 500; ++i) ctr.record_click(0);  // observed CTR 5%
    // (500 + 2) / (10000 + 200) = 0.04922
    CHECK_NEAR(ctr.predicted_ctr(0), 502.0 / 10'200.0, 1e-12);
    CHECK(ctr.quality_factor(0) > 4.0);

    // One lucky click on a new ad barely moves it: (1 + 2) / (1 + 200) ~ 1.5%, not 100%.
    ctr.record_impression(1);
    ctr.record_click(1);
    CHECK(ctr.predicted_ctr(1) < 0.02);
}

// ---------------------------------------------------------------- metrics

TEST(latency_histogram_percentiles) {
    LatencyHistogram h;
    for (int i = 0; i < 98; ++i) h.record(8);  // falls in the <=10us bucket
    h.record(150);                             // <=200us
    h.record(3'000);                           // <=5000us
    CHECK_EQ(h.count(), 100u);
    CHECK_EQ(h.percentile_us(50), 10);
    CHECK_EQ(h.percentile_us(99), 200);
    CHECK_EQ(h.percentile_us(100), 5'000);
}

// ---------------------------------------------------------------- inventory loader

TEST(loader_parses_valid_files_and_rejects_bad_rows) {
    const std::string campaigns_path = "test_campaigns.csv";
    const std::string ads_path = "test_ads.csv";
    {
        std::ofstream c(campaigns_path);
        c << "campaign_id,daily_budget\n# comment\n1,12.50\r\n";
        std::ofstream a(ads_path);
        a << "ad_id,campaign_id,bid_cpm,keywords,creative\n"
          << "10,1,3.25,running shoes; marathon ,Shoes, now 20% off\n";
    }
    const auto campaigns = load_campaigns(campaigns_path);
    CHECK_EQ(campaigns.size(), 1u);
    if (!campaigns.empty()) CHECK_EQ(campaigns[0].daily_budget, 12'500'000);

    const auto ads = load_ads(ads_path);
    CHECK_EQ(ads.size(), 1u);
    if (!ads.empty()) {
        CHECK_EQ(ads[0].keywords.size(), 2u);
        CHECK_EQ(ads[0].creative, std::string("Shoes, now 20% off"));
    }

    {
        std::ofstream a(ads_path);
        a << "10,1,not-a-number,shoes,x\n";
    }
    bool threw = false;
    try {
        load_ads(ads_path);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find(":1:") != std::string::npos;  // reports line number
    }
    CHECK(threw);
    std::remove(campaigns_path.c_str());
    std::remove(ads_path.c_str());
}

// Ids are uint32. std::stoul returns unsigned long, which is 64-bit on Linux and
// 32-bit on Windows, so before the range check this row loaded as ad id 1 on
// Linux and was rejected on Windows -- the same file, two different inventories.
TEST(loader_rejects_an_id_too_large_for_32_bits) {
    const std::string ads_path = "test_ads_big_id.csv";
    {
        std::ofstream a(ads_path);
        a << "4294967297,1,1.00,shoes,X\n";  // 2^32 + 1, truncates to 1
    }
    bool threw = false;
    try {
        load_ads(ads_path);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find(":1:") != std::string::npos;
    }
    CHECK(threw);
    std::remove(ads_path.c_str());
}

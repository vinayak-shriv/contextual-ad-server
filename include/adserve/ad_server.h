#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adserve/budget.h"
#include "adserve/context_index.h"
#include "adserve/ctr_estimator.h"
#include "adserve/frequency_cap.h"
#include "adserve/lru_cache.h"
#include "adserve/metrics.h"
#include "adserve/types.h"

namespace adserve {

enum class MatchMode { InvertedIndex, BruteForce };

struct ServerConfig {
    std::size_t top_k = 20;             // candidates passed from matching to the auction
    float min_relevance = 0.05f;        // below this cosine similarity an ad is "unrelated"
    bool use_context_cache = true;
    std::size_t cache_capacity = 10'000;
    std::size_t cache_shards = 16;
    int freq_cap = 3;                   // impressions per user per campaign per window
    std::int64_t freq_window_ms = 60 * 60 * 1000;
    std::size_t freq_shards = 16;
    MatchMode match_mode = MatchMode::InvertedIndex;
};

// Serves one ad per request. Pipeline:
//
//   page context (cache or TF-IDF) -> top-K relevant ads -> filter (floor, frequency cap,
//   budget) -> quality-weighted second-price auction -> charge budget -> record impression
//
// Thread-safety: serve() and record_click() may be called concurrently from any number
// of threads. Read-only structures (inventory, index) are immutable after construction;
// mutable state uses sharded locks (cache, frequency caps) or atomics (budgets, CTR,
// metrics).
class AdServer {
public:
    AdServer(std::vector<Ad> ads, const std::vector<Campaign>& campaigns, ServerConfig config);

    AdResponse serve(const AdRequest& request);

    // Click feedback for the CTR model. Returns false for an unknown ad id.
    bool record_click(AdId ad_id);

    StatsSnapshot stats() const { return metrics_.snapshot(); }
    const Ad* find_ad(AdId ad_id) const;
    const BudgetLedger& budgets() const { return budgets_; }
    const CtrEstimator& ctr() const { return ctr_; }
    const ServerConfig& config() const { return config_; }

private:
    std::shared_ptr<const SparseVector> page_context(const AdRequest& request, bool& cache_hit);

    const ServerConfig config_;
    const std::vector<Ad> ads_;
    std::unordered_map<AdId, std::size_t> ad_position_;
    const ContextIndex index_;
    ShardedLruCache<std::string, std::shared_ptr<const SparseVector>> context_cache_;
    BudgetLedger budgets_;
    FrequencyCapper freq_caps_;
    CtrEstimator ctr_;
    Metrics metrics_;
};

}  // namespace adserve

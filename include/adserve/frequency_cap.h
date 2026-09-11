#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adserve/types.h"

namespace adserve {

// Limits how many times one user sees the same campaign within a time window
// (e.g. at most 3 impressions per hour), so users are not shown the same ad endlessly.
//
// Uses a fixed-window counter per (user, campaign): O(1) memory per pair. The trade-off
// versus a sliding window (storing every timestamp) is a boundary effect: a user can
// see up to 2x the cap across the edge of two windows.
//
// Concurrency: the key space is split across shards, each guarded by its own mutex
// (lock striping), so requests for different users rarely contend.
class FrequencyCapper {
public:
    FrequencyCapper(int max_impressions, std::int64_t window_ms, std::size_t num_shards = 16);

    // Read-only check used to filter candidates before the auction.
    bool allowed(const std::string& user_id, CampaignId campaign, std::int64_t now_ms) const;

    // Authoritative check-and-increment under the shard lock. Returns false if the cap
    // was reached (possibly by a concurrent request since allowed() was called).
    bool try_record(const std::string& user_id, CampaignId campaign, std::int64_t now_ms);

    // Removes entries whose window has expired; bounds memory for inactive users.
    std::size_t evict_expired(std::int64_t now_ms);

    std::size_t size() const;

private:
    struct Entry {
        std::int64_t window_start = 0;
        int count = 0;
    };
    struct Shard {
        mutable std::mutex mu;
        std::unordered_map<std::string, Entry> entries;
    };

    static std::string make_key(const std::string& user_id, CampaignId campaign);
    Shard& shard_for(const std::string& key) const;

    const int max_impressions_;
    const std::int64_t window_ms_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace adserve

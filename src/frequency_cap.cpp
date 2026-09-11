#include "adserve/frequency_cap.h"

#include <functional>

namespace adserve {

FrequencyCapper::FrequencyCapper(int max_impressions, std::int64_t window_ms,
                                 std::size_t num_shards)
    : max_impressions_(max_impressions), window_ms_(window_ms) {
    if (num_shards == 0) num_shards = 1;
    shards_.reserve(num_shards);
    for (std::size_t i = 0; i < num_shards; ++i) shards_.push_back(std::make_unique<Shard>());
}

std::string FrequencyCapper::make_key(const std::string& user_id, CampaignId campaign) {
    return user_id + '#' + std::to_string(campaign);
}

FrequencyCapper::Shard& FrequencyCapper::shard_for(const std::string& key) const {
    return *shards_[std::hash<std::string>{}(key) % shards_.size()];
}

bool FrequencyCapper::allowed(const std::string& user_id, CampaignId campaign,
                              std::int64_t now_ms) const {
    const std::string key = make_key(user_id, campaign);
    Shard& s = shard_for(key);
    std::lock_guard<std::mutex> lock(s.mu);
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return true;
    if (now_ms - it->second.window_start >= window_ms_) return true;  // window expired
    return it->second.count < max_impressions_;
}

bool FrequencyCapper::try_record(const std::string& user_id, CampaignId campaign,
                                 std::int64_t now_ms) {
    const std::string key = make_key(user_id, campaign);
    Shard& s = shard_for(key);
    std::lock_guard<std::mutex> lock(s.mu);
    Entry& e = s.entries[key];  // value-initialised {0, 0} if new
    if (e.count == 0 || now_ms - e.window_start >= window_ms_) {
        e.window_start = now_ms;  // start a fresh window
        e.count = 0;
    }
    if (e.count >= max_impressions_) return false;
    ++e.count;
    return true;
}

std::size_t FrequencyCapper::evict_expired(std::int64_t now_ms) {
    std::size_t removed = 0;
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s->mu);
        for (auto it = s->entries.begin(); it != s->entries.end();) {
            if (now_ms - it->second.window_start >= window_ms_) {
                it = s->entries.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    return removed;
}

std::size_t FrequencyCapper::size() const {
    std::size_t total = 0;
    for (const auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s->mu);
        total += s->entries.size();
    }
    return total;
}

}  // namespace adserve

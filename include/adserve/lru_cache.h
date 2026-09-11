#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adserve {

// Thread-safe LRU cache with O(1) get/put.
//
// Classic design: a doubly linked list keeps entries in recency order (front = most
// recent) and a hash map points each key at its list node, so a hit can be moved to
// the front with list::splice in O(1) without invalidating iterators.
//
// Concurrency: one mutex around the whole cache would serialise every request.
// Instead the key space is split across N independent shards (lock striping), each
// with its own list, map and mutex. Requests for different shards never contend.
// The trade-off: eviction is LRU *per shard*, not globally exact.
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedLruCache {
public:
    ShardedLruCache(std::size_t capacity, std::size_t num_shards) {
        if (num_shards == 0) num_shards = 1;
        const std::size_t per_shard = (capacity + num_shards - 1) / num_shards;  // ceil
        shards_.reserve(num_shards);
        for (std::size_t i = 0; i < num_shards; ++i) {
            shards_.push_back(std::make_unique<Shard>(per_shard));
        }
    }

    std::optional<Value> get(const Key& key) {
        Shard& s = shard_for(key);
        std::lock_guard<std::mutex> lock(s.mu);
        auto it = s.index.find(key);
        if (it == s.index.end()) return std::nullopt;
        s.items.splice(s.items.begin(), s.items, it->second);  // mark most recently used
        return it->second->second;
    }

    void put(const Key& key, Value value) {
        Shard& s = shard_for(key);
        if (s.capacity == 0) return;
        std::lock_guard<std::mutex> lock(s.mu);
        auto it = s.index.find(key);
        if (it != s.index.end()) {
            it->second->second = std::move(value);
            s.items.splice(s.items.begin(), s.items, it->second);
            return;
        }
        if (s.items.size() >= s.capacity) {
            s.index.erase(s.items.back().first);  // evict least recently used
            s.items.pop_back();
        }
        s.items.emplace_front(key, std::move(value));
        s.index.emplace(key, s.items.begin());
    }

    std::size_t size() const {
        std::size_t total = 0;
        for (const auto& s : shards_) {
            std::lock_guard<std::mutex> lock(s->mu);
            total += s->items.size();
        }
        return total;
    }

private:
    struct Shard {
        explicit Shard(std::size_t cap) : capacity(cap) {}
        mutable std::mutex mu;
        std::size_t capacity;
        std::list<std::pair<Key, Value>> items;
        std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator, Hash> index;
    };

    Shard& shard_for(const Key& key) { return *shards_[Hash{}(key) % shards_.size()]; }

    // unique_ptr because std::mutex is neither copyable nor movable.
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace adserve

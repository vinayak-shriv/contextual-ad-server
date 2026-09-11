#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adserve {

using AdId = std::uint32_t;
using CampaignId = std::uint32_t;

// Money is stored as integer micros (1 currency unit = 1,000,000 micros).
// Summing many tiny per-impression charges in floating point drifts;
// integers don't, and they let us use a lock-free atomic counter for spend.
using Micros = std::int64_t;
constexpr Micros kMicrosPerUnit = 1'000'000;

// CPM = price per 1000 impressions, so one impression costs cpm / 1000.
inline Micros cpm_to_micros_per_impression(double cpm) {
    return static_cast<Micros>(cpm * static_cast<double>(kMicrosPerUnit) / 1000.0 + 0.5);
}

struct Ad {
    AdId id = 0;
    CampaignId campaign = 0;
    double bid_cpm = 0.0;               // max price the advertiser pays per 1000 impressions
    std::vector<std::string> keywords;  // contextual targeting keywords (may be multi-word)
    std::string creative;               // what gets rendered (title / landing URL)
};

struct Campaign {
    CampaignId id = 0;
    Micros daily_budget = 0;
};

struct AdRequest {
    std::string user_id;
    std::string page_url;    // identifies the page; used as the context-cache key
    std::string page_text;   // page content used for contextual matching
    double floor_cpm = 0.0;  // publisher's reserve price
    std::int64_t now_ms = 0; // request time, injected so tests can control the clock
};

enum class NoFillReason {
    None,
    NoRelevantAds,      // nothing in inventory matched the page context
    BelowFloor,         // matches existed but none bid at or above the floor
    CappedOrNoBudget,   // matches existed but were frequency-capped or out of budget
};

const char* to_string(NoFillReason reason);

struct AdResponse {
    bool filled = false;
    AdId ad_id = 0;
    CampaignId campaign = 0;
    double price_cpm = 0.0;   // clearing price (what the advertiser actually pays)
    double relevance = 0.0;   // cosine similarity between page and ad
    NoFillReason reason = NoFillReason::None;
    bool cache_hit = false;
    std::int64_t latency_us = 0;
};

}  // namespace adserve

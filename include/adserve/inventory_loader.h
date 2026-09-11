#pragma once

#include <string>
#include <vector>

#include "adserve/types.h"

namespace adserve {

// campaigns.csv:  campaign_id,daily_budget          (budget in currency units, e.g. 50.00)
// ads.csv:        ad_id,campaign_id,bid_cpm,keywords,creative
//                 keywords are ';'-separated and may contain spaces ("running shoes;marathon").
// Lines starting with '#' and the header row are skipped.
// Throws std::runtime_error with the file name and line number on malformed input.
std::vector<Campaign> load_campaigns(const std::string& path);
std::vector<Ad> load_ads(const std::string& path);

}  // namespace adserve

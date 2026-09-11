#pragma once

#include <cstddef>
#include <vector>

namespace adserve {

struct Bid {
    std::size_t ad_index = 0;
    double bid_cpm = 0.0;  // advertiser's maximum price
    double quality = 0.0;  // relevance x normalised predicted CTR; > 0
};

struct AuctionResult {
    bool has_winner = false;
    std::size_t winner = 0;  // position of the winning bid in the input vector
    double price_cpm = 0.0;  // clearing price
};

// Quality-weighted second-price auction (the "ad rank" model used by search ads).
//
//   rank score = bid x quality
//   winner     = highest rank score among bids >= floor
//   price      = the smallest bid that would still have beaten the runner-up
//              = runner_up_score / winner_quality, clamped to [floor, winner_bid]
//
// Paying the runner-up's price rather than your own bid makes bidding your true value
// the best strategy, and the quality weighting stops a high bid from buying a slot on
// a page it is irrelevant to.
AuctionResult run_second_price_auction(const std::vector<Bid>& bids, double floor_cpm);

}  // namespace adserve

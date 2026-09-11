#include "adserve/auction.h"

#include <algorithm>

namespace adserve {

AuctionResult run_second_price_auction(const std::vector<Bid>& bids, double floor_cpm) {
    AuctionResult result;

    // Single pass tracking the best and second-best eligible rank scores: O(n), no sort.
    // Ties keep the earlier bid, which makes the outcome deterministic.
    double best_score = -1.0;
    double second_score = -1.0;
    std::size_t best = 0;

    for (std::size_t i = 0; i < bids.size(); ++i) {
        const Bid& b = bids[i];
        if (b.bid_cpm < floor_cpm || b.quality <= 0.0) continue;  // not eligible
        const double score = b.bid_cpm * b.quality;
        if (score > best_score) {
            second_score = best_score;
            best_score = score;
            best = i;
        } else if (score > second_score) {
            second_score = score;
        }
    }

    if (best_score < 0.0) return result;  // no eligible bids

    const Bid& winner = bids[best];
    double price = floor_cpm;  // lone bidder pays the reserve
    if (second_score >= 0.0) {
        price = std::max(floor_cpm, second_score / winner.quality);
    }
    price = std::min(price, winner.bid_cpm);  // never charge more than the advertiser bid

    result.has_winner = true;
    result.winner = best;
    result.price_cpm = price;
    return result;
}

}  // namespace adserve

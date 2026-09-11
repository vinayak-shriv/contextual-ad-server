#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "adserve/types.h"

namespace adserve {

// Tracks each campaign's spend against its daily budget. Safe to call from many
// request threads at once.
//
// The campaign map is built once in the constructor and never modified, so lookups
// need no lock. Each campaign's spend is a single std::atomic counter updated with a
// compare-and-swap loop, which guarantees the budget is never overspent even when
// several threads try to charge the same campaign at the same instant.
class BudgetLedger {
public:
    explicit BudgetLedger(const std::vector<Campaign>& campaigns);

    // Cheap pre-check used to filter auction candidates. May race with other threads,
    // so the authoritative check is try_spend().
    bool can_afford(CampaignId campaign, Micros amount) const;

    // Atomically reserves `amount` if it fits in the remaining budget. Returns false
    // (and charges nothing) otherwise.
    bool try_spend(CampaignId campaign, Micros amount);

    // Returns a previously reserved amount (used when a later step of serving fails).
    void refund(CampaignId campaign, Micros amount);

    Micros spent(CampaignId campaign) const;
    Micros remaining(CampaignId campaign) const;

    // Start of a new budget day.
    void reset_all();

private:
    struct Account {
        explicit Account(Micros b) : budget(b) {}
        const Micros budget;
        std::atomic<Micros> spent{0};
    };

    const Account* find(CampaignId campaign) const;
    Account* find(CampaignId campaign);

    std::unordered_map<CampaignId, std::unique_ptr<Account>> accounts_;
};

}  // namespace adserve

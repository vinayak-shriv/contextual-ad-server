#include "adserve/budget.h"

namespace adserve {

BudgetLedger::BudgetLedger(const std::vector<Campaign>& campaigns) {
    for (const auto& c : campaigns) {
        accounts_[c.id] = std::make_unique<Account>(c.daily_budget);
    }
}

const BudgetLedger::Account* BudgetLedger::find(CampaignId campaign) const {
    auto it = accounts_.find(campaign);
    return it == accounts_.end() ? nullptr : it->second.get();
}

BudgetLedger::Account* BudgetLedger::find(CampaignId campaign) {
    auto it = accounts_.find(campaign);
    return it == accounts_.end() ? nullptr : it->second.get();
}

bool BudgetLedger::can_afford(CampaignId campaign, Micros amount) const {
    const Account* acc = find(campaign);
    if (acc == nullptr) return false;
    return acc->spent.load(std::memory_order_relaxed) + amount <= acc->budget;
}

bool BudgetLedger::try_spend(CampaignId campaign, Micros amount) {
    Account* acc = find(campaign);
    if (acc == nullptr || amount < 0) return false;

    // CAS loop: read the current spend, check the new total fits, and publish it only
    // if no other thread changed `spent` in between. If one did, compare_exchange_weak
    // reloads `current` with the fresh value and we re-check. A plain
    // "if (spent + amount <= budget) spent += amount" would let two threads both pass
    // the check and overspend.
    //
    // Relaxed ordering is enough: the counter is the only shared state involved, and
    // atomicity of the read-modify-write is what prevents overspend.
    Micros current = acc->spent.load(std::memory_order_relaxed);
    do {
        if (current + amount > acc->budget) return false;
    } while (!acc->spent.compare_exchange_weak(current, current + amount,
                                               std::memory_order_relaxed));
    return true;
}

void BudgetLedger::refund(CampaignId campaign, Micros amount) {
    if (Account* acc = find(campaign)) {
        acc->spent.fetch_sub(amount, std::memory_order_relaxed);
    }
}

Micros BudgetLedger::spent(CampaignId campaign) const {
    const Account* acc = find(campaign);
    return acc == nullptr ? 0 : acc->spent.load(std::memory_order_relaxed);
}

Micros BudgetLedger::remaining(CampaignId campaign) const {
    const Account* acc = find(campaign);
    return acc == nullptr ? 0 : acc->budget - acc->spent.load(std::memory_order_relaxed);
}

void BudgetLedger::reset_all() {
    for (auto& [id, acc] : accounts_) acc->spent.store(0, std::memory_order_relaxed);
}

}  // namespace adserve

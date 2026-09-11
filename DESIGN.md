# Design notes

Why each component is built the way it is, what the alternatives were, and what I would
change at production scale.

## 1. Request pipeline

`AdServer::serve` runs five steps: page context -> candidate retrieval -> eligibility
filters -> auction -> commit. Retrieval is deliberately separated from the auction: the
index only answers "which ads are *relevant*", and the auction answers "which relevant ad
*wins* and at what price". This keeps each piece independently testable, and lets the
expensive filters (budget, frequency cap) run on at most `top_k` = 20 candidates instead of
the whole inventory.

## 2. Contextual matching: TF-IDF + cosine similarity

Each ad's keywords and each page's text become TF-IDF vectors, L2-normalised so that their
dot product is the cosine similarity.

- **Sublinear TF** (`1 + ln tf`): a word appearing 10 times is more relevant than once, but
  not 10x more, so long repetitive pages don't dominate.
- **Smoothed IDF** (`ln((1+N)/(1+df)) + 1`): rare terms like "trekking" outweigh common ones
  like "sale", and the smoothing avoids division by zero.
- **Page terms outside the ad vocabulary are dropped** at vectorisation time; they can never
  contribute to any score.

*Alternative considered:* dense sentence embeddings. Better at synonyms, but they need a
model at serving time and approximate nearest-neighbour search, which is harder to make
exact and to test. TF-IDF is fully explainable, deterministic, and fast enough to show the
systems trade-offs, which are the focus here.

## 3. Retrieval: inverted index vs brute force

Brute force computes a dot product with every ad: O(ads x terms). The inverted index maps
each term to the ads that use it (a *posting list*); a query walks only the posting lists
of the page's terms and accumulates partial dot products. Ads sharing no term with the page
are never touched.

Implementation details:

- Scores accumulate in a dense `thread_local` array indexed by ad position, plus a list of
  touched positions to reset afterwards. `thread_local` means concurrent requests never
  share the buffer and nothing is allocated per request.
- **Top-K uses a size-K min-heap**: O(n log K) instead of sorting all n scores (O(n log n)).
- Ties break on lower ad index, so results are deterministic.
- Both paths add contributions in the same (ascending term id) order, so their floating
  point scores are bit-identical. That is what allows the differential test to demand an
  *exact* match between the two paths, not an approximate one.

Measured: 55x faster at 1k ads, 268x at 10k, 515x at 50k. The gap widens because brute
force scales with inventory size while the index scales with posting-list length.

## 4. Page-context cache: sharded LRU

Vectorising a 300-word page (tokenise, hash, weight, normalise) is the most expensive step
of a request. Traffic is highly skewed (a few popular pages get most requests), so the
page vector is cached by URL.

- **O(1) LRU:** a doubly linked list in recency order plus a hash map from key to list node.
  A hit is moved to the front with `list::splice`, which relinks nodes without copying and
  without invalidating iterators.
- **Lock striping:** one mutex around one cache would serialise every request. The key
  space is split across 16 shards, each with its own list, map and mutex. Trade-off:
  eviction is LRU *per shard*, not globally exact.
- **`shared_ptr<const SparseVector>`** as the value: a request keeps using its vector even if
  another thread evicts that entry mid-request.
- **Known gap:** no TTL, so page edits are not seen until eviction. A production version
  would store an insertion time and treat old entries as misses.

Measured: 80% hit rate under Zipf traffic; mean latency 70.7 -> 26.7 us, p50 67.9 -> 15.0 us.
p99 improves less (113.5 -> 97.6 us) because the tail is dominated by cache misses.

## 5. Auction: quality-weighted second price

`rank = bid x quality`, where `quality = relevance x (predicted CTR / prior CTR)`.
The winner pays the smallest bid that would still have beaten the runner-up:
`price = runner_up_rank / winner_quality`, clamped to `[floor, winner_bid]`.

- **Why second price:** paying the runner-up's price instead of your own bid means bidding
  your true value is optimal, so advertisers have no incentive to game their bids.
- **Why weight by quality:** without it, the highest bid wins even on an irrelevant page,
  which hurts users and publishers. With it, a relevant ad can beat a higher but less
  relevant bid, *and pay less than its bid* (tested in `auction_quality_can_beat_a_higher_bid`).
- **Why the price can't exceed the bid:** `runner_up_rank <= winner_rank = bid x quality`, so
  `runner_up_rank / quality <= bid`. The clamp only guards against floating-point rounding.
- A lone eligible bidder pays the floor. The winner is found in one O(n) pass tracking the
  best and second-best scores, with no sort.

## 6. Money: integer micros

All spend is `int64` micros (1 unit = 1,000,000 micros). Summing millions of tiny float
charges drifts; integers are exact and let the budget be a single atomic counter.

## 7. Budgets: lock-free compare-and-swap

The campaign map is built once and never modified, so looking up an account needs no
lock. Each account's spend is one `std::atomic<int64_t>`:

```cpp
Micros current = spent.load();
do {
    if (current + amount > budget) return false;
} while (!spent.compare_exchange_weak(current, current + amount));
```

A naive `if (spent + amount <= budget) spent += amount;` is a *check-then-act race*: two
threads can both pass the check before either adds, and together overspend. The CAS loop
makes "check and add" one atomic step: if another thread changed `spent` in between, the
exchange fails, `current` is reloaded with the fresh value, and the check is redone.

- `compare_exchange_weak` may fail spuriously on some CPUs, which is fine inside a loop.
- **Relaxed memory ordering** is sufficient: the counter is the only shared data involved,
  and the atomicity of the read-modify-write is what prevents overspend. No other memory
  is being published through it.
- Verified by mutation: replacing the CAS loop with the naive version makes both
  concurrency tests fail (in one run, 14,292 charges succeeded where at most 14,285 fit).

## 8. Committing a win: reserve, record, compensate

Filters run *before* the auction as cheap pre-checks, but another thread may use up the
winner's budget or frequency-cap slot before we commit. So the commit re-checks
authoritatively:

1. `try_spend` reserves the price (CAS).
2. `try_record` increments the frequency counter under the shard lock.
3. If step 2 fails, `refund` the reservation. On any failure, drop the winner and re-run
   the auction on the remaining bids (whose prices change, correctly, because the
   runner-up set changed).

The concurrency test checks the result: after 24,000 concurrent requests with tiny
budgets, every campaign's ledger spend equals the sum of the prices returned to callers.
A missing refund would break that equality.

## 9. Frequency capping: fixed window, lock-striped

One counter per (user, campaign): `{window_start, count}`. When a request arrives after the
window ends, the counter resets.

- *Alternative:* a sliding window storing every impression timestamp. Exact, but O(cap)
  memory per pair. The fixed window is O(1), at the cost of a boundary effect: a user can
  see up to 2x the cap across the edge of two windows.
- Shards with their own mutexes, as in the cache. A mutex is used here (not atomics)
  because the reset-and-increment touches two fields together.
- `evict_expired` bounds memory for users who stop visiting.
- Time is passed in (`now_ms`) rather than read from the clock, so tests control time
  precisely.

## 10. CTR estimate: Bayesian smoothing

Raw `clicks / impressions` fails for new ads (0/0, or 100% after one lucky click).
A Beta prior acts like `prior_strength` virtual impressions at the average CTR:

```
pCTR = (clicks + alpha) / (impressions + alpha + beta)
alpha = prior_ctr x strength,   beta = (1 - prior_ctr) x strength
```

A new ad starts at the prior (quality factor 1.0) and moves toward its observed rate as
data accumulates. Counters are relaxed atomics: a slightly stale read shifts the estimate
by one event, which doesn't matter for ranking.

## 11. Concurrency model summary

| State | Mutability | Protection |
|---|---|---|
| Inventory, `ContextIndex`, campaign map | immutable after construction | none needed |
| Page cache | mutable | 16 mutex-guarded shards |
| Frequency caps | mutable | 16 mutex-guarded shards |
| Budgets | mutable | per-campaign atomic + CAS |
| CTR counters, metrics | mutable | relaxed atomics |
| Retrieval scratch buffers | per request | `thread_local` |

## 12. Observability

Counters for requests, fills, cache hits and each no-fill reason, plus a fixed-bucket
latency histogram (one relaxed atomic increment per request). Percentiles are approximate:
they report the upper bound of the bucket. The benchmark computes exact percentiles from
raw samples instead.

## 13. What would change at production scale

- **Distributed budgets:** a central store (e.g. Redis) or per-server budget slices with
  periodic rebalancing, plus *pacing* to spread spend across the day instead of spending
  it all in the morning.
- **Live inventory updates:** build a new index in the background and swap it in atomically
  (`std::atomic<std::shared_ptr>` or RCU), so readers never block.
- **Better relevance:** embeddings with approximate nearest-neighbour search, and a learned
  CTR model using page, ad and context features instead of per-ad counts.
- **Fraud and click validation:** signed impression tokens so `/click` can't be spoofed.
- **Cache TTL** and an admission policy so one-off pages don't evict popular ones.

# Contextual Ad Server

A real-time **contextual ad-serving engine in C++17**. Given a web page, it finds the most
relevant ads, runs a quality-weighted second-price auction, enforces campaign budgets and
per-user frequency caps, and returns a winner. It serves requests in microseconds and is
safe under heavy concurrent load.

Contextual advertising matches ads to the *content of the page* rather than to tracked user
profiles. This project implements that pipeline end to end, focusing on the engineering
problems that make it hard at scale: low latency, thread safety, and correctness under
concurrency.

```
POST /ad?user_id=u1&url=https://blog.example/marathon-tips&floor=1.0
"Training for your first marathon? Choosing the right running shoes ..."

{"filled":true,"ad_id":101,"campaign_id":1,
 "creative":"StrideMax Pro - lightweight running shoes",
 "price_cpm":1,"relevance":0.735523,"cache_hit":false,"latency_us":33}
```

## Highlights

| | |
|---|---|
| **Retrieval** | TF-IDF + cosine similarity over an **inverted index**: 268x faster than a brute-force scan at 10k ads, 515x at 50k |
| **Caching** | Thread-safe **sharded LRU** cache of page vectors: 80% hit rate on Zipf traffic, mean latency 70.7 -> 26.7 us |
| **Auction** | Quality-weighted **second-price** auction (bid x relevance x predicted CTR) with publisher floor prices |
| **Budgets** | **Lock-free** ledger: one atomic compare-and-swap loop per charge, provably no overspend |
| **Frequency caps** | Per (user, campaign) fixed-window counters, **lock-striped** across 16 shards |
| **Learning** | Online **Bayesian-smoothed CTR** estimate updated from click feedback |
| **Correctness** | 32 tests incl. differential tests against a brute-force oracle and multi-threaded invariant tests; clean under ThreadSanitizer, AddressSanitizer and UBSan |

## Architecture

```
                 HTTP (cpp-httplib, thread pool)
                              |
                              v
+-------------------------------------------------------------------+
| AdServer::serve(request)                                          |
|                                                                   |
|  1. Page context   url -> ShardedLruCache --miss--> tokenize +    |
|                                                    TF-IDF vector  |
|  2. Retrieval      ContextIndex::top_k (inverted index, min-heap) |
|  3. Filters        floor price, FrequencyCapper, BudgetLedger     |
|  4. Auction        quality = relevance x CtrEstimator factor      |
|                    rank = bid x quality, price = 2nd price        |
|  5. Commit         BudgetLedger::try_spend (CAS)                  |
|                    -> FrequencyCapper::try_record (shard lock)    |
|                    -> on failure: refund, drop winner, re-run     |
|  6. Metrics        atomic counters + latency histogram            |
+-------------------------------------------------------------------+

 immutable after start:  inventory, ContextIndex        -> lock-free reads
 mutable, sharded locks: LRU cache, frequency caps
 mutable, atomics:       budgets, CTR counters, metrics
```

See [DESIGN.md](DESIGN.md) for the reasoning and trade-offs behind each component.

## Build and run

Requires CMake >= 3.16 and a C++17 compiler (GCC, Clang or MSVC). No external dependencies:
the HTTP library ([cpp-httplib](https://github.com/yhirose/cpp-httplib), MIT) is vendored as
a single header.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/adserve_tests                # run the test suite
./build/adserve_bench                # run the benchmarks (add --quick for a short run)
./build/adserve_server --port 8080   # start the HTTP server with data/ads.csv
```

Sanitizer builds (GCC/Clang):

```bash
cmake -S . -B build-tsan -DADSERVE_SANITIZE=thread -DADSERVE_BUILD_SERVER=OFF
cmake --build build-tsan -j && ./build-tsan/adserve_tests
```

## API

| Endpoint | Description |
|---|---|
| `POST /ad?user_id=..&url=..&floor=..` | Body is the page text (`text/plain`). Returns the winning ad, clearing price and relevance, or a `reason` for no fill: `no_relevant_ads`, `below_floor`, `capped_or_no_budget`. |
| `POST /click?ad_id=..` | Records a click; feeds the CTR model. |
| `GET /stats` | Requests, fill rate, cache hit rate, no-fill breakdown, clicks, p50/p99 latency. |
| `GET /health` | Liveness check. |

```bash
curl -X POST "localhost:8080/ad?user_id=u1&url=https://news.example/sip&floor=2" \
     --data "Should you start a SIP in mutual funds or pick stocks for your portfolio?"
# {"filled":true,"ad_id":302,"creative":"GrowWise - start a SIP in 2 minutes","price_cpm":2,...}

curl localhost:8080/stats
```

Inventory lives in `data/campaigns.csv` (`campaign_id,daily_budget`) and `data/ads.csv`
(`ad_id,campaign_id,bid_cpm,keywords,creative`, keywords `;`-separated). Malformed rows are
rejected with the file name and line number.

## Benchmarks

Synthetic workload (`bench/synthetic.h`, fixed seed): ads grouped into 200 topics, 300-word
pages that are 20% on-topic, and page popularity following a Zipf distribution like real
traffic. Engine only, no HTTP. Measured on a single-core Linux VM (GCC 13, `-O3`); numbers
will differ on other hardware, so run `./build/adserve_bench` to reproduce.

**1. Candidate retrieval (top-20)**

| Ads | Brute force (us/query) | Inverted index (us/query) | Speedup |
|---:|---:|---:|---:|
| 1,000 | 69.4 | 1.3 | 55x |
| 10,000 | 1,487.6 | 5.6 | 268x |
| 50,000 | 8,790.3 | 17.1 | 515x |

Brute force grows with inventory size; the inverted index grows only with the length of
the posting lists for the page's terms.

**2. End-to-end serve latency** (10,000 ads, 200,000 requests over 20,000 pages)

| Context cache | Hit rate | Mean (us) | p50 (us) | p95 (us) | p99 (us) |
|---|---:|---:|---:|---:|---:|
| off | - | 70.7 | 67.9 | 94.4 | 113.5 |
| on | 80.4% | 26.7 | 15.0 | 79.2 | 97.6 |

Tokenising a 300-word page dominates the uncached path; popular pages make caching
the page vector by URL very effective.

**3. Throughput:** about 50,500 requests/second on one core, 81% fill rate.

## Testing

`./build/adserve_tests` runs 32 tests using a small dependency-free harness (`tests/mini_test.h`).

- **Unit tests:** tokenizer, TF-IDF scoring, LRU eviction order, auction pricing edge cases
  (lone bidder, ties, floor, quality beating a higher bid), budget limits, frequency-cap
  windows, CTR smoothing, histogram percentiles, CSV validation.
- **Differential tests:** the inverted-index path must return exactly the same top-K as
  brute force on 1,000 random pages, and both matching modes must make identical serving
  decisions end to end.
- **Concurrency tests:** 8 threads hammer the budget ledger, frequency capper, LRU cache
  and the full server with tiny budgets. After the run: no campaign is over budget, every
  campaign's recorded spend equals the sum of prices returned, and no user saw a campaign
  more than the cap allows.
- **Sanitizers:** CI runs the whole suite under ThreadSanitizer and
  AddressSanitizer + UBSan. As a check that the concurrency tests are meaningful,
  replacing the budget's CAS loop with a naive check-then-add makes them fail with overspend.

## Project layout

```
include/adserve/   public headers, one per component
src/               implementations + HTTP server (main_server.cpp)
tests/             unit, differential and concurrency tests
bench/             benchmark harness and synthetic workload generator
data/              sample campaigns and ads
third_party/       vendored cpp-httplib (MIT)
```

## Limitations and next steps

- Single process. At real scale, budgets would need a shared store with pacing across
  servers, and the ad index would be sharded across machines.
- The index is built once at startup; live inventory updates would need a rebuilt index
  swapped in atomically (double buffering).
- The page cache has no TTL, so edits to a page are not seen until it is evicted.
- TF-IDF matches words, not meaning; sentence embeddings with approximate nearest-neighbour
  search would catch synonyms ("sneakers" vs "running shoes").
- Clicks are trusted as-is; production systems validate them with signed impression tokens.

## How this was built

I used AI coding assistants throughout, to draft components, generate test cases and
review for concurrency bugs. Every design decision, test and benchmark in this repository
I reviewed, ran and can explain; the reasoning is written up in DESIGN.md.

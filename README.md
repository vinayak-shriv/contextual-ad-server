# Contextual Ad Server

[![CI](https://github.com/vinayak-shriv/contextual-ad-server/actions/workflows/ci.yml/badge.svg)](https://github.com/vinayak-shriv/contextual-ad-server/actions/workflows/ci.yml)

A real-time **contextual ad-serving engine in C++17**. Given a web page, it finds the most
relevant ads, runs a quality-weighted second-price auction, enforces campaign budgets and
per-user frequency caps, and returns a winner. It serves requests in microseconds and is
safe under heavy concurrent load.

Contextual advertising matches ads to the *content of the page* rather than to tracked user
profiles. This project implements that pipeline end to end, focusing on the engineering
problems that make it hard at scale: low latency, thread safety, and correctness under
concurrency.

```
POST /ad?user_id=u3&url=https://blog.example/marathon-tips&floor=1.0
"Training for your first marathon? Choosing the right running shoes matters."

{"filled":true,"ad_id":101,"campaign_id":1,
 "creative":"StrideMax Pro - lightweight running shoes",
 "price_cpm":1.04629,"relevance":0.636437,"cache_hit":false,"latency_us":15}
```

(Real output from `data/ads.csv`, not an illustration: send that exact request to a running
server and you get that response back, give or take the latency.)

## Highlights

| | |
|---|---|
| **Retrieval** | TF-IDF + cosine similarity over an **inverted index**: 184x faster than a brute-force scan at 10k ads, 410x at 50k |
| **Caching** | Thread-safe **sharded LRU** cache of page vectors: 80% hit rate on Zipf traffic, mean latency 58.2 -> 24.5 us |
| **Throughput** | 49k requests/s on one thread, 240k/s on 16 (4.9x), engine only |
| **Auction** | Quality-weighted **second-price** auction (bid x relevance x predicted CTR) with publisher floor prices |
| **Budgets** | **Lock-free** ledger: one atomic compare-and-swap loop per charge, provably no overspend |
| **Frequency caps** | Per (user, campaign) fixed-window counters, **lock-striped** across 16 shards |
| **Learning** | Online **Bayesian-smoothed CTR** estimate updated from click feedback |
| **Correctness** | 34 tests incl. differential tests against a brute-force oracle and multi-threaded invariant tests; clean under ThreadSanitizer, AddressSanitizer and UBSan in CI |

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
./build/adserve_tests --list         # list test names
./build/adserve_tests auction budget # run only tests matching these substrings
./build/adserve_bench                # benchmarks (--quick for a short run)
./build/adserve_server --port 8080   # HTTP server, serving data/ads.csv
```

Sanitizer builds (GCC/Clang):

```bash
cmake -S . -B build-tsan -DADSERVE_SANITIZE=thread -DADSERVE_BUILD_SERVER=OFF
cmake --build build-tsan -j && ./build-tsan/adserve_tests
```

Built and tested on Linux (GCC, in CI) and on Windows with MinGW-w64 GCC 16.1. See
[Portability](#portability) for one Windows-specific issue worth knowing about.

## API

| Endpoint | Description |
|---|---|
| `POST /ad?user_id=..&url=..&floor=..` | Body is the page text (`text/plain`). Returns the winning ad, clearing price and relevance, or a `reason` for no fill: `no_relevant_ads`, `below_floor`, `capped_or_no_budget`. A `floor` that is missing is treated as zero; one that is negative or not finite is rejected with 400. |
| `POST /click?ad_id=..` | Records a click; feeds the CTR model. |
| `GET /stats` | Requests, fill rate, cache hit rate, no-fill breakdown, clicks, p50/p99 latency. |
| `GET /health` | Liveness check. |

```bash
curl -X POST "localhost:8080/ad?user_id=u1&url=https://news.example/sip&floor=2" \
     --data "Should you start a SIP in mutual funds or pick stocks for your portfolio?"
# {"filled":true,"ad_id":302,"campaign_id":3,"creative":"GrowWise - start a SIP in 2 minutes",
#  "price_cpm":2,"relevance":0.912871,"cache_hit":false,"latency_us":24}

# The same URL again is a cache hit, and cheaper: "cache_hit":true, latency_us 24 -> 13.

curl -X POST "localhost:8080/ad?user_id=u1&url=https://a.example/b&floor=nan" --data "shoes"
# {"error":"floor must be a finite number >= 0"}   [HTTP 400]

curl localhost:8080/stats
# {"requests":5,"filled":3,"fill_rate":0.6,"cache_hit_rate":0.2,
#  "no_fill":{"no_relevant_ads":1,"below_floor":1,"capped_or_no_budget":0},
#  "clicks":1,"latency_us":{"p50":20,"p99":50}}
```

Inventory lives in `data/campaigns.csv` (`campaign_id,daily_budget`) and `data/ads.csv`
(`ad_id,campaign_id,bid_cpm,keywords,creative`, keywords `;`-separated). Malformed rows are
rejected with the file name and line number.

## Benchmarks

Synthetic workload (`bench/synthetic.h`, fixed seed): ads grouped into 200 topics, 300-word
pages that are 20% on-topic, and page popularity following a Zipf distribution like real
traffic. Engine only, no HTTP.

Measured on an AMD Ryzen 7 4800H (8 cores / 16 threads), Windows 11, GCC 16.1 (MinGW-w64
UCRT), `-O3`. Every figure below is the median of three consecutive runs; run-to-run spread
is a few percent, and wider on the sub-microsecond indexed path, so treat the speedups as
"roughly this order" rather than exact. Run `./build/adserve_bench` to reproduce on your own
hardware — the numbers will differ.

**1. Candidate retrieval (top-20)**

| Ads | Brute force (us/query) | Inverted index (us/query) | Speedup |
|---:|---:|---:|---:|
| 1,000 | 40.8 | 1.1 | 36x |
| 10,000 | 822.7 | 4.4 | 184x |
| 50,000 | 5,772.0 | 14.0 | 410x |

Brute force grows with inventory size; the inverted index grows only with the length of
the posting lists for the page's terms.

**2. End-to-end serve latency** (10,000 ads, 200,000 requests over 20,000 pages, one thread)

| Context cache | Hit rate | Mean (us) | p50 (us) | p95 (us) | p99 (us) |
|---|---:|---:|---:|---:|---:|
| off | - | 58.2 | 56.6 | 70.0 | 91.3 |
| on | 80.4% | 24.5 | 14.6 | 69.2 | 83.9 |

Tokenising a 300-word page dominates the uncached path; popular pages make caching the page
vector by URL very effective. p50 improves far more than p99 because the tail is made of
cache misses, which the cache by definition does not help.

**3. Throughput scaling** (10,000 ads, 100,000 requests per thread)

| Threads | QPS | Speedup | Fill rate | Cache hit rate |
|---:|---:|---:|---:|---:|
| 1 | 48,974 | 1.00x | 100.0% | 86.8% |
| 2 | 88,741 | 1.81x | 100.0% | 88.4% |
| 4 | 158,504 | 3.24x | 100.0% | 89.4% |
| 8 | 214,117 | 4.37x | 100.0% | 90.0% |
| 16 | 239,909 | 4.90x | 100.0% | 90.2% |

Scaling is real but sub-linear, and the shape is worth reading honestly: 3.2x at 4 threads,
4.4x at 8 (the physical core count), and only 4.9x at 16, where the extra threads are SMT
siblings sharing execution units. The remaining gap at 4-8 threads is not lock contention on
any single structure — the shards exist precisely to avoid that — but per-request allocation:
every request builds a page vector, candidate vectors and bid vectors, so the threads
contend in the allocator and on memory bandwidth. An arena per request would be the next
thing to try.

## Testing

`./build/adserve_tests` runs 34 tests using a small dependency-free harness
(`tests/mini_test.h`). Pass substrings to run a subset, `--list` to see the names.

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
- **Sanitizers:** CI runs the whole suite under ThreadSanitizer and AddressSanitizer + UBSan.
- **Tests checked against the unfixed code.** A regression test that passes before the fix
  is proof of nothing. Replacing the budget's CAS loop with a naive check-then-add makes the
  concurrency tests fail with overspend; `server_treats_a_nonsense_floor_as_no_reserve` was
  run against the pre-fix commit in a worktree and fails there.

## Portability

The suite crashed on Windows (MinGW-w64) on every run, with a heap corruption at thread
exit, while being clean on Linux. It came down to registering a destructor per thread for
the retrieval scratch buffers: mingw-w64's TLS teardown corrupts the heap when several
threads exit at once.

The evidence that it was the runtime and not this code:

| Check | Result |
|---|---|
| Every test run individually | passes |
| Whole suite, dynamically linked runtime | crashes 10/10 |
| Whole suite, statically linked runtime (no source change) | crashes 2/10 |
| libstdc++ debug mode (bounds-checked containers, validated iterators) | no violation reported |
| Standalone reproducer: two `thread_local` vectors, 8 threads exiting together | clean |
| Linux CI, ThreadSanitizer and AddressSanitizer + UBSan | clean |

`ContextIndex` therefore keeps a raw pointer in TLS — trivially destructible, so no teardown
callback is registered — with the buffers owned by a process-wide list and freed at exit.
The hot path is unchanged. Windows now passes 15 runs out of 15.

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
- Per-request allocation is what limits multi-core scaling (see the throughput table); an
  arena allocator per request is the obvious next move.
- Clicks are trusted as-is; production systems validate them with signed impression tokens.

## How this was built

I used AI coding assistants throughout — drafting components, generating test cases,
reviewing for concurrency bugs — and treated everything they produced as a proposal to be
verified rather than an answer. Three things from this repository's history that verification
caught:

- **A crash the tests hid.** The suite passed test by test and died with heap corruption when
  run as a whole. Making the harness able to run one test per process, then libstdc++ debug
  mode, a standalone reproducer and a static-runtime link, separated "a bug in this code"
  from "a bug in the runtime"; Linux sanitizers cleared the code before I changed it.
- **A bug no test caught.** `bid < NaN` is false, so `floor=nan` passed every eligibility
  check and reached a NaN-to-integer conversion — undefined behaviour that on x86 quietly
  returned the wrong no-fill reason instead of crashing. Found by reading the filter, fixed
  at both the engine and the HTTP boundary.
- **Numbers that were not measurements.** The benchmark mixed `printf` with `cout`, so
  redirecting it to a file scrambled the tables; and its warm-up used a single user id, which
  tripped the frequency cap and turned the reported fill rate into an artifact of how many
  requests followed it.

Every design decision, test and benchmark here is one I have run and can defend.

// Benchmarks the engine directly (no HTTP) on a synthetic workload.
//
//   1. Candidate retrieval: inverted index vs brute-force scan, across inventory sizes.
//   2. End-to-end serve latency with and without the page-context cache (Zipf traffic).
//   3. Throughput scaling: QPS at 1, 2, 4 ... up to the machine's thread count.
//
// Usage: adserve_bench [--quick] [--threads N]
//
//   --threads N  measure only N threads instead of sweeping the scaling curve.
//
// Everything is written to std::cout. An earlier version mixed std::printf with
// std::cout, and the two buffers reach a pipe in their own order: redirecting the
// output to a file produced tables whose rows arrived before their headers.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "adserve/ad_server.h"
#include "adserve/context_index.h"
#include "synthetic.h"

using Clock = std::chrono::steady_clock;
using namespace adserve;

namespace {

double elapsed_us(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

std::string fixed(double value, int precision) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << value;
    return os.str();
}

struct LatencySummary {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0;
};

LatencySummary summarize(std::vector<double> samples) {
    LatencySummary s;
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (double v : samples) sum += v;
    auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(p / 100.0 * (samples.size() - 1));
        return samples[idx];
    };
    s.mean = sum / samples.size();
    s.p50 = pct(50);
    s.p95 = pct(95);
    s.p99 = pct(99);
    return s;
}

// Keeps the optimiser from deleting work whose result is otherwise unused.
std::atomic<std::size_t> g_sink{0};

void bench_retrieval(bool quick) {
    std::cout << "\n## 1. Candidate retrieval (top-20), single thread\n\n"
              << "| Ads | Brute force (us/query) | Inverted index (us/query) | Speedup |\n"
              << "|---:|---:|---:|---:|\n";

    const std::vector<std::size_t> sizes =
        quick ? std::vector<std::size_t>{1'000, 5'000} : std::vector<std::size_t>{1'000, 10'000, 50'000};
    for (std::size_t n : sizes) {
        synthetic::Config cfg;
        cfg.num_ads = n;
        cfg.num_pages = quick ? 300 : 2'000;
        const auto w = synthetic::generate(cfg);
        ContextIndex index(w.ads);

        std::vector<SparseVector> pages;
        pages.reserve(w.pages.size());
        for (const auto& p : w.pages) pages.push_back(index.vectorize(p.text));

        auto time_per_query = [&](bool brute) {
            const int reps = brute ? 1 : 5;  // more reps for the fast path => stable timing
            const auto start = Clock::now();
            for (int r = 0; r < reps; ++r) {
                for (const auto& v : pages) {
                    const auto res = brute ? index.top_k_brute_force(v, 20, 0.05f)
                                           : index.top_k(v, 20, 0.05f);
                    g_sink += res.size();
                }
            }
            return elapsed_us(start) / (static_cast<double>(pages.size()) * reps);
        };

        const double brute = time_per_query(true);
        const double indexed = time_per_query(false);
        std::cout << "| " << n << " | " << fixed(brute, 1) << " | " << fixed(indexed, 1) << " | "
                  << fixed(brute / indexed, 1) << "x |" << std::endl;
    }
}

void bench_cache(bool quick) {
    synthetic::Config cfg;
    cfg.num_ads = 10'000;
    cfg.num_pages = quick ? 2'000 : 20'000;
    const auto w = synthetic::generate(cfg);
    const std::size_t num_requests = quick ? 20'000 : 200'000;

    std::cout << "\n## 2. End-to-end serve latency, " << cfg.num_ads << " ads, "
              << cfg.words_per_page << "-word pages, Zipf(s=1) traffic over " << cfg.num_pages
              << " pages, " << num_requests << " requests, single thread\n\n"
              << "| Context cache | Hit rate | Mean (us) | p50 (us) | p95 (us) | p99 (us) |\n"
              << "|---|---:|---:|---:|---:|---:|\n";

    for (bool use_cache : {false, true}) {
        ServerConfig sc;
        sc.use_context_cache = use_cache;
        sc.cache_capacity = 5'000;
        AdServer server(w.ads, w.campaigns, sc);
        synthetic::ZipfSampler sampler(w.pages.size(), cfg.zipf_s, 7);

        std::vector<double> latencies;
        latencies.reserve(num_requests);
        for (std::size_t i = 0; i < num_requests; ++i) {
            const auto& p = w.pages[sampler.next()];
            AdRequest req;
            req.user_id = "u" + std::to_string(i % cfg.num_users);
            req.page_url = p.url;
            req.page_text = p.text;
            req.floor_cpm = 0.5;
            const auto start = Clock::now();
            const auto r = server.serve(req);
            latencies.push_back(elapsed_us(start));
            g_sink += r.ad_id;
        }
        const auto s = summarize(latencies);
        std::cout << "| " << (use_cache ? "on" : "off") << " | "
                  << fixed(100.0 * server.stats().cache_hit_rate(), 1) << "% | " << fixed(s.mean, 1)
                  << " | " << fixed(s.p50, 1) << " | " << fixed(s.p95, 1) << " | "
                  << fixed(s.p99, 1) << " |" << std::endl;
    }
}

// One row of the scaling table. A fresh server per row on purpose: frequency-cap
// and budget state left over from a previous row would change the fill rate and
// make the rows incomparable.
double throughput_row(const synthetic::Workload& w, const synthetic::Config& cfg,
                      std::size_t per_thread, unsigned threads, double baseline_qps) {
    AdServer server(w.ads, w.campaigns, ServerConfig{});

    // Warm-up pass so the cache state is realistic rather than cold.
    //
    // Distinct user ids matter here: warming with a single user trips the
    // per-user frequency cap after three impressions, so the rest of the
    // warm-up no-fills and drags the reported fill rate down. That is what
    // made fill rate appear to climb with thread count -- more real requests
    // diluting a fixed block of capped warm-up ones -- which says nothing
    // about concurrency.
    {
        synthetic::ZipfSampler sampler(w.pages.size(), cfg.zipf_s, 99);
        for (std::size_t i = 0; i < per_thread / 4; ++i) {
            const auto& p = w.pages[sampler.next()];
            server.serve({"warm" + std::to_string(i % cfg.num_users), p.url, p.text, 0.5, 0});
        }
    }

    std::vector<std::thread> pool;
    const auto start = Clock::now();
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            synthetic::ZipfSampler sampler(w.pages.size(), cfg.zipf_s, 1'000 + t);
            for (std::size_t i = 0; i < per_thread; ++i) {
                const auto& p = w.pages[sampler.next()];
                AdRequest req{"u" + std::to_string((i * 31 + t) % cfg.num_users), p.url, p.text, 0.5, 0};
                g_sink += server.serve(req).ad_id;
            }
        });
    }
    for (auto& th : pool) th.join();

    const double seconds = elapsed_us(start) / 1e6;
    const double total = static_cast<double>(per_thread) * threads;
    const double qps = total / seconds;
    const auto s = server.stats();

    std::cout << "| " << threads << " | " << fixed(total, 0) << " | " << fixed(qps, 0) << " | ";
    if (baseline_qps > 0.0) {
        std::cout << fixed(qps / baseline_qps, 2) << "x";
    } else {
        std::cout << "1.00x";
    }
    std::cout << " | " << fixed(100.0 * s.fill_rate(), 1) << "% | "
              << fixed(100.0 * s.cache_hit_rate(), 1) << "% |" << std::endl;
    return qps;
}

void bench_throughput(bool quick, const std::vector<unsigned>& thread_counts) {
    synthetic::Config cfg;
    cfg.num_ads = 10'000;
    cfg.num_pages = quick ? 2'000 : 20'000;
    const auto w = synthetic::generate(cfg);
    const std::size_t per_thread = quick ? 10'000 : 100'000;

    std::cout << "\n## 3. Throughput scaling (engine only, no HTTP)\n\n"
              << "| Threads | Requests | QPS | Speedup | Fill rate | Cache hit rate |\n"
              << "|---:|---:|---:|---:|---:|---:|\n";

    double baseline = 0.0;
    for (unsigned threads : thread_counts) {
        const double qps = throughput_row(w, cfg, per_thread, threads, baseline);
        if (baseline == 0.0) baseline = qps;
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    bool threads_given = false;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            quick = true;
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::stoul(argv[++i]));
            threads_given = true;
        } else {
            std::cerr << "usage: adserve_bench [--quick] [--threads N]\n";
            return 2;
        }
    }
    if (threads == 0) threads = 1;

    // Default: sweep 1, 2, 4 ... up to the machine's thread count, so the table
    // shows how throughput scales rather than a single number whose meaning
    // depends on hardware nobody else has. --threads N measures just N.
    std::vector<unsigned> thread_counts;
    if (threads_given) {
        thread_counts.push_back(threads);
    } else {
        for (unsigned t = 1; t < threads; t *= 2) thread_counts.push_back(t);
        thread_counts.push_back(threads);
    }

    std::cout << "# adserve benchmark" << (quick ? " (quick)" : "") << "\n"
              << "hardware threads: " << std::thread::hardware_concurrency() << std::endl;
    bench_retrieval(quick);
    bench_cache(quick);
    bench_throughput(quick, thread_counts);
    return g_sink.load() == 0xFFFFFFFF ? 1 : 0;  // practically never; just consumes the sink
}

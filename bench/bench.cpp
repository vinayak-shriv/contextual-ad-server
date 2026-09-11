// Benchmarks the engine directly (no HTTP) on a synthetic workload.
//
//   1. Candidate retrieval: inverted index vs brute-force scan, across inventory sizes.
//   2. End-to-end serve latency with and without the page-context cache (Zipf traffic).
//   3. Throughput with N worker threads.
//
// Usage: adserve_bench [--quick] [--threads N]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
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
        std::printf("| %zu | %.1f | %.1f | %.1fx |\n", n, brute, indexed, brute / indexed);
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
        std::printf("| %s | %.1f%% | %.1f | %.1f | %.1f | %.1f |\n", use_cache ? "on" : "off",
                    100.0 * server.stats().cache_hit_rate(), s.mean, s.p50, s.p95, s.p99);
    }
}

void bench_throughput(bool quick, unsigned threads) {
    synthetic::Config cfg;
    cfg.num_ads = 10'000;
    cfg.num_pages = quick ? 2'000 : 20'000;
    const auto w = synthetic::generate(cfg);
    const std::size_t per_thread = quick ? 10'000 : 100'000;

    AdServer server(w.ads, w.campaigns, ServerConfig{});

    // Warm-up pass so the cache state is realistic rather than cold.
    {
        synthetic::ZipfSampler sampler(w.pages.size(), cfg.zipf_s, 99);
        for (std::size_t i = 0; i < per_thread / 4; ++i) {
            const auto& p = w.pages[sampler.next()];
            server.serve({"warm", p.url, p.text, 0.5, 0});
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
    const auto s = server.stats();

    std::cout << "\n## 3. Throughput (engine only, no HTTP)\n\n"
              << "| Threads | Requests | QPS | Fill rate | Cache hit rate |\n"
              << "|---:|---:|---:|---:|---:|\n";
    std::printf("| %u | %.0f | %.0f | %.1f%% | %.1f%% |\n", threads, total, total / seconds,
                100.0 * s.fill_rate(), 100.0 * s.cache_hit_rate());
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") quick = true;
        else if (arg == "--threads" && i + 1 < argc) threads = static_cast<unsigned>(std::stoul(argv[++i]));
        else {
            std::cerr << "usage: adserve_bench [--quick] [--threads N]\n";
            return 2;
        }
    }

    std::cout << "# adserve benchmark" << (quick ? " (quick)" : "") << "\n"
              << "hardware threads: " << std::thread::hardware_concurrency() << "\n";
    bench_retrieval(quick);
    bench_cache(quick);
    bench_throughput(quick, threads);
    return g_sink.load() == 0xFFFFFFFF ? 1 : 0;  // practically never; just consumes the sink
}

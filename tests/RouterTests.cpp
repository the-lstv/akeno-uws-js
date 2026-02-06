#include "../src/Router.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <iomanip>

#include <benchmark/benchmark.h>
#include <string>

// Bulid with
// (need to have Google Benchmark installed, on Fedora that is `dnf install google-benchmark google-benchmark-devel`)
// g++ -O3 -DNDEBUG -std=c++20 RouterTests.cpp -lbenchmark -lpthread -o RouterTests

// (!) This is not a final test, just a prototype to verify basic functionality

using namespace Akeno;

// Simple handler for testing
struct TestHandler {
    int id;
    std::string name;

    bool operator==(const TestHandler& other) const {
        return id == other.id && name == other.name;
    }
};

void runTests() {
    std::cout << "Running Tests..." << std::endl;

    {
        // 1. Exact Matches
        PathMatcher<TestHandler> router;
        router.add("/api/v1/users", {1, "users"});
        
        auto* h = router.match("/api/v1/users");
        assert(h != nullptr);
        assert(h->id == 1);

        assert(router.match("/api/v1/user") == nullptr); // Partial
        assert(router.match("/api/v1/users/123") == nullptr); // Too long
        std::cout << "  [PASS] Exact Matches" << std::endl;
    }

    {
        // 2. Expansion Matches {id} -> literal "id"
        PathMatcher<TestHandler> router;
        router.add("/api/v1/users/{id}", {2, "user_id_literal"});

        assert(router.match("/api/v1/users/id") != nullptr);
        assert(router.match("/api/v1/users/123") == nullptr); 
        std::cout << "  [PASS] Expansion Literal Matches" << std::endl;
    }

    {
        // 3. Optional Expansion {a,b} and {,a}
        PathMatcher<TestHandler> router;
        router.add("/{a,b}", {3, "ab"});
        router.add("/opt/{,c}", {4, "opt_c"});

        assert(router.match("/a") != nullptr && router.match("/a")->id == 3);
        assert(router.match("/b") != nullptr && router.match("/b")->id == 3);
        assert(router.match("/c") == nullptr);

        assert(router.match("/opt") != nullptr && router.match("/opt")->id == 4);
        
        assert(router.match("/opt/c") != nullptr && router.match("/opt/c")->id == 4);
        std::cout << "  [PASS] Braced Expansion" << std::endl;
    }

    {
        // 4. Wildcard Expansion {*,}
        PathMatcher<TestHandler> router;
        router.add("/test/{*,}", {5, "wildcard_opt"}); // Expands to "/test/*" and "/test"

        assert(router.match("/test") != nullptr);
        assert(router.match("/test/foo") != nullptr);
        assert(router.match("/test/foo/bar") == nullptr); // * is single segment
        std::cout << "  [PASS] Wildcard Expansion" << std::endl;
    }

    {
        // 5. Strict Single Wildcard *
        PathMatcher<TestHandler> router;
        router.add("/user/*", {6, "user_wildcard"});

        assert(router.match("/user/123") != nullptr);
        assert(router.match("/user/") == nullptr); // * requires a non-empty segment
        assert(router.match("/user") == nullptr);
        assert(router.match("/user/123/profile") == nullptr); // Too deep
        std::cout << "  [PASS] Strict Single Wildcard" << std::endl;
    }

    {
        // 6. Double Wildcard **
        PathMatcher<TestHandler> router;
        router.add("/files/**", {7, "double_wildcard"});

        assert(router.match("/files/") != nullptr);
        assert(router.match("/files/docs/report.pdf") != nullptr);
        assert(router.match("/files") != nullptr); // ** matches zero or more
        std::cout << "  [PASS] Double Wildcard" << std::endl;
    }
    
    {
        // 7. Negated Sets
        PathMatcher<TestHandler> router;
        router.add("/!{a,b}", {8, "negated"});

        assert(router.match("/a") == nullptr);
        assert(router.match("/b") == nullptr);
        assert(router.match("/c") != nullptr);
        assert(router.match("/") == nullptr); // Should not match empty
        std::cout << "  [PASS] Negated Sets" << std::endl;
    }

    {
        // 8. Complex fallback
        PathMatcher<TestHandler> router;
        router.add("/api/**", {9, "api_fallback"});
        router.add("/api/special", {10, "special"});

        assert(router.match("/api/special")->id == 10);
        assert(router.match("/api/other")->id == 9);
        assert(router.match("/api/other/deep")->id == 9);
        assert(router.match("/other") == nullptr);
        std::cout << "  [PASS] Complex Fallback" << std::endl;
    }

    {
        // 9. Simple Matcher
        MatcherOptions<TestHandler> opts;
        opts.simpleMatcher = true;
        PathMatcher<TestHandler> router(opts);
        
        router.add("/static/*", {11, "simple_wildcard"});
        // Simple matcher usually treats * as "match anything until end" or similar depending on impl,
        // but the current C++ impl mimics "prefix / suffix" optimization.
        // Let's verify standard prefix/suffix behavior:
        router.add("/img/*.png", {12, "png_images"}); 

        // /static/* should match /static/foo/bar in simple mode if implemented as prefix
        // In this implementation logic:
        // /static/* -> parts ["/static/", ""] -> prefix "/static/"
        assert(router.match("/static/foo.js") != nullptr);
        assert(router.match("/static/foo/bar.css") != nullptr); 

        // /img/*.png -> prefix "/img/", suffix ".png"
        assert(router.match("/img/icon.png") != nullptr);
        assert(router.match("/img/icon.jpg") == nullptr);
        assert(router.match("/other/icon.png") == nullptr);
        
        std::cout << "  [PASS] Simple Matcher" << std::endl;
    }

    {
        // 10. Merge Handlers
        MatcherOptions<TestHandler> opts;
        opts.mergeHandlers = true;
        opts.mergeFn = [](TestHandler existing, const TestHandler& incoming) {
            return TestHandler{existing.id + incoming.id, existing.name + "+" + incoming.name};
        };
        PathMatcher<TestHandler> router(opts);

        router.add("/merge", {100, "A"});
        router.add("/merge", {200, "B"});

        auto* res = router.match("/merge");
        assert(res != nullptr);
        assert(res->id == 300);
        assert(res->name == "A+B");
        std::cout << "  [PASS] Merge Handlers" << std::endl;
    }

    {
        // 11. Groups /user/{a,b,c}
        PathMatcher<TestHandler> router;
        router.add("/user/{a,b,c}", {13, "user_group"});
        
        assert(router.match("/user/a") != nullptr);
        assert(router.match("/user/b") != nullptr);
        assert(router.match("/user/c") != nullptr);
        assert(router.match("/user/d") == nullptr);
        std::cout << "  [PASS] Groups" << std::endl;
    }

    {
        // 12. Combined Braces and Wildcards /{user,admin}/*
        PathMatcher<TestHandler> router;
        router.add("/{user,admin}/*", {14, "segment_or_wildcard"});
        
        assert(router.match("/user/123") != nullptr);
        assert(router.match("/admin/settings") != nullptr);
        assert(router.match("/guest/login") == nullptr);
        std::cout << "  [PASS] Combined Braces" << std::endl;
    }
    
    std::cout << "All Tests Passed!" << std::endl << std::endl;
}

struct Routers {
    PathMatcher<int> router;
    PathMatcher<int> simpleRouter;

    Routers()
        : router()
        , simpleRouter({.simpleMatcher = true})
    {
        for (int i = 0; i < 10000; i++) {
            router.add("/api/v1/user/" + std::to_string(i), i);
            router.add("/api/v1/data/" + std::to_string(i) + "/details", i);
            router.add("/api/v1/data/" + std::to_string(i) + "/*/a", i);

            // In your simple router, these are treated as literal matches for exact strings
            simpleRouter.add("/api/v1/user/" + std::to_string(i), i);
        }

        router.add("/assets/**", 1000);
        router.add("/static/*", 1001);
        router.add("/**", 9999);

        // For simple router, use patterns it excels at
        simpleRouter.add("/assets/*", 1000);
    }
};

// Construct once per process.
static Routers& GetRouters() {
    static Routers routers;
    return routers;
}

// A volatile sink to ensure results are used.
static volatile int g_sink = 0;

// Helper to run one match and sink the result.
static inline void SinkMatch(const int* res) {
    if (res) g_sink = *res;
    benchmark::DoNotOptimize(g_sink);
}

// ---- Benchmarks -------------------------------------------------------------

static void BM_ExactDeep(benchmark::State& state) {
    auto& r = GetRouters().router;
    const std::string path = "/api/v1/data/50/details";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ExactDeep);

static void BM_ExactShallow(benchmark::State& state) {
    auto& r = GetRouters().router;
    const std::string path = "/api/v1/user/50";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ExactShallow);

static void BM_WildcardStar(benchmark::State& state) {
    auto& r = GetRouters().router;
    const std::string path = "/static/style.css";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_WildcardStar);

static void BM_DoubleWildcardStarStar(benchmark::State& state) {
    auto& r = GetRouters().router;
    const std::string path = "/assets/images/logo.png";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_DoubleWildcardStarStar);

static void BM_FallbackRoot(benchmark::State& state) {
    auto& r = GetRouters().router;
    const std::string path = "/random/page/not/found";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_FallbackRoot);

// --- Simple matcher cases ---

static void BM_SimpleExact(benchmark::State& state) {
    auto& r = GetRouters().simpleRouter;
    const std::string path = "/api/v1/user/50";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SimpleExact);

static void BM_SimplePrefix(benchmark::State& state) {
    auto& r = GetRouters().simpleRouter;
    const std::string path = "/assets/images/huge.jpg";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SimplePrefix);

static void BM_Single(benchmark::State& state) {
    auto& r = GetRouters().simpleRouter;
    const std::string path = "/assets";

    for (auto _ : state) {
        benchmark::DoNotOptimize(path);
        auto* res = r.match(path);
        SinkMatch(res);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Single);

int main(int argc, char** argv) {
    try {
        runTests();
        ::benchmark::Initialize(&argc, argv);
        if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
        ::benchmark::RunSpecifiedBenchmarks();
        ::benchmark::Shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
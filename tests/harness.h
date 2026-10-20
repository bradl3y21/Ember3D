// tests/harness.h — owned by Verity · QA (tests/** only).
//
// A tiny assert-based test runner. No external framework: the pinned stack
// (ARCHITECTURE.md §3) says we use our own harness wired into CTest.
//
// Usage in a test file:
//
//     #include "harness.h"
//     TEST(transform_identity_is_identity_matrix) {
//         ember::Transform t{};
//         ember::Mat4 m = t.toMatrix();
//         CHECK_MAT4_EQ(m, kIdentity, 1e-6f);
//     }
//
// The harness supplies main(). Command line:
//     <test binary>            run everything that does NOT need a GPU
//     <test binary> --gpu      additionally run tests tagged GPU_REQUIRE
//     <test binary> --only S   run only tests whose name contains S (exit 1 if none match)
//
// Anything needing an OpenGL context must be tagged GPU_REQUIRE so CTest
// passes on a machine with no GPU/GL at all (tests/README.md).
#pragma once

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

struct TestCase {
    const char* name;
    void (*fn)();
    bool needsGpu;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)(), bool needsGpu) {
        registry().push_back(TestCase{name, fn, needsGpu});
    }
};

// ---- result bookkeeping -----------------------------------------------------

inline int  g_checks     = 0;
inline int  g_failures   = 0;
inline bool g_currentFailed = false;
inline const char* g_currentTest = "";

inline void reportFailure(const char* file, int line, const std::string& what) {
    ++g_failures;
    g_currentFailed = true;
    std::printf("    FAIL %s:%d  %s\n", file, line, what.c_str());
}

// ---- value formatting helpers ----------------------------------------------

inline std::string fmtf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::string(buf);
}

// ---- scalar checks ----------------------------------------------------------

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond)) harness::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        auto va_ = (a); auto vb_ = (b);                                          \
        if (!(va_ == vb_))                                                       \
            harness::reportFailure(__FILE__, __LINE__,                           \
                harness::fmtf("CHECK_EQ(%s, %s): %s != %s", #a, #b,              \
                              std::to_string(va_).c_str(),                       \
                              std::to_string(vb_).c_str()));                     \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        double va_ = (double)(a); double vb_ = (double)(b);                      \
        if (!(std::fabs(va_ - vb_) <= (eps)))                                    \
            harness::reportFailure(__FILE__, __LINE__,                           \
                harness::fmtf("CHECK_NEAR(%s, %s, %s): %.9g vs %.9g (|d|=%.3g)", \
                              #a, #b, #eps, va_, vb_, std::fabs(va_ - vb_)));    \
    } while (0)

// Float-array comparison over any aggregate exposing operator[] (Vec3, Mat4…).
#define CHECK_ARRAY_NEAR(a, b, n, eps)                                           \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        bool ok_ = true; double worst_ = 0.0; int worstI_ = -1;                  \
        for (int i_ = 0; i_ < (int)(n); ++i_) {                                  \
            double d_ = std::fabs((double)(a)[i_] - (double)(b)[i_]);            \
            if (d_ > worst_) { worst_ = d_; worstI_ = i_; }                      \
            if (!(d_ <= (eps))) ok_ = false;                                     \
        }                                                                        \
        if (!ok_)                                                                \
            harness::reportFailure(__FILE__, __LINE__,                           \
                harness::fmtf("CHECK_ARRAY_NEAR(%s, %s, %s, %s): index %d, "     \
                              "|d|=%.6g > eps=%s", #a, #b, #n, #eps,             \
                              worstI_, worst_, #eps));                           \
    } while (0)

// Mat4 is column-major (ARCHITECTURE.md §4.1): element (col,row) is m[col*4+row].
#define CHECK_MAT4_EQ(a, b, eps)  CHECK_ARRAY_NEAR((a).m, (b).m, 16, (eps))
#define CHECK_VEC3_EQ(a, b, eps)  CHECK_ARRAY_NEAR(&((a).x), &((b).x), 3, (eps))

// ---- test registration ------------------------------------------------------

#define TEST(name)                                                               \
    static void harness_test_##name();                                           \
    static harness::Registrar harness_reg_##name(#name, &harness_test_##name,    \
                                                 /*needsGpu=*/false);            \
    static void harness_test_##name()

// Same, but only runs under `--gpu` (needs a live OpenGL context).
#define TEST_GPU(name)                                                           \
    static void harness_test_##name();                                           \
    static harness::Registrar harness_reg_##name(#name, &harness_test_##name,    \
                                                 /*needsGpu=*/true);             \
    static void harness_test_##name()

// ---- main -------------------------------------------------------------------

inline int runAll(int argc, char** argv) {
    bool gpu = false;
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0) gpu = true;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
    }

    int ran = 0, skipped = 0, failedTests = 0;
    std::printf("== %s ==\n", argv[0]);
    for (const TestCase& t : registry()) {
        if (only && std::strstr(t.name, only) == nullptr) continue;
        if (t.needsGpu && !gpu) { ++skipped; continue; }
        ++ran;
        g_currentTest = t.name;
        g_currentFailed = false;
        std::printf("[ RUN  ] %s%s\n", t.name, t.needsGpu ? " (gpu)" : "");
        t.fn();
        if (g_currentFailed) { ++failedTests; std::printf("[ FAIL ] %s\n", t.name); }
        else                 { std::printf("[  ok  ] %s\n", t.name); }
    }
    std::printf("== %d test(s), %d skipped (no --gpu), %d check(s), "
                "%d failed check(s) in %d test(s) ==\n",
                ran, skipped, g_checks, g_failures, failedTests);
    if (!gpu && skipped > 0)
        std::printf("   (GPU-tagged tests are skipped by default; "
                    "pass --gpu to run them)\n");
    if (only && ran == 0 && skipped == 0) {   // a filter that matches nothing must not pass vacuously
        std::printf("!! --only '%s' matched no test\n", only);
        return 1;
    }
    return failedTests == 0 ? 0 : 1;
}

} // namespace harness

#define HARNESS_MAIN()                                                           \
    int main(int argc, char** argv) { return harness::runAll(argc, argv); }

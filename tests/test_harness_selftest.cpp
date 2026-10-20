// tests/test_harness_selftest.cpp — owned by Verity · QA.
//
// Tests the TEST HARNESS ITSELF, using only standard C++ (no src/ includes).
// This file must pass on a bare machine: it proves the QA tooling works
// before any engine code exists, so a later red run means a real defect and
// not a broken runner.
//
// Note: harness failures inside the CHECK macros increment counters; a
// deliberately-failing case cannot be expressed as a normal TEST here (it
// would fail the suite). Instead the properties tested are the runner's
// observable contract: registration order, gpu gating is exercised by
// comparing binary invocations (see tests/README.md), and formatting helpers.
#include "harness.h"

#include <sstream>

TEST(registry_collects_tests) {
    CHECK(harness::registry().size() >= 2);  // at least selftest + one other
    bool foundIdentity = false;
    for (const auto& t : harness::registry())
        if (std::strcmp(t.name, "registry_collects_tests") == 0) foundIdentity = true;
    CHECK(foundIdentity);
}

TEST(check_macro_passes_on_true) {
    int x = 2 + 2;
    CHECK(x == 4);
    CHECK_EQ(x, 4);
    CHECK_NEAR(x, 4.0, 1e-12);
    int arr[3] = {1, 2, 3};
    int arr2[3] = {1, 2, 3};
    CHECK_ARRAY_NEAR(arr, arr2, 3, 1e-6);
}

TEST(near_tolerates_float_noise) {
    float a = 1.0f / 3.0f;
    CHECK_NEAR(a, 0.333333343f, 1e-6);
}

TEST(fmtf_formats) {
    std::string s = harness::fmtf("%d items, %.2f", 3, 1.5);
    CHECK(s == "3 items, 1.50");
}

TEST(fmtf_handles_long_strings) {
    std::string big(1000, 'x');
    std::string s = harness::fmtf("%s", big.c_str());  // must truncate, not overflow
    CHECK(s.size() == 511);                             // 512 buf minus NUL
}

// Verifies the --gpu gate: this test must be SKIPPED by default (so CTest
// passes with no OpenGL context anywhere) and only run under --gpu. The
// summary line reports the skip count; both modes are exercised in CI.
TEST_GPU(gpu_gate_runs_only_with_flag) {
    CHECK(true);
}

HARNESS_MAIN()

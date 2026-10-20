// CPU-only tests for the standalone adaptive-sampling statistics utility.
#include "harness.h"

#include "render/PathSampling.h"

#include <cmath>
#include <limits>

using ember::render::PathAdaptiveSampler;
using ember::render::PathAdaptiveSamplingSettings;

TEST(welford_statistics_drive_absolute_and_relative_error_stopping) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(1));

    PathAdaptiveSamplingSettings settings;
    settings.minSamples = 2;
    settings.maxSamples = 10;
    settings.absoluteError = 0.1;
    settings.relativeError = 0.0;

    CHECK(sampler.needsSample(0, settings));
    CHECK(!sampler.converged(0, settings));
    CHECK(sampler.addSample(0, 9.0));
    CHECK(sampler.addSample(0, 11.0));
    CHECK_EQ(sampler.sampleCount(0), 2u);
    CHECK(!sampler.converged(0, settings)); // SE = 1, above absolute target.

    settings.relativeError = 0.1;
    CHECK(sampler.converged(0, settings)); // 1 <= 0.1 + 0.1 * |mean(10)|.
    CHECK(!sampler.needsSample(0, settings));
}

TEST(minimum_samples_and_maximum_sample_cap_are_distinct) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(2));

    PathAdaptiveSamplingSettings settings;
    settings.minSamples = 3;
    settings.maxSamples = 4;
    settings.absoluteError = 100.0;
    settings.relativeError = 0.0;

    CHECK(sampler.addSample(0, 2.0));
    CHECK(sampler.addSample(0, 2.0));
    CHECK(!sampler.converged(0, settings));
    CHECK(sampler.needsSample(0, settings));
    CHECK(sampler.addSample(0, 2.0));
    CHECK(sampler.converged(0, settings));
    CHECK(!sampler.needsSample(0, settings));

    settings.absoluteError = 0.0;
    CHECK(sampler.addSample(1, 0.0));
    CHECK(sampler.addSample(1, 10.0));
    CHECK(sampler.addSample(1, 0.0));
    CHECK(sampler.addSample(1, 10.0));
    CHECK(!sampler.converged(1, settings));
    CHECK(!sampler.needsSample(1, settings)); // Hard cap, not convergence.
    CHECK(!sampler.allConverged(settings));
}

TEST(per_pixel_limits_do_not_stop_work_for_other_pixels) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(2));
    PathAdaptiveSamplingSettings settings;
    settings.minSamples = 2;
    settings.maxSamples = 4;
    settings.absoluteError = 0.0;
    settings.relativeError = 0.0;

    // Pixel 0 is flat and converges at its minimum. Pixel 1 remains noisy and
    // continues independently until its cap.
    CHECK(sampler.addSample(0, 3.0));
    CHECK(sampler.addSample(0, 3.0));
    CHECK(sampler.addSample(1, 0.0));
    CHECK(sampler.addSample(1, 10.0));
    CHECK(!sampler.needsSample(0, settings));
    CHECK(sampler.needsSample(1, settings));
    CHECK(sampler.addSample(1, 0.0));
    CHECK(sampler.needsSample(1, settings));
    CHECK(sampler.addSample(1, 10.0));
    CHECK(!sampler.needsSample(1, settings));
    CHECK(!sampler.converged(1, settings)); // stopped by cap, not error target
    CHECK_EQ(sampler.sampleCount(0), 2u);
    CHECK_EQ(sampler.sampleCount(1), 4u);
}

TEST(global_convergence_requires_every_pixel_and_respects_minimum) {
  PathAdaptiveSampler sampler;
  CHECK(sampler.reset(2));
  PathAdaptiveSamplingSettings settings;
  settings.minSamples = 2;
  settings.maxSamples = 4;
  settings.absoluteError = 0.0;
  settings.relativeError = 0.0;

  CHECK(sampler.addSample(0, 5.0));
  CHECK(sampler.addSample(1, 5.0));
  CHECK(!sampler.allConverged(settings));
  CHECK(sampler.addSample(0, 5.0));
  CHECK(!sampler.allConverged(settings)); // Pixel 1 has not met minSamples.
  CHECK(sampler.addSample(1, 5.0));
  CHECK(sampler.allConverged(settings));

  CHECK(sampler.reset(0));
  CHECK(!sampler.allConverged(settings)); // Empty images never converge.
}

TEST(reset_is_per_pixel_and_clears_all_statistics) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(3));
    CHECK_EQ(sampler.sampleCount(0), 0u);
    CHECK_EQ(sampler.sampleCount(2), 0u);
    CHECK_EQ(sampler.sampleCount(3), 0u);
    CHECK(sampler.addSample(1, 4.0));
    CHECK_EQ(sampler.sampleCount(0), 0u);
    CHECK_EQ(sampler.sampleCount(1), 1u);

    CHECK(sampler.reset(2));
    CHECK_EQ(sampler.sampleCount(1), 0u);
    CHECK(!sampler.addSample(2, 1.0));
    CHECK(!sampler.needsSample(2, {}));
    CHECK(!sampler.converged(2, {}));
}

TEST(non_finite_samples_and_invalid_settings_are_rejected) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(1));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK(!sampler.addSample(0, nan));
    CHECK(!sampler.addSample(0, infinity));
    CHECK_EQ(sampler.sampleCount(0), 0u);

    PathAdaptiveSamplingSettings settings;
    settings.absoluteError = nan;
    CHECK(!sampler.needsSample(0, settings));
    CHECK(!sampler.converged(0, settings));
    settings.absoluteError = 0.01;
    settings.relativeError = infinity;
    CHECK(!sampler.needsSample(0, settings));
    settings.relativeError = 0.0;
    settings.minSamples = 0;
    CHECK(!sampler.needsSample(0, settings));
    settings.minSamples = 5;
    settings.maxSamples = 4;
    CHECK(!sampler.needsSample(0, settings));
    settings.minSamples = 1;
    settings.maxSamples = 4;
    settings.absoluteError = -1.0;
    CHECK(!sampler.needsSample(0, settings));
}

TEST(unrepresentable_updates_do_not_partially_mutate_pixel_state) {
    PathAdaptiveSampler sampler;
    CHECK(sampler.reset(1));
    const double max = std::numeric_limits<double>::max();
    CHECK(sampler.addSample(0, max));
    CHECK(!sampler.addSample(0, -max)); // Difference overflows double.
    CHECK_EQ(sampler.sampleCount(0), 1u);
    CHECK(!sampler.addSample(1, 1.0));
}

TEST(sample_order_produces_repeatable_decisions) {
    PathAdaptiveSampler a;
    PathAdaptiveSampler b;
    CHECK(a.reset(2));
    CHECK(b.reset(2));

    PathAdaptiveSamplingSettings settings;
    settings.minSamples = 3;
    settings.maxSamples = 20;
    settings.absoluteError = 0.02;
    settings.relativeError = 0.01;
    const double values[] = {0.0, 1.0, 0.5, 0.25, 0.75, 0.5, 0.5, 0.5};

    for (std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        const std::size_t pixel = i % 2;
        CHECK_EQ(a.needsSample(pixel, settings), b.needsSample(pixel, settings));
        if (a.needsSample(pixel, settings)) {
            CHECK(a.addSample(pixel, values[i]));
            CHECK(b.addSample(pixel, values[i]));
        }
        CHECK_EQ(a.sampleCount(pixel), b.sampleCount(pixel));
        CHECK_EQ(a.converged(pixel, settings), b.converged(pixel, settings));
    }
}

HARNESS_MAIN()

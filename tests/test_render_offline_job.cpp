// CPU-only lifecycle tests for the asynchronous offline render job.
#include "harness.h"

#include "core/Math.h"
#include "core/Types.h"
#include "render/OfflineRenderJob.h"

#include <atomic>
#include <algorithm>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>

using namespace ember;
using namespace ember::render;

namespace {

void setCamera(RenderPacket& packet) {
  packet.cameraPosition = {0.0f, 0.0f, 3.0f};
  packet.view = lookAt(packet.cameraPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
  packet.projection = perspective(radians(35.0f), 1.0f, 0.1f, 100.0f);
  packet.background.color = {0.15f, 0.25f, 0.35f, 1.0f};
  packet.background.strength = 0.7f;
  packet.background.lighting = false;
}

OfflineRenderSettings smallSettings(u32 samples = 3) {
  OfflineRenderSettings settings;
  settings.width = 12;
  settings.height = 8;
  settings.targetSamplesPerPixel = samples;
  settings.batchSamples = 2;
  return settings;
}

void makeEmissivePatch(RenderPacket& packet, MeshData& mesh) {
  mesh.positions = {-0.55f, -0.55f, 0.0f,
                     0.55f, -0.55f, 0.0f,
                     0.55f,  0.55f, 0.0f,
                    -0.55f,  0.55f, 0.0f};
  mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  DrawItem item;
  item.mesh = &mesh;
  item.model = Mat4::identity();
  item.material.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
  item.material.emissionColor = {1.0f, 0.7f, 0.2f, 1.0f};
  item.material.emissionStrength = 1.0f;
  packet.items.push_back(item);
}

bool sameImage(const PathTraceImage& a, const PathTraceImage& b) {
  return a.width == b.width && a.height == b.height &&
         a.samplesPerPixel == b.samplesPerPixel && a.linearPixels == b.linearPixels &&
         a.pixelSampleCounts == b.pixelSampleCounts;
}

void makeEmissiveHalfPlane(RenderPacket& packet, MeshData& mesh) {
  mesh.positions = {-100.0f, -100.0f, 0.0f,
                      0.0f, -100.0f, 0.0f,
                      0.0f,  100.0f, 0.0f,
                   -100.0f,  100.0f, 0.0f};
  mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  DrawItem item;
  item.mesh = &mesh;
  item.model = Mat4::identity();
  item.material.baseColor = {0, 0, 0, 1};
  item.material.emissionColor = {1, 0.5f, 0.25f, 1};
  item.material.emissionStrength = 2.0f;
  packet.items.push_back(item);
}

} // namespace

TEST(offline_job_completes_exact_target_and_returns_a_copy) {
  RenderPacket packet;
  setCamera(packet);

  OfflineRenderJob job;
  const OfflineRenderSettings settings = smallSettings(5);
  CHECK(job.state() == OfflineRenderState::Idle);
  CHECK(job.start(packet, settings));
  job.wait();

  CHECK(job.state() == OfflineRenderState::Completed);
  CHECK_EQ(job.samplesDone(), 5u);
  CHECK_EQ(job.minimumSamplesDone(), 5u);
  CHECK_EQ(job.totalSamplesDone(), 12u * 8u * 5u);
  auto preview = job.previewImage();
  CHECK(preview.has_value());
  if (preview) {
    CHECK_EQ(preview->samplesPerPixel, settings.targetSamplesPerPixel);
    preview->linearPixels[0].r = 1000.0f;
  }
  const auto secondPreview = job.previewImage();
  CHECK(secondPreview.has_value());
  if (secondPreview) CHECK(secondPreview->linearPixels[0].r != 1000.0f);

  auto firstImage = job.image();
  CHECK(firstImage.has_value());
  if (firstImage) {
    CHECK_EQ(firstImage->width, settings.width);
    CHECK_EQ(firstImage->height, settings.height);
    CHECK_EQ(firstImage->samplesPerPixel, settings.targetSamplesPerPixel);
    CHECK_EQ(firstImage->linearPixels.size(),
             static_cast<std::size_t>(settings.width * settings.height));
    firstImage->linearPixels[0].r = 1000.0f;
  }
  const auto secondImage = job.image();
  CHECK(secondImage.has_value());
  if (secondImage) CHECK(secondImage->linearPixels[0].r != 1000.0f);
  const auto rawResult = job.rawImage();
  const auto rawPreview = job.rawPreviewImage();
  CHECK(rawResult.has_value());
  CHECK(rawPreview.has_value());
  if (secondImage && rawResult) CHECK(sameImage(*secondImage, *rawResult));
  if (rawResult && rawPreview) CHECK(sameImage(*rawResult, *rawPreview));
  CHECK(!job.cancel());
}

TEST(offline_job_adaptive_sampling_stops_at_minimum_and_is_deterministic) {
  RenderPacket packet;
  setCamera(packet);
  OfflineRenderSettings settings = smallSettings(12);
  settings.batchSamples = 5; // Adaptive mode deliberately uses one-spp checks.
  settings.enableAdaptiveSampling = true;
  settings.adaptiveSamplingSettings.minSamples = 3;
  settings.adaptiveSamplingSettings.maxSamples = 10;
  settings.adaptiveSamplingSettings.absoluteError = 0.02;
  settings.adaptiveSamplingSettings.relativeError = 0.0;

  OfflineRenderJob first;
  OfflineRenderJob second;
  CHECK(first.start(packet, settings));
  CHECK(second.start(packet, settings));
  first.wait();
  second.wait();

  CHECK(first.state() == OfflineRenderState::Completed);
  CHECK(second.state() == OfflineRenderState::Completed);
  CHECK_EQ(first.samplesDone(), 3u);
  CHECK_EQ(second.samplesDone(), first.samplesDone());
  CHECK_EQ(first.totalSamplesDone(), 12u * 8u * 3u);
  const auto firstImage = first.image();
  const auto secondImage = second.image();
  CHECK(firstImage.has_value());
  CHECK(secondImage.has_value());
  if (firstImage && secondImage) CHECK(sameImage(*firstImage, *secondImage));
}

TEST(offline_job_adaptive_sampling_obeys_each_pixels_sample_cap) {
  RenderPacket packet;
  setCamera(packet);
  MeshData mesh;
  makeEmissiveHalfPlane(packet, mesh);
  OfflineRenderSettings settings = smallSettings(7);
  settings.width = 23;
  settings.height = 15;
  settings.enableAdaptiveSampling = true;
  settings.adaptiveSamplingSettings.minSamples = 2;
  settings.adaptiveSamplingSettings.maxSamples = 5;
  settings.adaptiveSamplingSettings.absoluteError = 0.0;
  settings.adaptiveSamplingSettings.relativeError = 0.0;

  OfflineRenderJob job;
  CHECK(job.start(packet, settings));
  job.wait();
  CHECK(job.state() == OfflineRenderState::Completed);
  const auto image = job.image();
  CHECK(image.has_value());
  if (image) {
    const auto bounds = std::minmax_element(image->pixelSampleCounts.begin(),
                                            image->pixelSampleCounts.end());
    CHECK_EQ(image->samplesPerPixel, *bounds.first);
    CHECK_EQ(*bounds.first, settings.adaptiveSamplingSettings.minSamples);
    CHECK_EQ(*bounds.second, settings.adaptiveSamplingSettings.maxSamples);
    CHECK_EQ(job.samplesDone(), *bounds.first);
    CHECK_EQ(job.minimumSamplesDone(), *bounds.first);
    CHECK_EQ(job.totalSamplesDone(), std::accumulate(
        image->pixelSampleCounts.begin(), image->pixelSampleCounts.end(), std::uint64_t{0}));
  }
}

TEST(offline_job_adaptive_scheduling_has_per_pixel_limits_and_stable_progress) {
  RenderPacket packet;
  setCamera(packet);
  MeshData mesh;
  makeEmissiveHalfPlane(packet, mesh);
  OfflineRenderSettings settings = smallSettings(12);
  settings.width = 23;
  settings.height = 15;
  settings.enableAdaptiveSampling = true;
  settings.adaptiveSamplingSettings.minSamples = 2;
  settings.adaptiveSamplingSettings.maxSamples = 12;
  settings.adaptiveSamplingSettings.absoluteError = 0.0;
  settings.adaptiveSamplingSettings.relativeError = 0.0;

  OfflineRenderJob first;
  OfflineRenderJob second;
  CHECK(first.start(packet, settings));
  CHECK(second.start(packet, settings));
  first.wait();
  second.wait();
  CHECK(first.state() == OfflineRenderState::Completed);
  CHECK(second.state() == OfflineRenderState::Completed);
  const auto firstImage = first.image();
  const auto secondImage = second.image();
  CHECK(firstImage.has_value());
  CHECK(secondImage.has_value());
  if (firstImage && secondImage) {
    CHECK(sameImage(*firstImage, *secondImage));
    const auto bounds = std::minmax_element(firstImage->pixelSampleCounts.begin(),
                                            firstImage->pixelSampleCounts.end());
    CHECK_EQ(*bounds.first, settings.adaptiveSamplingSettings.minSamples);
    CHECK_EQ(*bounds.second, settings.adaptiveSamplingSettings.maxSamples);
    CHECK_EQ(firstImage->samplesPerPixel, *bounds.first);
    CHECK_EQ(first.minimumSamplesDone(), *bounds.first);
    CHECK_EQ(first.samplesDone(), *bounds.first);
    CHECK_EQ(first.totalSamplesDone(), std::accumulate(
        firstImage->pixelSampleCounts.begin(), firstImage->pixelSampleCounts.end(),
        std::uint64_t{0}));
  }
}

TEST(offline_job_denoise_changes_presentation_but_keeps_raw_preview_immutable) {
  RenderPacket packet;
  setCamera(packet);
  MeshData mesh;
  makeEmissivePatch(packet, mesh);
  OfflineRenderSettings settings;
  settings.width = 32;
  settings.height = 24;
  settings.targetSamplesPerPixel = 8;
  settings.batchSamples = 2;
  settings.denoiseOutput = true;
  settings.denoiserSettings.radius = 2;
  settings.denoiserSettings.spatialSigma = 10.0f;
  settings.denoiserSettings.colorSigma = 10.0f;

  OfflineRenderJob job;
  CHECK(job.start(packet, settings));
  std::optional<PathTraceImage> progressiveRaw;
  std::optional<PathTraceImage> progressivePresentation;
  while (job.state() == OfflineRenderState::Running &&
         (!progressiveRaw || !progressivePresentation)) {
    progressiveRaw = job.rawPreviewImage();
    progressivePresentation = job.previewImage();
    if (!progressiveRaw || !progressivePresentation) std::this_thread::yield();
  }
  CHECK(progressiveRaw.has_value());
  CHECK(progressivePresentation.has_value());
  if (progressiveRaw && progressivePresentation)
    CHECK(progressiveRaw->linearPixels != progressivePresentation->linearPixels);
  job.wait();
  CHECK(job.state() == OfflineRenderState::Completed);
  const auto rawPreview = job.rawPreviewImage();
  const auto rawResult = job.rawImage();
  const auto preview = job.previewImage();
  const auto result = job.image();
  CHECK(rawPreview.has_value());
  CHECK(rawResult.has_value());
  CHECK(preview.has_value());
  CHECK(result.has_value());
  if (rawPreview && rawResult && preview && result) {
    CHECK(sameImage(*rawPreview, *rawResult));
    CHECK(sameImage(*preview, *result));
    CHECK(preview->linearPixels != rawPreview->linearPixels);
    CHECK(rawResult->linearPixels != result->linearPixels);
    PathTraceImage modifiedPreview = *preview;
    modifiedPreview.linearPixels[0].r += 100.0f;
    const auto unchangedRaw = job.rawImage();
    CHECK(unchangedRaw.has_value());
    if (unchangedRaw && rawResult) CHECK(sameImage(*unchangedRaw, *rawResult));
    CHECK(preview->linearPixels[0].r != modifiedPreview.linearPixels[0].r);
  }
}

TEST(offline_job_publishes_progressive_previews_and_hides_cancelled_snapshots) {
  RenderPacket packet;
  setCamera(packet);

  OfflineRenderSettings settings;
  settings.width = 128;
  settings.height = 96;
  settings.targetSamplesPerPixel = 1u << 20;
  settings.batchSamples = 1;

  OfflineRenderJob job;
  CHECK(!job.previewImage().has_value());
  CHECK(job.start(packet, settings));

  // Wait for the first publication without relying on elapsed wall-clock time.
  std::optional<PathTraceImage> firstPreview;
  while (job.state() == OfflineRenderState::Running && !firstPreview) {
    firstPreview = job.previewImage();
    if (!firstPreview) std::this_thread::yield();
  }
  CHECK(firstPreview.has_value());
  if (firstPreview) {
    CHECK_EQ(firstPreview->width, settings.width);
    CHECK_EQ(firstPreview->height, settings.height);
    CHECK(firstPreview->samplesPerPixel > 0);
    CHECK(firstPreview->samplesPerPixel < settings.targetSamplesPerPixel);
    CHECK_EQ(firstPreview->linearPixels.size(),
             static_cast<std::size_t>(settings.width * settings.height));

    const u32 firstSampleCount = firstPreview->samplesPerPixel;
    std::optional<PathTraceImage> laterPreview;
    while (job.state() == OfflineRenderState::Running &&
           (!laterPreview || laterPreview->samplesPerPixel <= firstSampleCount)) {
      laterPreview = job.previewImage();
      if (!laterPreview || laterPreview->samplesPerPixel <= firstSampleCount) {
        std::this_thread::yield();
      }
    }
    CHECK(laterPreview.has_value());
    if (laterPreview) {
      CHECK(laterPreview->samplesPerPixel > firstSampleCount);
      CHECK(laterPreview->samplesPerPixel < settings.targetSamplesPerPixel);
    }

    // A caller may modify its snapshot without changing the job's publication.
    firstPreview->linearPixels[0].r = 1000.0f;
    const auto nextPreview = job.previewImage();
    CHECK(nextPreview.has_value());
    if (nextPreview) CHECK(nextPreview->linearPixels[0].r != 1000.0f);
  }

  CHECK(job.cancel());
  CHECK(job.state() == OfflineRenderState::Cancelled);
  CHECK(!job.image().has_value());
  CHECK(!job.previewImage().has_value());
  CHECK(!job.rawImage().has_value());
  CHECK(!job.rawPreviewImage().has_value());
  CHECK(job.samplesDone() < settings.targetSamplesPerPixel);
  job.wait();
  CHECK(job.state() == OfflineRenderState::Cancelled);

  // Reset must discard the cancelled job's final progressive snapshot.
  CHECK(job.reset());
  CHECK(!job.previewImage().has_value());
  CHECK(!job.rawPreviewImage().has_value());
  CHECK(job.start(packet, smallSettings(2)));
  job.wait();
  const auto completedPreview = job.previewImage();
  CHECK(completedPreview.has_value());
  if (completedPreview) CHECK_EQ(completedPreview->samplesPerPixel, 2u);
}

TEST(offline_job_preview_copy_outlives_job_and_reset) {
  PathTraceImage savedPreview;
  {
    RenderPacket packet;
    setCamera(packet);
    OfflineRenderJob job;
    CHECK(job.start(packet, smallSettings(3)));
    job.wait();

    auto preview = job.previewImage();
    CHECK(preview.has_value());
    if (preview) savedPreview = std::move(*preview);
    CHECK(job.reset());
    CHECK(!job.previewImage().has_value());
  }

  CHECK_EQ(savedPreview.samplesPerPixel, 3u);
  CHECK_EQ(savedPreview.linearPixels.size(), 12u * 8u);
}

TEST(offline_job_preview_reads_race_safely_with_cancel_and_wait) {
  RenderPacket packet;
  setCamera(packet);
  OfflineRenderSettings settings;
  settings.width = 128;
  settings.height = 96;
  settings.targetSamplesPerPixel = 1u << 20;
  settings.batchSamples = 1;

  OfflineRenderJob job;
  CHECK(job.start(packet, settings));
  std::atomic<bool> keepReading{true};
  std::atomic<bool> inconsistentSnapshot{false};
  std::atomic<bool> beginJoinRace{false};
  bool cancelRequested = false;

  std::thread reader([&] {
    while (keepReading.load(std::memory_order_acquire)) {
      const auto preview = job.previewImage();
      const auto rawPreview = job.rawPreviewImage();
      if (preview &&
          (preview->width != settings.width || preview->height != settings.height ||
           preview->linearPixels.size() !=
               static_cast<std::size_t>(settings.width * settings.height) ||
            preview->samplesPerPixel == 0 ||
            preview->samplesPerPixel > settings.targetSamplesPerPixel)) {
        inconsistentSnapshot.store(true, std::memory_order_release);
      }
      if (preview &&
          (preview->pixelSampleCounts.size() !=
               static_cast<std::size_t>(settings.width * settings.height) ||
           std::any_of(preview->pixelSampleCounts.begin(), preview->pixelSampleCounts.end(),
                       [samplesPerPixel = preview->samplesPerPixel](u32 count) {
                         return count != samplesPerPixel;
                       }))) {
        inconsistentSnapshot.store(true, std::memory_order_release);
      }
      if (rawPreview &&
          (rawPreview->width != settings.width || rawPreview->height != settings.height ||
           rawPreview->linearPixels.size() !=
               static_cast<std::size_t>(settings.width * settings.height) ||
           rawPreview->samplesPerPixel == 0 ||
            rawPreview->samplesPerPixel > settings.targetSamplesPerPixel)) {
        inconsistentSnapshot.store(true, std::memory_order_release);
      }
      if (rawPreview &&
          (rawPreview->pixelSampleCounts.size() !=
               static_cast<std::size_t>(settings.width * settings.height) ||
           std::any_of(rawPreview->pixelSampleCounts.begin(), rawPreview->pixelSampleCounts.end(),
                       [samplesPerPixel = rawPreview->samplesPerPixel](u32 count) {
                         return count != samplesPerPixel;
                       }))) {
        inconsistentSnapshot.store(true, std::memory_order_release);
      }
      const auto completed = job.image();
      if (completed && completed->samplesPerPixel != settings.targetSamplesPerPixel) {
        inconsistentSnapshot.store(true, std::memory_order_release);
      }
      (void)job.samplesDone();
      (void)job.state();
      std::this_thread::yield();
    }
  });

  std::thread canceller([&] {
    while (!beginJoinRace.load(std::memory_order_acquire)) std::this_thread::yield();
    cancelRequested = job.cancel();
  });
  std::thread waiter([&] {
    while (!beginJoinRace.load(std::memory_order_acquire)) std::this_thread::yield();
    job.wait();
  });
  beginJoinRace.store(true, std::memory_order_release);
  canceller.join();
  waiter.join();
  keepReading.store(false, std::memory_order_release);
  reader.join();

  CHECK(cancelRequested);
  CHECK(!inconsistentSnapshot.load(std::memory_order_acquire));
  CHECK(job.state() == OfflineRenderState::Cancelled);
  CHECK(!job.image().has_value());
  CHECK(!job.previewImage().has_value());
}

TEST(offline_job_owns_packet_data_before_start_returns) {
  OfflineRenderJob job;
  const OfflineRenderSettings settings = smallSettings(2);
  {
    MeshData mesh;
    mesh.positions = {-2.0f, -2.0f, 0.0f,
                       2.0f, -2.0f, 0.0f,
                       2.0f,  2.0f, 0.0f,
                      -2.0f,  2.0f, 0.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    MaterialSlot slot;
    slot.name = "snapshot material";
    slot.material.baseColor = {0.4f, 0.7f, 0.2f, 1.0f};
    const u32 triangleSlots[] = {0, 0};

    RenderPacket packet;
    setCamera(packet);
    DrawItem item;
    item.mesh = &mesh;
    item.material = slot.material;
    item.materials = &slot;
    item.materialCount = 1;
    item.triangleMaterialSlots = triangleSlots;
    item.triangleMaterialSlotCount = 2;
    item.model = Mat4::identity();
    packet.items.push_back(item);
    CHECK(job.start(packet, settings));
  } // All source packet data and its non-owning pointers are now gone.

  job.wait();
  CHECK(job.state() == OfflineRenderState::Completed);
  CHECK_EQ(job.samplesDone(), settings.targetSamplesPerPixel);
  const auto image = job.image();
  CHECK(image.has_value());
  if (image) CHECK_EQ(image->samplesPerPixel, settings.targetSamplesPerPixel);
}

TEST(offline_job_cancellation_is_observed_and_joined) {
  RenderPacket packet;
  setCamera(packet);
  OfflineRenderSettings settings;
  settings.width = 384;
  settings.height = 256;
  settings.targetSamplesPerPixel = 10000;
  settings.batchSamples = 1;
  settings.enableAdaptiveSampling = true;
  settings.adaptiveSamplingSettings.minSamples = 2;
  settings.adaptiveSamplingSettings.maxSamples = 10000;
  settings.adaptiveSamplingSettings.absoluteError = 0.0;
  settings.adaptiveSamplingSettings.relativeError = 0.0;

  OfflineRenderJob job;
  CHECK(job.start(packet, settings));
  CHECK(job.cancel()); // cancel joins before returning, even mid-batch
  CHECK(job.state() == OfflineRenderState::Cancelled);
  CHECK(job.samplesDone() < settings.targetSamplesPerPixel);
  CHECK(!job.image().has_value());
  CHECK(!job.rawImage().has_value());
  CHECK(!job.rawPreviewImage().has_value());
  const u32 joinedProgress = job.samplesDone();
  job.wait();
  CHECK(job.state() == OfflineRenderState::Cancelled);
  CHECK_EQ(job.samplesDone(), joinedProgress);
}

TEST(offline_job_rejects_invalid_settings_and_requires_reset) {
  RenderPacket packet;
  setCamera(packet);
  OfflineRenderJob job;

  OfflineRenderSettings invalid = smallSettings();
  invalid.targetSamplesPerPixel = 0;
  CHECK(!job.start(packet, invalid));
  CHECK(job.state() == OfflineRenderState::Idle);
  invalid = smallSettings();
  invalid.batchSamples = 0;
  CHECK(!job.start(packet, invalid));
  invalid = smallSettings();
  invalid.width = -1;
  CHECK(!job.start(packet, invalid));
  invalid = smallSettings(3);
  invalid.enableAdaptiveSampling = true;
  invalid.adaptiveSamplingSettings.maxSamples = 4;
  CHECK(!job.start(packet, invalid)); // Adaptive max may not exceed target spp.
  invalid.adaptiveSamplingSettings.maxSamples = 2;
  invalid.adaptiveSamplingSettings.minSamples = 3;
  CHECK(!job.start(packet, invalid));
  invalid = smallSettings();
  invalid.denoiseOutput = true;
  invalid.denoiserSettings.radius = 9;
  CHECK(!job.start(packet, invalid));

  const OfflineRenderSettings settings = smallSettings(2);
  OfflineRenderSettings longSettings = smallSettings(10000);
  longSettings.width = 512;
  longSettings.height = 256;
  longSettings.batchSamples = 1;
  CHECK(job.start(packet, longSettings));
  CHECK(!job.start(packet, settings));
  CHECK(!job.reset()); // Running jobs cannot be reset.
  CHECK(job.cancel());
  CHECK(job.state() == OfflineRenderState::Cancelled);
  CHECK(!job.start(packet, settings)); // Terminal jobs require explicit reset.
  CHECK(job.reset());
  CHECK(job.state() == OfflineRenderState::Idle);
  CHECK_EQ(job.samplesDone(), 0u);
  CHECK_EQ(job.totalSamplesDone(), 0u);
  CHECK(!job.reset()); // An already-idle job has nothing to reset.
  CHECK(job.start(packet, settings));
  job.wait();
  CHECK(job.state() == OfflineRenderState::Completed);
}

HARNESS_MAIN()

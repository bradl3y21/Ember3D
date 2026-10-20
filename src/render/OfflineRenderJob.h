#pragma once

#include <mutex>
#include <memory>
#include <optional>
#include <cstdint>
#include <thread>

#include "render/PathTracer.h"
#include "render/PathDenoiser.h"
#include "render/PathSampling.h"

namespace ember::render {

enum class OfflineRenderState {
  Idle,
  Running,
  Completed,
  Cancelled,
  Failed,
};

struct OfflineRenderSettings {
  int width = 640;
  int height = 480;
  ember::u32 targetSamplesPerPixel = 64;
  ember::u32 batchSamples = 1;
  PathTraceSettings pathTrace{};
  // Optional true per-pixel adaptive scheduling. maxSamples must not exceed
  // targetSamplesPerPixel; each batch schedules one sample per active pixel.
  bool enableAdaptiveSampling = false;
  PathAdaptiveSamplingSettings adaptiveSamplingSettings{};
  // Raw linear output remains the default. When enabled, previewImage() and
  // image() return spatially denoised presentation copies; raw*Image() retain
  // the unfiltered accumulation.
  bool denoiseOutput = false;
  PathDenoiserSettings denoiserSettings{};
};

// Backend-only asynchronous wrapper around the synchronous CPU PathTracer.
// start() snapshots packet data before returning, so the caller may release
// or mutate the source packet as soon as start() succeeds. previewImage()
// exposes copied batch snapshots while a job runs (or after completion), while
// image() remains restricted to successfully completed jobs.
class OfflineRenderJob {
 public:
  OfflineRenderJob() = default;
  ~OfflineRenderJob();

  OfflineRenderJob(const OfflineRenderJob&) = delete;
  OfflineRenderJob& operator=(const OfflineRenderJob&) = delete;

  bool start(const ember::RenderPacket&, const OfflineRenderSettings& = {});
  // Signals cancellation without joining. Useful for an interactive viewport;
  // the caller must still wait/reap before reset or destruction.
  bool requestCancel();
  // Requests cancellation if running and always joins the worker before return.
  // Returns true iff cancellation was requested for a running job.
  bool cancel();
  void wait();

  OfflineRenderState state() const;
  // Minimum per-pixel spp, preserving the prior uniform-render semantics.
  ember::u32 samplesDone() const;
  ember::u32 minimumSamplesDone() const;
  // Sum of per-pixel sample counts across the image.
  std::uint64_t totalSamplesDone() const;
  // Returns an independent copy of the latest published batch while Running
  // or Completed. Returns no image before the first batch or after cancellation
  // or failure.
  std::optional<PathTraceImage> previewImage() const;
  // Unfiltered accumulation corresponding to previewImage().
  std::optional<PathTraceImage> rawPreviewImage() const;
  // Returns an independent image copy only after successful completion.
  std::optional<PathTraceImage> image() const;
  // Unfiltered accumulation, available only after successful completion.
  std::optional<PathTraceImage> rawImage() const;

  // A terminal job must be reset before it can be started again. Resetting an
  // Idle or Running job is rejected; a running job must first be cancelled/joined.
  bool reset();

 private:
  struct Snapshot;
  void run(std::shared_ptr<Snapshot>, OfflineRenderSettings);

  // Serializes operations on worker_ (start/cancel/wait/reset). The state mutex
  // is separate so the worker can publish while a caller is joining it.
  mutable std::mutex lifecycleMutex_;
  mutable std::mutex stateMutex_;
  std::thread worker_;
  OfflineRenderState state_ = OfflineRenderState::Idle;
  ember::u32 samplesDone_ = 0;
  std::uint64_t totalSamplesDone_ = 0;
  bool cancelRequested_ = false;
  bool denoiseOutput_ = false;
  std::optional<PathTraceImage> previewImage_;
  std::optional<PathTraceImage> rawPreviewImage_;
  PathTraceImage completedImage_{};
  PathTraceImage completedRawImage_{};
};

} // namespace ember::render

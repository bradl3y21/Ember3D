#include "render/OfflineRenderJob.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ember::render {

namespace {

constexpr int kMaximumDimension = 8192;
constexpr std::size_t kMaximumPixels = 16u * 1024u * 1024u;
constexpr ember::u32 kMaximumBounces = 64;
constexpr ember::u32 kMaximumBatchSamples = 1u << 20;
constexpr ember::u32 kMaximumSamplesPerPixel = 1u << 24;
constexpr int kMaximumDenoiserRadius = 8;

bool validAdaptiveSettings(const PathAdaptiveSamplingSettings& settings,
                           ember::u32 targetSamplesPerPixel) {
  return settings.minSamples > 0 && settings.maxSamples >= settings.minSamples &&
         settings.maxSamples <= targetSamplesPerPixel &&
         std::isfinite(settings.absoluteError) && settings.absoluteError >= 0.0 &&
         std::isfinite(settings.relativeError) && settings.relativeError >= 0.0;
}

bool validDenoiserSettings(const PathDenoiserSettings& settings) {
  return settings.radius >= 0 && settings.radius <= kMaximumDenoiserRadius &&
         std::isfinite(settings.spatialSigma) && settings.spatialSigma > 0.0f &&
         std::isfinite(settings.colorSigma) && settings.colorSigma > 0.0f;
}

bool validSettings(const OfflineRenderSettings& settings) {
  if (settings.width <= 0 || settings.height <= 0 ||
      settings.width > kMaximumDimension || settings.height > kMaximumDimension ||
      static_cast<std::size_t>(settings.width) * static_cast<std::size_t>(settings.height) >
          kMaximumPixels ||
      settings.targetSamplesPerPixel == 0 ||
      settings.targetSamplesPerPixel > kMaximumSamplesPerPixel ||
      settings.batchSamples == 0 || settings.batchSamples > kMaximumBatchSamples ||
      settings.pathTrace.maxBounces > kMaximumBounces) {
    return false;
  }
  if (settings.denoiseOutput && !validDenoiserSettings(settings.denoiserSettings)) return false;
  if (settings.enableAdaptiveSampling &&
      !validAdaptiveSettings(settings.adaptiveSamplingSettings,
                             settings.targetSamplesPerPixel)) {
    return false;
  }
  return true;
}

std::uint64_t totalSamples(const PathTraceImage& image) {
  std::uint64_t total = 0;
  for (ember::u32 count : image.pixelSampleCounts) total += count;
  return total;
}

} // namespace

// All non-owning packet members that PathTracer consumes are rebound to the
// heap-owned copies below. Heap-backed copies keep pointers stable if this
// Snapshot itself is moved into the worker's shared_ptr.
struct OfflineRenderJob::Snapshot {
  std::vector<std::unique_ptr<ember::MeshData>> meshes;
  std::vector<std::unique_ptr<std::vector<ember::MaterialSlot>>> materials;
  std::vector<std::unique_ptr<std::vector<ember::u32>>> triangleSlots;
  ember::RenderPacket packet;

  explicit Snapshot(const ember::RenderPacket& source) {
    packet.cameraPosition = source.cameraPosition;
    packet.view = source.view;
    packet.projection = source.projection;
    packet.clearColor = source.clearColor;
    packet.lights = source.lights;
    packet.background = source.background;
    packet.shadowLightIndex = source.shadowLightIndex;
    packet.assetBaseDirectory = source.assetBaseDirectory;
    packet.width = source.width;
    packet.height = source.height;
    // Offline tracing does not consume edit overlays. In particular, do not
    // copy their frame-valid mesh pointer into a long-lived background packet.

    meshes.reserve(source.items.size());
    materials.reserve(source.items.size());
    triangleSlots.reserve(source.items.size());
    packet.items.reserve(source.items.size());
    for (const ember::DrawItem& original : source.items) {
      ember::DrawItem item = original;
      item.mesh = nullptr;
      item.materials = nullptr;
      item.materialCount = 0;
      item.triangleMaterialSlots = nullptr;
      item.triangleMaterialSlotCount = 0;

      if (original.mesh != nullptr) {
        auto mesh = std::make_unique<ember::MeshData>(*original.mesh);
        item.mesh = mesh.get();
        meshes.push_back(std::move(mesh));
      }
      if (original.materials != nullptr && original.materialCount > 0) {
        auto copiedMaterials = std::make_unique<std::vector<ember::MaterialSlot>>(
            original.materials, original.materials + original.materialCount);
        item.materials = copiedMaterials->data();
        item.materialCount = original.materialCount;
        materials.push_back(std::move(copiedMaterials));
      }
      if (original.triangleMaterialSlots != nullptr && original.triangleMaterialSlotCount > 0) {
        auto copiedSlots = std::make_unique<std::vector<ember::u32>>(
            original.triangleMaterialSlots,
            original.triangleMaterialSlots + original.triangleMaterialSlotCount);
        item.triangleMaterialSlots = copiedSlots->data();
        item.triangleMaterialSlotCount = original.triangleMaterialSlotCount;
        triangleSlots.push_back(std::move(copiedSlots));
      }
      packet.items.push_back(item);
    }
  }
};

OfflineRenderJob::~OfflineRenderJob() {
  cancel();
  wait();
}

bool OfflineRenderJob::start(const ember::RenderPacket& packet,
                             const OfflineRenderSettings& settings) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (state_ != OfflineRenderState::Idle || !validSettings(settings)) return false;
    state_ = OfflineRenderState::Running;
    samplesDone_ = 0;
    totalSamplesDone_ = 0;
    cancelRequested_ = false;
    denoiseOutput_ = settings.denoiseOutput;
    previewImage_.reset();
    rawPreviewImage_.reset();
    completedImage_ = {};
    completedRawImage_ = {};
  }

  std::shared_ptr<Snapshot> snapshot;
  try {
    // Deep-copy synchronously: after this construction and before start()
    // returns, the worker has no dependency on caller-owned packet storage.
    snapshot = std::make_shared<Snapshot>(packet);
  } catch (...) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    state_ = OfflineRenderState::Failed;
    return false;
  }

  try {
    worker_ = std::thread(&OfflineRenderJob::run, this, std::move(snapshot), settings);
  } catch (...) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    state_ = OfflineRenderState::Failed;
    return false;
  }
  return true;
}

void OfflineRenderJob::run(std::shared_ptr<Snapshot> snapshot,
                           OfflineRenderSettings settings) {
  try {
    PathTracer tracer;
    if (!tracer.setScene(snapshot->packet, settings.width, settings.height,
                         settings.pathTrace)) {
      std::lock_guard<std::mutex> stateLock(stateMutex_);
      state_ = cancelRequested_ ? OfflineRenderState::Cancelled : OfflineRenderState::Failed;
      return;
    }

    for (;;) {
      {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (cancelRequested_) {
          state_ = OfflineRenderState::Cancelled;
          return;
        }
      }

      if (settings.enableAdaptiveSampling) {
        if (tracer.adaptiveComplete(settings.adaptiveSamplingSettings) ||
            tracer.image().samplesPerPixel >= settings.targetSamplesPerPixel) break;
      } else if (tracer.image().samplesPerPixel >= settings.targetSamplesPerPixel) {
        break;
      }

      const ember::u32 batch = settings.enableAdaptiveSampling
          ? 1u
          : std::min(settings.batchSamples,
                     settings.targetSamplesPerPixel - tracer.image().samplesPerPixel);
      const bool rendered = settings.enableAdaptiveSampling
          ? tracer.renderAdaptiveSample(settings.adaptiveSamplingSettings)
          : tracer.renderSamples(batch);
      if (!rendered) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        state_ = cancelRequested_ ? OfflineRenderState::Cancelled : OfflineRenderState::Failed;
        return;
      }

      // PathTracer is worker-owned, so this image copy cannot race its next
      // batch. Publish the image and its sample count together under the state
      // mutex, allowing readers to take a consistent independent snapshot.
      PathTraceImage rawPreview = tracer.image();
      PathTraceImage preview = rawPreview;
      if (settings.denoiseOutput &&
          !denoisePathTraceImage(rawPreview, preview, settings.denoiserSettings)) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        state_ = cancelRequested_ ? OfflineRenderState::Cancelled : OfflineRenderState::Failed;
        return;
      }
      // The denoiser only changes pixels; preserve the source image's adaptive
      // accounting metadata on the independently published presentation copy.
      preview.pixelSampleCounts = rawPreview.pixelSampleCounts;
      {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (cancelRequested_) {
          state_ = OfflineRenderState::Cancelled;
          return;
        }
        previewImage_ = std::move(preview);
        if (settings.denoiseOutput) rawPreviewImage_ = std::move(rawPreview);
        else rawPreviewImage_.reset();
        samplesDone_ = tracer.image().samplesPerPixel;
        totalSamplesDone_ = totalSamples(tracer.image());
      }
    }

    // Copy from the tracer before entering the publication critical section.
    // Cancellation and completion are linearized under stateMutex_: a cancel
    // request that wins that lock prevents this image from being published.
    PathTraceImage rawResult = tracer.image();
    PathTraceImage result = rawResult;
    if (settings.denoiseOutput &&
        !denoisePathTraceImage(rawResult, result, settings.denoiserSettings)) {
      std::lock_guard<std::mutex> stateLock(stateMutex_);
      state_ = cancelRequested_ ? OfflineRenderState::Cancelled : OfflineRenderState::Failed;
      return;
    }
    result.pixelSampleCounts = rawResult.pixelSampleCounts;
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (cancelRequested_) {
      state_ = OfflineRenderState::Cancelled;
      return;
    }
    completedImage_ = std::move(result);
    if (settings.denoiseOutput) completedRawImage_ = std::move(rawResult);
    else completedRawImage_ = {};
    samplesDone_ = tracer.image().samplesPerPixel;
    totalSamplesDone_ = totalSamples(tracer.image());
    state_ = OfflineRenderState::Completed;
  } catch (...) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    state_ = cancelRequested_ ? OfflineRenderState::Cancelled : OfflineRenderState::Failed;
  }
}

bool OfflineRenderJob::requestCancel() {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != OfflineRenderState::Running) return false;
  cancelRequested_ = true;
  return true;
}

bool OfflineRenderJob::cancel() {
  bool requested = requestCancel();
  // Signal the worker before waiting for lifecycleMutex_: a concurrent wait()
  // may already hold that mutex while joining this same worker. The worker
  // only needs stateMutex_ to observe the request and finish that join.
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  if (!requested) {
    // Preserve serialization with start()/reset() when the initial state check
    // found no running job but one started before this call acquired the lock.
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (state_ == OfflineRenderState::Running) {
      cancelRequested_ = true;
      requested = true;
    }
  }
  if (worker_.joinable()) worker_.join();
  return requested;
}

void OfflineRenderJob::wait() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  if (worker_.joinable()) worker_.join();
}

OfflineRenderState OfflineRenderJob::state() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  return state_;
}

ember::u32 OfflineRenderJob::samplesDone() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  return samplesDone_;
}

ember::u32 OfflineRenderJob::minimumSamplesDone() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  return samplesDone_;
}

std::uint64_t OfflineRenderJob::totalSamplesDone() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  return totalSamplesDone_;
}

std::optional<PathTraceImage> OfflineRenderJob::previewImage() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != OfflineRenderState::Running && state_ != OfflineRenderState::Completed) {
    return std::nullopt;
  }
  return previewImage_;
}

std::optional<PathTraceImage> OfflineRenderJob::rawPreviewImage() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != OfflineRenderState::Running && state_ != OfflineRenderState::Completed) {
    return std::nullopt;
  }
  return denoiseOutput_ ? rawPreviewImage_ : previewImage_;
}

std::optional<PathTraceImage> OfflineRenderJob::image() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != OfflineRenderState::Completed) return std::nullopt;
  return completedImage_;
}

std::optional<PathTraceImage> OfflineRenderJob::rawImage() const {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != OfflineRenderState::Completed) return std::nullopt;
  return denoiseOutput_ ? completedRawImage_ : completedImage_;
}

bool OfflineRenderJob::reset() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (state_ == OfflineRenderState::Idle || state_ == OfflineRenderState::Running) return false;
  }
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  state_ = OfflineRenderState::Idle;
  samplesDone_ = 0;
  totalSamplesDone_ = 0;
  cancelRequested_ = false;
  denoiseOutput_ = false;
  previewImage_.reset();
  rawPreviewImage_.reset();
  completedImage_ = {};
  completedRawImage_ = {};
  return true;
}

} // namespace ember::render

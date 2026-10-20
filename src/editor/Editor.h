// Editor.h — the ImGui host: context, backends, dockspace, panels.
// Owner: Chisel · Editor. Contract: ARCHITECTURE.md §4.5 (init signature per D13).
//
// The editor links core, render and scene only. It never includes a platform
// header: the window arrives as an opaque void* (the GLFWwindow*), and App owns
// the UndoStack and passes it in.
#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

#include "core/Types.h"
#include "editor/Gizmo.h"
#include "editor/Modal.h"
#include "editor/Panels.h"
#include "editor/Preferences.h"
#include "editor/Undo.h"
#include "render/OfflineRenderJob.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

namespace ember::editor {

struct CyclesViewportProgress {
    bool visible = false;
    bool settling = false;
    bool hasPreview = false;
    bool workerRunning = false;
    render::OfflineRenderState state = render::OfflineRenderState::Idle;
    u32 samplesDone = 0;
    u32 targetSamples = 0;
    std::uint64_t generation = 0;
};

enum class ViewportPreviewFormat { PFM, PPM };
enum class ViewportPreviewSaveState { Idle, Running, Completed, Failed };

struct ViewportPreviewSaveProgress {
    ViewportPreviewSaveState state = ViewportPreviewSaveState::Idle;
    std::filesystem::path path;
};

class Editor {
public:
    Editor() = default;
    ~Editor();

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // nativeWindow is platform::Window::nativeHandle(). The GL context must be
    // current and loaded already (platform guarantees this inside Window::create).
    bool init(void* nativeWindow);
    void shutdown();

    // Poll/start the Cycles-style viewport job before Renderer draws this packet.
    // App supplies monotonic time so the settle debounce is deterministic in
    // CPU-only lifecycle tests. A missing viewport area clears the job/preview.
    void prepareViewportRender(const RenderPacket& packet, render::Renderer& renderer,
                              double nowSeconds, bool viewportAreaAvailable);
    void cancelCyclesViewport() { cyclesCancelRequested_ = true; }
    const CyclesViewportProgress& cyclesViewportProgress() const { return cyclesProgress_; }
    // Saves only a completed, viewport-sized CPU preview. The image is held by
    // shared ownership and written on a worker so the UI frame never waits for IO.
    bool saveCompletedViewportPreview(const std::filesystem::path& path,
                                      ViewportPreviewFormat format);
    ViewportPreviewSaveProgress viewportPreviewSaveProgress() const;

    // Runs one ImGui frame over the already-rendered viewport: NewFrame, panels,
    // Render, backend draw. Call after renderer.renderFrame(packet).
    void draw(scene::World& world, render::Renderer& renderer, const RenderStats& stats,
              UndoStack& undo);

    // True while ImGui is not capturing the mouse, i.e. the pointer is over the
    // viewport area (the dockspace's transparent central node).
    bool wantsViewportFocus() const;

    // View options set from the Viewport menu. The renderer reads the same
    // struct through Renderer::viewportSettings() once Halide's half lands.
    const ember::ViewportSettings& viewportSettings() const { return viewport_; }
    TransformSpace transformSpace() const { return transformSpace_; }
    // Preference read by the camera input (App). True = drag the other way.
    bool invertOrbit() const { return prefs_.invertOrbit; }
    // When enabled, the App lets top-row digits emulate viewport numpad keys;
    // otherwise Object Mode leaves them alone and Edit Mode uses 1/2/3.
    bool emulateNumpad() const { return prefs_.emulateNumpad; }
    InteractionMode interactionMode() const { return interactionMode_; }
    SelectMode selectMode() const { return selectMode_; }
    SidebarTab sidebarTab() const { return sidebarTab_; }
    EditOverlayState editOverlayState(const scene::World& world) const;
    // The 3D viewport area in framebuffer pixels, top-left origin. Invalid (w or h
    // <= 0) until the layout exists.
    Rect viewportRect() const;
    Rect viewportAreaRect() const;
    // 0 = not run / still running, 1 = PASS, -1 = FAIL. Meaningful for the
    // EMBER_SELFTEST_TRANSFORM and EMBER_SELFTEST_PIE event scripts; App turns a
    // reported failure into the process exit code.
    int selfTestStatus() const { return selfTestStatus_; }

private:
    bool initialized_ = false;
    FileDialog file_;
    GizmoController gizmo_;
    bool sidebarOpen_ = true;
    PaletteState palette_;
    DocumentState doc_;
    ToolState tools_;
    Preferences prefs_;
    bool prefsOpen_ = false;
    bool historyOpen_ = false;
    bool layoutBuilt_ = false;   // the fixed dock layout is built once per launch
    SidebarTab sidebarTab_ = SidebarTab::Item;
    PropertiesTab propertiesTab_ = PropertiesTab::Object;
    AreaType areas_[4] = {AreaType::Outliner, AreaType::Properties, AreaType::Timeline, AreaType::Viewport};
    WorkspacePreset workspace_ = WorkspacePreset::Layout;
    WorkspacePreset appliedWorkspace_ = WorkspacePreset::Layout;
    SnapTarget snapTarget_ = SnapTarget::GridIncrement;
    bool snapEnabled_ = false;
    TimelineState timeline_;
    ModalTransform modal_;
    PieState pie_;
    ModePieState modePie_;
    ImVec4 viewportRegion_{0.0f, 0.0f, 0.0f, 0.0f};   // window points, from drawWorkspace
    ImVec4 viewportAreaRegion_{0.0f, 0.0f, 0.0f, 0.0f}; // full Area D, including its Header
    int selfTestStatus_ = 0;
    InteractionMode interactionMode_ = InteractionMode::Object;
    SelectMode selectMode_ = SelectMode::Vertex;
    ember::ViewportSettings viewport_;
    TransformSpace transformSpace_ = TransformSpace::Global;
    render::OfflineRenderJob cyclesJob_;
    std::shared_ptr<const render::PathTraceImage> cyclesPreview_;
    render::Renderer* cyclesRenderer_ = nullptr; // non-owning; App shuts Editor down first
    CyclesViewportProgress cyclesProgress_{};
    std::uint64_t cyclesGeneration_ = 0;
    std::uint64_t cyclesPacketKey_ = 0;
    double cyclesLastChangeSeconds_ = 0.0;
    double cyclesLastPreviewSeconds_ = 0.0;
    bool cyclesHasPacketKey_ = false;
    bool cyclesWaitingForSettle_ = false;
    bool cyclesCancelledByUser_ = false;
    bool cyclesCancelRequested_ = false;
    bool compareDividerDragging_ = false;
    char previewSavePath_[260] = "viewport-preview.pfm";
    int previewSaveFormat_ = 0;
    std::thread previewSaveWorker_;
    std::atomic<ViewportPreviewSaveState> previewSaveState_{ViewportPreviewSaveState::Idle};
    std::filesystem::path previewSavePathResult_;
};

// Main-row 1/2/3 select Vertex/Edge/Face only in Edit Mode, on the viewport,
// when text input and Emulate Numpad do not own those keys. Shared with the
// headless input test so the key contract is exercised directly.
bool handleEditSelectModeKey(const ImGuiIO& io, InteractionMode mode,
                             bool emulateNumpad, SelectMode& selectMode);

} // namespace ember::editor

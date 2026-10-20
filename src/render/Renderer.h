// Renderer.h — frame lifecycle and passes. Contract: ARCHITECTURE.md §4.3.
//
// THE ONE RULE: the renderer draws the RenderPacket it is handed. It never
// walks the scene graph, never includes a scene header, and does not read
// platform input — App polls and feeds deltas to camera() if it wants.
#pragma once

#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "render/Camera.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/PathTracer.h"
#include "render/Shader.h"
#include "render/TextureImage.h"

namespace ember::render {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init();
    void shutdown();
    void resize(int width, int height);
    void renderFrame(const RenderPacket& packet);
    // Borrow a linear CPU path-trace image for the next renderFrame only. The
    // caller must keep it alive through that call; it is cleared/consumed even
    // when the renderer cannot draw or the image is malformed.
    void setPathTraceImage(const PathTraceImage* image) { pathTraceImage_ = image; }
    void readViewport(int x, int y, int w, int h, std::vector<u8>& outRGBA);
    RenderStats lastStats() const { return stats_; }
    // Fixed, deterministic exposure in photographic stops. The display pass
    // applies 2^stops before tone mapping; non-finite inputs are ignored.
    void setExposureStops(float stops);
    float exposureStops() const { return exposureStops_; }
    // Asynchronous GL_TIME_ELAPSED measurement of the last completed shadow
    // depth pass. 0 until the driver has returned the first query result.
    double lastShadowGpuMs() const { return lastShadowGpuMs_; }

    // --- M1 orbit camera ---
    // The camera type is render's (see Camera.h's ownership note). App polls
    // platform::Input, forwards deltas here, and fills the packet's
    // view/projection/cameraPosition from it. renderFrame() still consumes
    // only the packet.
    OrbitCamera& camera() { return camera_; }
    const OrbitCamera& camera() const { return camera_; }

    // --- viewport settings (D19, ARCHITECTURE §4.3) ---
    // Editor UI state pushed once per draw(). The renderer consumes it on the
    // NEXT renderFrame — the immediate-mode norm, documented not surprising.
    // honoured now: Wireframe = GL_LINE; Solid = fast scene-light-independent
    // studio diffuse; Material = PBR + neutral studio lights/environment;
    // Rendered = full packet lights/world ambient and optional shadow map.
    // showGrid (with item-85 distance fade) / showAxes / showOrigins /
    // showSelectionOutline. showGizmo gates markers and drawGizmo.
    void setViewportSettings(const ViewportSettings& settings);
    const ViewportSettings& viewportSettings() const { return settings_; }

    // --- D29 (item 65): the viewport rect -----------------------------------
    // Framebuffer px, TOP-LEFT origin (core::Rect). When valid, renderFrame
    // scopes its 3D pass — viewport AND scissor — to that rect, so areas,
    // headers and panels around it are never overdrawn; markers/gizmo (drawn
    // later, same area) inherit the scope. Invalid = legacy full-framebuffer.
    // readViewport then works in AREA-LOCAL coordinates exactly as before:
    // x,y measure inside the rect the same way they measured inside the fb
    // when no rect is set. The packet's width/height are the AREA's size
    // (D29) — aspect and worldPerPixel math therefore stay correct inside
    // non-square areas.
    void setViewportRect(const Rect& rect, const Rect& clearRect = {}) {
        viewportRect_ = rect;
        viewportClearRect_ = clearRect;
    }
    const Rect& viewportRect() const { return viewportRect_; }

    // --- light markers (item 58) ---
    // Each entry in packet.lights draws a Blender-like line marker at its
    // world position: directional = sun disc + travel line, point = star with
    // rays. (Spot needs LightType::Spot in core — raised with Ash; the cone
    // drops in once the kind exists.) Markers honour showGizmo, are sized in
    // PIXELS (constant on screen at any zoom), drawn after scene items with
    // depth test off so objects cannot occlude them, and tinted by the
    // light's own colour (dark lights fall back to white). Directional sun
    // disc radius is lightMarkerRadiusPixels(); the point star's OUTERMOST ray
    // tip is 13 px, strictly inside that 14 px hit radius.
    //
    // PICKING CONTRACT (D20 precedent — render draws, editor hit-tests): the
    // editor maps marker i to its light object via creation order —
    // packet.lights is filled in world creation order (Slate's D19 contract),
    // and the anchor is LightInstance::position in world space. Hit radius is
    // lightMarkerRadiusPixels(); the editor projects with camera().view()/
    // projection(aspect) and selects on click. This accessor is additive per
    // standing policy — the drawn size and the clickable size are one number.
    static float lightMarkerRadiusPixels();   // sun disc / star radius

    // --- transform gizmo (D19/D20/D21, frozen signature) ---
    // Draws the gizmo the EDITOR built: handles are len = handleLengthPixels
    // long, measured in world units at the anchor's depth (constant on screen),
    // along the columns of GizmoState::orientation — identity = Global, an
    // object's world rotation = Local (D20). The same number is the editor's
    // hit-test size: visible handle and clickable handle cannot drift.
    // hoveredAxis/activeAxis are shown (brightened), never interpreted.
    // Gated by settings_.showGizmo, drawn with depth test off (on top).
    void drawGizmo(const GizmoState& state, const Mat4& view, const Mat4& projection);

private:
    struct MeshCacheEntry {
        Mesh mesh;
        bool uploaded = false;
        std::size_t positionCount = 0;
        std::size_t normalCount = 0;
        std::size_t uvCount = 0;
        std::size_t indexCount = 0;
        u32 geometryVersion = 0;
        u32 modifierVersion = 0;
        bool hasGeometryVersion = false;
        Vec3 localCenter{0.0f, 0.0f, 0.0f}; // cached mesh AABB center for transparent depth sort
    };

    struct ImageTextureCacheEntry {
        u32 texture = 0;
        bool attempted = false;
        bool hasAlpha = false;
    };

    void buildOverlay();
    void buildGrid(int subdivisions);   // D24: rebuilt when gridSubdivisions changes
    void uploadMeshes(const RenderPacket& packet);
    void drawOverlay(const Mat4& view, const Mat4& proj, const Vec3& cameraPosition);
    void drawOrigins(const RenderPacket& packet, const Mat4& view, const Mat4& proj);
    void drawEditOverlay(const EditOverlayState& state, const Mat4& view, const Mat4& proj);
    void drawSelectionOutlines(const RenderPacket& packet, const Mat4& view, const Mat4& proj);
    bool ensureHdrTarget(int width, int height);
    bool ensurePathTraceTarget(int width, int height);
    bool presentPathTraceImage(const PathTraceImage& image, bool blitToHdr = true);
    ImageTextureCacheEntry* imageTexture(const ImageTextureRef& reference,
                                         const std::string& baseDirectory);
    void drawEnvironmentBackground(const RenderPacket& packet, const Mat4& view,
                                   const Mat4& proj, const Vec3& cameraPosition);
    void drawDisplayPass(u32 outputFramebuffer, bool comparePreview, float compareSplit);
    bool ensureShadowTarget(int resolution);
    bool renderShadowPass(const RenderPacket& packet);
    void collectShadowGpuTime();
    void drawItems(const RenderPacket& packet, const Mat4& view, const Mat4& proj,
                   const Vec3& cameraPosition);
    void drawLightMarkers(const RenderPacket& packet, const Mat4& view, const Mat4& proj);

    bool initialized_ = false;
    int lastLightIgnored_ = -1;      // warn-once tracker for the light cap (D19)
    int builtGridGs_ = -1;           // gridSubdivisions the grid_ was built for
    int width_ = 0;
    int height_ = 0;
    Rect viewportRect_{};            // D29: 3D area in fb px (invalid = full fb)
    Rect viewportClearRect_{};       // owning editor Area; clears the body outside the 3D rect too
    int fbW_ = 0, fbH_ = 0;         // framebuffer size from resize() (window)
    int vpOriginX_ = 0, vpOriginY_ = 0;  // last renderFrame's GL-bottom origin
    u32 hdrFbo_ = 0;
    u32 hdrColor_ = 0;
    u32 hdrDepth_ = 0;
    u32 pathTraceFbo_ = 0;
    u32 pathTraceTexture_ = 0;
    int pathTraceWidth_ = 0, pathTraceHeight_ = 0;
    const PathTraceImage* pathTraceImage_ = nullptr; // borrowed through the next renderFrame
    u32 displayVao_ = 0;
    int hdrWidth_ = 0, hdrHeight_ = 0;
    u32 lastOutputReadFramebuffer_ = 0;
    float exposureStops_ = 0.0f;
    u32 shadowFbo_ = 0;
    u32 shadowDepth_ = 0;
    u32 shadowResolution_ = 0;
    u32 shadowTimerQuery_ = 0;
    bool shadowTimerPending_ = false;
    bool shadowValid_ = false;
    int activeShadowLight_ = -1;
    int lastShadowResolutionLogged_ = -1;
    Mat4 lightViewProjection_{};
    double lastShadowGpuMs_ = 0.0;
    // Must differ from the "missing shadowLightIndex already warned" state
    // (-1) so the cold-start invalid-caster path emits its first warning.
    int lastShadowWarning_ = -4;
    RenderStats stats_{};
    OrbitCamera camera_{};
    ViewportSettings settings_{};
    Shader overlayShader_{};
    Shader sceneShader_{};
    Shader outlineShader_{};                    // item 85: extruded-hull orange
    Shader shadowShader_{};                     // item 96: depth-only directional pass
    Shader displayShader_{};                    // fullscreen HDR -> display transform
    Shader environmentShader_{};                // lat-long image -> raster world background
    Mesh grid_{};   // viewport grid lines  — gated by settings_.showGrid
    Mesh axes_{};   // RGB world axes lines — gated by settings_.showAxes
    Mesh originsMesh_{};   // object-origin crosses, rebuilt per frame (item 85, showOrigins)
    Mesh editOverlayMesh_{};  // Edit Mode edges/vertex crosses, rebuilt per frame
    Mesh lightMesh_{};  // light markers, rebuilt per frame when shown (item 58)
    Mesh gizmoMesh_{};  // transform gizmo, rebuilt per drawGizmo call (D20/D21)
    std::unordered_map<const MeshData*, MeshCacheEntry> meshCache_;
    std::unordered_map<std::string, ImageTextureCacheEntry> imageTextureCache_;
};

} // namespace ember::render

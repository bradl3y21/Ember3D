// Ember3D — the composition root (implementation).
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §4.6.
//
// This is the only translation unit that knows the whole module graph. Which
// modules are present arrives from CMake as real compile definitions
// (EMBER_HAS_<MODULE>=1, set in src/core/CMakeLists.txt).
//
// That is deliberate, learned the hard way: CMake cannot see __has_include, so
// when the editor module appeared, this file was NOT recompiled and the editor
// was silently missing from an otherwise green build (operator, 2026-10-16). A
// compile definition is tracked: adding a module changes the command line,
// which forces the rebuild.
//
// Frame order is the contract (§4.5/§4.6, decision D13):
//
//   poll events -> camera from input -> buildPacket -> editor.prepareViewportRender
//               -> renderFrame
//               -> editor.draw (ImGui NewFrame..draw) -> swapBuffers
//
// The packet is built after the editor's mutations of the *previous* frame and
// consumed immediately, which is what makes DrawItem::mesh pointers safe (D9).

#include "core/App.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <vector>

#include "core/Log.h"
#include "core/Types.h"

#if defined(EMBER_HAS_PLATFORM)
  #include "platform/Input.h"
  #include "platform/Window.h"
#endif
#if defined(EMBER_HAS_RENDER)
  #include "render/Renderer.h"
#endif
#if defined(EMBER_HAS_SCENE)
  #include "scene/Scene.h"
#endif
#if defined(EMBER_HAS_EDITOR)
  #include "editor/Editor.h"
  #include "editor/Undo.h"
#endif

// Compiling this file by hand (outside CMake)? Pass -DEMBER_HAS_RENDER=1 and
// friends. With none of them set you get a core-only shell — say so at compile
// time instead of silently shipping a smaller app, which is the failure this
// whole scheme exists to prevent.
#if !defined(EMBER_HAS_PLATFORM) && !defined(EMBER_HAS_RENDER) && \
    !defined(EMBER_HAS_SCENE) && !defined(EMBER_HAS_EDITOR)
  #pragma message("Ember3D: no EMBER_HAS_* module defines — building a core-only shell. If you expected a window, the build system is not passing the module defines.")
#endif

namespace {

// ---------------------------------------------------------------------------
// Navigation actions, defined ONCE (item 5, Blender keys).
//
// Both the key bindings and the headless self-test call these methods, so the
// thing I verified with EMBER_NAV_SELFTEST is the same code path a key press
// takes — the same discipline as --frames/--demo-object. Logging lives in here
// too, so a key press and a self-test produce identical evidence lines.
// ---------------------------------------------------------------------------
#ifdef EMBER_HAS_RENDER
struct NavActions {
  ember::render::OrbitCamera& camera;
#ifdef EMBER_HAS_SCENE
  ember::scene::World* world = nullptr;
#endif

  void logState(const char* what) const {
    EMBER_LOG_INFO("nav %-22s yaw=%7.1f pitch=%7.1f ortho=%d dist=%6.2f target=(%.2f,%.2f,%.2f)",
                   what, ember::degrees(camera.yaw()), ember::degrees(camera.pitch()),
                   camera.isOrthographic() ? 1 : 0, camera.distance(), camera.target().x,
                   camera.target().y, camera.target().z);
  }

  void setAxisView(ember::render::StandardView view, const char* name) {
    camera.setView(view);          // orientation only: target and distance kept
    logState(name);
  }

  // Numpad 9: the opposite of the CURRENT axis view (Blender). Answered here
  // from the view direction rather than by asking the camera for a toggle: the
  // camera has no business deciding what "current view" means, and App can read
  // position/target. Off an axis view it is a documented no-op.
  //
  // The directions are the view DIRECTION (target - position), and Halide's
  // StandardView is named after the side the camera looks FROM: Front puts the
  // camera on +Z looking -Z, so front's direction is (0,0,-1). Getting this table
  // backwards is silent — the camera simply does not move — which is why the
  // self-test below prints what it did.
  void toggleAxisView() {
    using SV = ember::render::StandardView;
    const ember::Vec3 dir = ember::normalize(ember::Vec3{camera.target() - camera.position()});
    struct Axis { ember::Vec3 d; SV opposite; const char* name; };
    static const Axis kAxes[6] = {
        {{0.0f, 0.0f, -1.0f}, SV::Back,   "Back"},    // currently Front  (camera +Z)
        {{0.0f, 0.0f, 1.0f},  SV::Front,  "Front"},   // currently Back   (camera -Z)
        {{-1.0f, 0.0f, 0.0f}, SV::Left,   "Left"},    // currently Right  (camera +X)
        {{1.0f, 0.0f, 0.0f},  SV::Right,  "Right"},   // currently Left   (camera -X)
        {{0.0f, -1.0f, 0.0f}, SV::Bottom, "Bottom"},  // currently Top    (camera +Y)
        {{0.0f, 1.0f, 0.0f},  SV::Top,    "Top"},     // currently Bottom (camera -Y)
    };
    for (const Axis& a : kAxes) {
      if (ember::dot(dir, a.d) > 0.996f) {        // within ~5 degrees of an axis
        camera.setView(a.opposite);
        logState(a.name);
        return;
      }
    }
    EMBER_LOG_INFO("nav numpad-9              ignored: the camera is not on an axis view");
  }

  void toggleProjection() {
    camera.setOrthographic(!camera.isOrthographic());
    logState(camera.isOrthographic() ? "orthographic" : "perspective");
  }

#if defined(EMBER_HAS_PLATFORM)
  // The navigation map is owned here, next to the input polling. Both the
  // physical-key path and EMBER_NAV_SELFTEST call this exact dispatcher.
  bool handleNavigationKey(ember::platform::Key key, bool ctrlDown,
                           bool emulateNumpad = false) {
    using Key = ember::platform::Key;
    using SV = ember::render::StandardView;
    if (emulateNumpad) {
      switch (key) {
        case Key::Num1: key = Key::KP_1; break;
        case Key::Num3: key = Key::KP_3; break;
        case Key::Num5: key = Key::KP_5; break;
        case Key::Num7: key = Key::KP_7; break;
        case Key::Num9: key = Key::KP_9; break;
        default: break;
      }
    }
    switch (key) {
      case Key::KP_1:
        setAxisView(ctrlDown ? SV::Back : SV::Front, ctrlDown ? "Back" : "Front");
        return true;
      case Key::KP_3:
        setAxisView(ctrlDown ? SV::Left : SV::Right, ctrlDown ? "Left" : "Right");
        return true;
      case Key::KP_7:
        setAxisView(ctrlDown ? SV::Bottom : SV::Top, ctrlDown ? "Bottom" : "Top");
        return true;
      case Key::KP_9:
        toggleAxisView();
        return true;
      case Key::KP_5:
        toggleProjection();
        return true;
#ifdef EMBER_HAS_SCENE
      case Key::KP_Decimal:
        frameSelection();
        return true;
      case Key::Home:
        frameAll();
        return true;
#endif
      default:
        return false;
    }
  }
#endif

#ifdef EMBER_HAS_SCENE
  void frame(const std::optional<ember::scene::WorldBounds>& b, const char* what) {
    if (!b) {
      EMBER_LOG_INFO("nav %-22s nothing to frame", what);
      return;
    }
    // D26: `frameSphere` (Halide, additive) replaced a frameBounds() call plus a
    // `radius * 0.5773503f` constant. That constant existed because frameBounds
    // fits the BOX's bounding sphere and WorldBounds hands us a sphere radius —
    // keeping it here meant the only place in core that knew how the camera's fit
    // works was App, and it would have broken silently if the margin changed. Now
    // App passes what it holds (centre + radius) and the fit maths stays in the
    // camera. Behaviour is unchanged: the self-test's framing distance is identical.
    camera.frameSphere(b->center, b->radius);
    logState(what);
  }
  void frameSelection() { frame(world->selectionBounds(), "frame selection"); }
  void frameAll()       { frame(world->bounds(), "frame all"); }
#endif
};
#endif // EMBER_HAS_RENDER

// Frame diagnostics for the "is a panel actually on screen?" question — the one
// thing no unit test can answer and nobody can eyeball over a wire.
//
// EMBER_FRAME_DUMP=<path.bmp> (env var, not a CLI flag: it moves no contract,
// same precedent as platform's EMBER_AUTO_CLOSE_SECONDS) writes the composited
// frame — 3D viewport, grid/axes, ImGui panels — and logs statistics that
// separate "a UI panel is drawn" from "a clear colour with a few grid lines":
//
//   bg        dominant colour (most common 5-bit-per-channel bucket)
//   distinct  distinct quantised colours present (UI text/AA => many)
//   non-bg    % of pixels that are not the dominant colour
//   bbox      bounding box of the non-background pixels (image coords)
//   quads     non-bg % per quadrant (tl/tr/bl/br; row 0 = top). A panel shows up
//             as localised high values; a bare grid does not.
//
// Measured, so it can be reproduced by someone other than the author. The file
// is a 24-bit BMP: openable in any Windows viewer, and readable by this team's
// tooling, so "a panel is visible" is checked by looking rather than asserting.
#ifdef EMBER_HAS_RENDER
void dumpFrameDiagnostics(ember::render::Renderer& renderer, const ember::Rect& area) {
  const char* path = std::getenv("EMBER_FRAME_DUMP");
  if (path == nullptr || *path == '\0') return;

  // D29: with Blender-style areas the 3D view is a RECTANGLE, and readViewport is
  // area-relative (it adds the renderer's viewport origin and clamps to the area
  // dims). So this reads the 3D AREA, not the whole window — the ImGui panels are
  // outside it by construction. Since area.w/h are the renderer's own dims, the
  // read is exact.
  const int width  = area.w;
  const int height = area.h;
  if (width <= 0 || height <= 0) {
    EMBER_LOG_WARN("frame dump: no valid 3D area yet (layout not built) — skipping");
    return;
  }

  std::vector<ember::u8> px;
  renderer.readViewport(0, 0, width, height, px);
  const std::size_t expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (width <= 0 || height <= 0 || px.size() < expected) {
    EMBER_LOG_WARN("frame dump: readViewport returned %llu bytes, expected %llu — skipping",
                   static_cast<unsigned long long>(px.size()),
                   static_cast<unsigned long long>(expected));
    return;
  }

  // 5 bits per channel = 32768 buckets: enough to separate UI greys from the
  // clear colour without a container per pixel.
  const int kBuckets = 32 * 32 * 32;
  std::vector<int> hist(static_cast<std::size_t>(kBuckets), 0);
  auto bucketOf = [](ember::u8 r, ember::u8 g, ember::u8 b) {
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
  };

  for (std::size_t i = 0; i < expected; i += 4) ++hist[bucketOf(px[i], px[i + 1], px[i + 2])];

  int bgBucket = 0;
  for (int i = 1; i < kBuckets; ++i)
    if (hist[i] > hist[bgBucket]) bgBucket = i;

  const int bgR = ((bgBucket >> 10) & 31) << 3;
  const int bgG = ((bgBucket >> 5) & 31) << 3;
  const int bgB = (bgBucket & 31) << 3;

  const int pixels = width * height;
  std::size_t nonBg = 0;
  int distinct = 0;
  int x0 = width, y0 = height, x1 = -1, y1 = -1;
  std::size_t quad[4] = {0, 0, 0, 0};
  std::size_t quadTotal[4] = {0, 0, 0, 0};

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4u;
      const int q = (y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1);
      ++quadTotal[q];
      if (bucketOf(px[i], px[i + 1], px[i + 2]) == bgBucket) continue;
      ++nonBg;
      ++quad[q];
      if (x < x0) x0 = x;
      if (y < y0) y0 = y;
      if (x > x1) x1 = x;
      if (y > y1) y1 = y;
    }
  }
  for (int i = 0; i < kBuckets; ++i)
    if (hist[i] > 0) ++distinct;

  auto pct = [](std::size_t part, std::size_t total) {
    return total > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
  };

  EMBER_LOG_INFO("frame dump: %dx%d bg=(%d,%d,%d) distinct=%d non-bg=%.2f%% bbox=(%d,%d)-(%d,%d)",
                 width, height, bgR, bgG, bgB, distinct, pct(nonBg, static_cast<std::size_t>(pixels)),
                 x1 >= 0 ? x0 : 0, y1 >= 0 ? y0 : 0, x1, y1);
  EMBER_LOG_INFO("frame dump: quadrant non-bg%% tl=%.2f tr=%.2f bl=%.2f br=%.2f (row 0 = top)",
                 pct(quad[0], quadTotal[0]), pct(quad[1], quadTotal[1]),
                 pct(quad[2], quadTotal[2]), pct(quad[3], quadTotal[3]));

  // 24-bit BMP, bottom-up rows padded to 4 bytes: no compression, no library,
  // and every image viewer (and our own tooling) opens it.
  const int rowStride = (width * 3 + 3) & ~3;
  const std::uint32_t imageBytes = static_cast<std::uint32_t>(rowStride) * static_cast<std::uint32_t>(height);
  const std::uint32_t fileBytes = 54u + imageBytes;

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    EMBER_LOG_WARN("frame dump: cannot open '%s' for writing", path);
    return;
  }

  unsigned char header[54] = {0};
  header[0] = 'B';
  header[1] = 'M';
  auto put32 = [&](int off, std::uint32_t v) {
    header[off + 0] = static_cast<unsigned char>(v & 0xFFu);
    header[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    header[off + 2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    header[off + 3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
  };
  auto put16 = [&](int off, std::uint16_t v) {
    header[off + 0] = static_cast<unsigned char>(v & 0xFFu);
    header[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
  };
  put32(2, fileBytes);
  put32(10, 54u);                                   // pixel data offset
  put32(14, 40u);                                   // BITMAPINFOHEADER size
  put32(18, static_cast<std::uint32_t>(width));
  put32(22, static_cast<std::uint32_t>(height));    // positive = bottom-up
  put16(26, 1u);                                    // planes
  put16(28, 24u);                                   // bits per pixel
  put32(34, imageBytes);
  out.write(reinterpret_cast<const char*>(header), 54);

  std::vector<char> row(static_cast<std::size_t>(rowStride), 0);
  for (int y = height - 1; y >= 0; --y) {           // BMP rows run bottom-up
    for (int x = 0; x < width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4u;
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<char>(px[i + 2]);  // B
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>(px[i + 1]);  // G
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>(px[i + 0]);  // R
    }
    out.write(row.data(), rowStride);
  }
  EMBER_LOG_INFO("frame dump: wrote '%s' (%dx%d 24-bit BMP)", path, width, height);
}
#endif // EMBER_HAS_RENDER

#if defined(EMBER_HAS_SCENE) && defined(EMBER_HAS_RENDER)
bool fillPacketFromActiveCamera(const ember::scene::World& world, int width, int height,
                                ember::RenderPacket& packet) {
  const ember::ObjectId id = world.activeCamera();
  const ember::scene::Object* object = world.get(id);
  if (!object || !object->camera.isCamera) return false;

  const ember::Mat4 cameraWorld = world.worldMatrix(id);
  const ember::Vec3 eye = cameraWorld.translation();
  ember::Vec3 forward = ember::normalize(
      ember::transformDirection(cameraWorld, ember::Vec3{0.0f, 0.0f, -1.0f}));
  ember::Vec3 up = ember::normalize(
      ember::transformDirection(cameraWorld, ember::Vec3{0.0f, 1.0f, 0.0f}));
  if (ember::lengthSq(forward) < ember::kEpsilon) forward = ember::Vec3{0.0f, 0.0f, -1.0f};
  if (ember::lengthSq(up) < ember::kEpsilon) up = ember::Vec3{0.0f, 1.0f, 0.0f};

  const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  packet.view = ember::lookAt(eye, eye + forward, up);
  if (object->camera.orthographic) {
    const float halfHeight = object->camera.orthoScale * 0.5f;
    const float halfWidth = halfHeight * (aspect > ember::kEpsilon ? aspect : 1.0f);
    packet.projection = ember::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
                                     object->camera.clipNear, object->camera.clipFar);
  } else {
    packet.projection = ember::perspective(object->camera.fovY, aspect,
                                           object->camera.clipNear, object->camera.clipFar);
  }
  packet.cameraPosition = eye;
  return true;
}
#endif

} // namespace

namespace ember {

struct App::Impl {
  AppConfig config;
  bool initialized = false;

#ifdef EMBER_HAS_PLATFORM
  platform::Window window;
#endif

#ifdef EMBER_HAS_RENDER
  render::Renderer renderer;
  bool rendererReady  = false;
  int  rendererWidth  = 0;
  int  rendererHeight = 0;

  // Resize the renderer only when the framebuffer actually changed.
  void syncFramebuffer(int width, int height) {
    if (!rendererReady || (width == rendererWidth && height == rendererHeight)) return;
    renderer.resize(width, height);
    rendererWidth  = width;
    rendererHeight = height;
  }
#endif

#ifdef EMBER_HAS_SCENE
  // The world is owned here because App is the composition root; scene owns the
  // objects inside it. Editor mutates it through World's public API only.
  scene::World world;

  // Object count at startup. Once this is non-zero, a frame that draws zero
  // triangles is a FAILURE (D18) — that is the check an empty scene could never
  // make, and the reason --scene/--demo-object exist.
  std::size_t expectedObjects = 0;
#if defined(EMBER_HAS_RENDER)
  bool lookThroughCamera = false;  // KP_0 toggles between the active camera and orbit view
  int cameraSelfTestFrames = 0;
#endif
#endif

#ifdef EMBER_HAS_EDITOR
  editor::Editor     ed;
  editor::UndoStack  undo;          // App owns it and passes it to draw() (D13)
  bool               editorReady = false;
#endif

  // D29: the last 3D-area rect relayed to platform's input rect, so we only tell
  // it when the layout actually changes (a per-frame set would make platform's
  // own staleness signal vacuous).
  Rect relayedArea{};

  double lastFrameSeconds = 0.0;

  // Set by run() when a runtime failure is detected; runApp() turns it into a
  // non-zero exit code. See the smoke-test contract in main.cpp.
  bool failed = false;

  // Middle-mouse gesture latch (item 5). WHICH gesture is decided at press time,
  // so releasing Shift mid-drag cannot switch orbit -> pan under the user's hand,
  // and the gesture survives the cursor crossing a panel.
  enum class DragMode { None, Orbit, Pan };
  DragMode drag = DragMode::None;
};

App::App() : impl_(std::make_unique<Impl>()) {}

App::~App() { shutdown(); }

bool App::init(const AppConfig& config) {
  if (impl_->initialized) {
    EMBER_LOG_WARN("App::init called twice; ignoring the second call");
    return true;
  }

  impl_->config = config;

#ifdef EMBER_HAS_PLATFORM
  platform::WindowConfig windowConfig;
  windowConfig.width  = config.width;
  windowConfig.height = config.height;
  windowConfig.title  = config.title;

  if (!impl_->window.create(windowConfig)) {
    EMBER_LOG_ERROR("window creation failed (%dx%d, \"%s\")", config.width, config.height,
                    config.title.c_str());
    return false;
  }

  // GL function loading happens with the context current inside Window::create
  // (see src/platform/README.md), so everything below may assume a live loader.
  EMBER_LOG_INFO("window created: %dx%d, \"%s\"", impl_->window.width(), impl_->window.height(),
                 config.title.c_str());
#else
  EMBER_LOG_WARN("platform module not present — running core-only (no window). "
                 "Build the platform module to get a real window (M0).");
#endif

#ifdef EMBER_HAS_RENDER
  impl_->rendererReady = impl_->renderer.init();
  if (impl_->rendererReady) {
    impl_->renderer.resize(impl_->window.width(), impl_->window.height());
    impl_->rendererWidth  = impl_->window.width();
    impl_->rendererHeight = impl_->window.height();
    EMBER_LOG_INFO("renderer ready (%dx%d)", impl_->rendererWidth, impl_->rendererHeight);
  } else {
    // Fatal on purpose (WHAT-REMAINS.md: "surface failure, do not ignore it"):
    // a renderer that did not initialise means a blank window, which is exactly
    // the failure a smoke test must catch rather than hide.
    EMBER_LOG_ERROR("renderer init failed — cannot draw; failing App::init");
    return false;
  }
#endif

#ifdef EMBER_HAS_EDITOR
  // ImGui's backends need the GLFW handle and a live loader; editor owns the
  // whole ImGui frame from here on (D13).
  impl_->editorReady = impl_->ed.init(impl_->window.nativeHandle());
  if (!impl_->editorReady) EMBER_LOG_ERROR("editor init failed — continuing without UI");
#endif

#ifdef EMBER_HAS_SCENE
  // --- seed the world (D18): what makes triangles > 0 testable ------------
  // Both paths go through the same World API the editor's Add menu uses, so the
  // smoke test exercises the real draw path, not a special-cased one.
  if (!config.scenePath.empty()) {
    if (!scene::load(impl_->world, config.scenePath)) {
      // A requested scene that cannot load is a hard failure: the user asked for
      // something specific and we must not silently run an empty world.
      EMBER_LOG_ERROR("--scene: failed to load '%s'", config.scenePath.c_str());
      return false;
    }
    EMBER_LOG_INFO("--scene: loaded '%s' (%llu object(s))", config.scenePath.c_str(),
                   static_cast<unsigned long long>(impl_->world.objects().size()));
  }

  if (config.demoObject) {
    const ObjectId id = impl_->world.createPrimitive(scene::Primitive::Cube, "SmokeCube");
    EMBER_LOG_INFO("--demo-object: created a Cube (id %u)", id.value);
  }

  impl_->expectedObjects = 0;
  for (const scene::Object& object : impl_->world.objects()) {
    // Only a VISIBLE, MESHED object must produce triangles. An unmeshed group or
    // a hidden object legitimately draws nothing — repro.ember is exactly that
    // case (`mesh none`), and counting all objects made the check fail on it.
    if (object.hasMesh && object.visible) ++impl_->expectedObjects;
  }
  if (impl_->expectedObjects > 0) {
    EMBER_LOG_INFO("world seeded with %llu object(s), %llu with visible geometry — a frame drawing "
                   "0 triangles is now a FAILURE",
                   static_cast<unsigned long long>(impl_->world.objects().size()),
                   static_cast<unsigned long long>(impl_->expectedObjects));
  } else if (!impl_->world.objects().empty()) {
    EMBER_LOG_INFO("world seeded with %llu object(s), none with visible geometry — 0 triangles is "
                   "expected and is NOT a failure",
                   static_cast<unsigned long long>(impl_->world.objects().size()));
  }
#endif

  impl_->initialized = true;
  return true;
}

void App::run() {
  if (!impl_->initialized) {
    EMBER_LOG_ERROR("App::run called before a successful App::init");
    return;
  }

#ifdef EMBER_HAS_PLATFORM
  impl_->lastFrameSeconds = platform::timeSeconds();
  unsigned long long frames = 0;
  // Frame budget for the headless smoke flag (--frames N, decision D14).
  const unsigned long long frameLimit =
      impl_->config.frames > 0 ? static_cast<unsigned long long>(impl_->config.frames) : 0ULL;

  // Navigation actions, shared by the key bindings in the loop below and by the
  // headless self-test, so both exercise ONE implementation (item 5).
  NavActions nav{impl_->renderer.camera()};
#ifdef EMBER_HAS_SCENE
  nav.world = &impl_->world;
#endif

  // Headless navigation self-test (EMBER_NAV_SELFTEST=1): exercise one of every
  // navigation ACTION before the loop. The key->action mapping still needs a
  // human, but the actions themselves — axis views, the numpad-9 opposite, the
  // ortho toggle, and the framing maths — get machine evidence instead of my
  // word. Env var, no CLI flag, no contract change (same precedent as
  // platform's EMBER_AUTO_CLOSE_SECONDS).
  if (std::getenv("EMBER_NAV_SELFTEST") != nullptr) {
    EMBER_LOG_INFO("nav self-test: begin");
    nav.logState("start");
    const auto assertView = [&](float yaw, float pitch, const char* label) {
      const float yawError = std::remainder(nav.camera.yaw() - yaw, ember::kTwoPi);
      if (std::fabs(yawError) > 2e-3f || std::fabs(nav.camera.pitch() - pitch) > 2e-3f) {
        EMBER_LOG_ERROR("nav self-test FAILED: %s view yaw=%.3f pitch=%.3f", label,
                        double(nav.camera.yaw()), double(nav.camera.pitch()));
        impl_->failed = true;
      }
    };
    using Key = platform::Key;
    nav.handleNavigationKey(Key::KP_1, false);
    assertView(0.0f, 0.0f, "KP_1 front");
    nav.handleNavigationKey(Key::KP_1, true);   // Ctrl+KP_1 -> Back
    assertView(ember::kPi, 0.0f, "Ctrl+KP_1 back");
    nav.handleNavigationKey(Key::KP_3, false);  // KP_3 -> Right
    assertView(ember::kHalfPi, 0.0f, "KP_3 right");
    nav.handleNavigationKey(Key::KP_9, false);  // KP_9: Right -> Left
    assertView(-ember::kHalfPi, 0.0f, "KP_9 left");
    nav.handleNavigationKey(Key::KP_7, false);  // KP_7 -> Top
    assertView(0.0f, ember::kHalfPi, "KP_7 top");
    nav.handleNavigationKey(Key::KP_9, false);  // KP_9: Top -> Bottom
    assertView(0.0f, -ember::kHalfPi, "KP_9 bottom");
    nav.handleNavigationKey(Key::KP_5, false);  // KP_5 -> orthographic
    if (!nav.camera.isOrthographic()) impl_->failed = true;
    nav.handleNavigationKey(Key::KP_5, false);  // KP_5 -> perspective
    if (nav.camera.isOrthographic()) impl_->failed = true;
    const float topRowYaw = nav.camera.yaw();
    if (nav.handleNavigationKey(Key::Num1, false) ||
        !ember::nearlyEqual(nav.camera.yaw(), topRowYaw)) impl_->failed = true;
    nav.handleNavigationKey(Key::Num1, false, true);
    assertView(0.0f, 0.0f, "Emulate Numpad 1 front");
    nav.handleNavigationKey(Key::Num3, false, true);
    assertView(ember::kHalfPi, 0.0f, "Emulate Numpad 3 right");
#ifdef EMBER_HAS_SCENE
    nav.handleNavigationKey(Key::Home, false);
    nav.handleNavigationKey(Key::KP_Decimal, false);  // no selection: safe no-op
    if (!impl_->world.objects().empty()) {
      impl_->world.select(impl_->world.objects().front().id);
      nav.handleNavigationKey(Key::KP_Decimal, false);
    }
#endif
    EMBER_LOG_INFO("nav self-test: end");
  }

#if defined(EMBER_HAS_SCENE) && defined(EMBER_HAS_RENDER)
  if (std::getenv("EMBER_CAMERA_SELFTEST") != nullptr) {
    Transform pose;
    pose.position = Vec3{2.0f, 3.0f, 4.0f};
    const ObjectId id = impl_->world.createCamera("CameraSelfTest", pose);
    CameraSettings settings;
    settings.isCamera = true;
    settings.fovY = ember::radians(90.0f);
    settings.clipNear = 0.25f;
    settings.clipFar = 300.0f;
    settings.orthographic = false;
    settings.orthoScale = 8.0f;
    if (id.valid() && impl_->world.setCameraSettings(id, settings)) {
      impl_->world.setActiveCamera(id);
      impl_->lookThroughCamera = true;
      EMBER_LOG_INFO("camera self-test: active camera %u armed", id.value);
    } else {
      impl_->failed = true;
      EMBER_LOG_ERROR("camera self-test: could not create/configure active camera");
    }
  }
#endif

  while (!impl_->window.shouldClose() && (frameLimit == 0ULL || frames < frameLimit)) {
    impl_->window.pollEvents();

    const double now = platform::timeSeconds();
    const double dt  = now - impl_->lastFrameSeconds;
    impl_->lastFrameSeconds = now;
    (void)dt;

    const int width  = impl_->window.width();
    const int height = impl_->window.height();

    // --- D29: the 3D view is an AREA, not the window ------------------------
    // The editor owns the layout, so it owns the rect; App relays it to the three
    // consumers, in the one place that sees them all:
    //   1. the renderer (scopes the GL viewport),
    //   2. the packet's width/height (projection aspect comes from them — feeding
    //      the WINDOW size would stretch the scene inside a non-square area),
    //   3. platform's input rect, converted to WINDOW coordinates (D17).
    // Invalid (layout not built yet, or a core-only build) falls back to the whole
    // window, so the first frame and the no-editor path behave exactly as before.
    Rect area{0, 0, width, height};
    Rect owningEditorArea{};
#if defined(EMBER_HAS_EDITOR)
    if (impl_->editorReady) {
      const Rect edited = impl_->ed.viewportRect();
      if (edited.valid()) area = edited;
      owningEditorArea = impl_->ed.viewportAreaRect();
    }
#endif

#if defined(EMBER_HAS_RENDER)
    impl_->syncFramebuffer(width, height);
    impl_->renderer.setViewportRect(area, owningEditorArea);
#endif

#if defined(EMBER_HAS_PLATFORM)
    // Only when it CHANGES: platform's rectIsStaleFor() compares the captured
    // window size against the current one, and a per-frame write would mean it
    // could never observe a stale rect — the very thing defect 1 was about.
    if (area.x != impl_->relayedArea.x || area.y != impl_->relayedArea.y ||
        area.w != impl_->relayedArea.w || area.h != impl_->relayedArea.h) {
      const int windowW = impl_->window.windowWidth();
      const int windowH = impl_->window.windowHeight();
      if (windowW > 0 && windowH > 0 && width > 0 && height > 0) {
        const float toWindowX = static_cast<float>(windowW) / static_cast<float>(width);
        const float toWindowY = static_cast<float>(windowH) / static_cast<float>(height);
        platform::input().setViewportRect(
            static_cast<int>(std::lround(static_cast<float>(area.x) * toWindowX)),
            static_cast<int>(std::lround(static_cast<float>(area.y) * toWindowY)),
            static_cast<int>(std::lround(static_cast<float>(area.w) * toWindowX)),
            static_cast<int>(std::lround(static_cast<float>(area.h) * toWindowY)), windowW,
            windowH);
      }
      impl_->relayedArea = area;
    }
#endif

#ifdef EMBER_HAS_RENDER
    impl_->syncFramebuffer(width, height);

    // --- camera: App polls input, render builds the matrices (D1) -----------
    // Viewport ownership comes from the EDITOR, not from platform's rectangle.
    // That was the defect-1 fix: the rect was captured once at creation, so it
    // went stale on resize/fullscreen and dragging over the Outliner/Properties
    // spun the camera through the panel. Platform now resyncs the rect on both
    // size callbacks (Kiln) AND self-checks it every frame, but the camera gate
    // still belongs to the editor, because only it knows where the panels are:
    // Editor::wantsViewportFocus() (ImGui WantCaptureMouse), with the platform
    // rect merely as the no-editor fallback.
    //
    // A drag is LATCHED: once it starts over the viewport it keeps orbiting
    // until the button is released, even if the cursor crosses a panel —
    // otherwise every drag dies the moment the pointer leaves the viewport.
    render::OrbitCamera& camera = impl_->renderer.camera();
    {
      platform::Input& in = platform::input();

      const bool middleDown = in.mouseDown(platform::MouseButton::Middle);
      const bool shiftDown  = in.keyDown(platform::Key::LeftShift) ||
                              in.keyDown(platform::Key::RightShift);
      const bool ctrlDown   = in.keyDown(platform::Key::LeftCtrl) ||
                              in.keyDown(platform::Key::RightCtrl);
#if defined(EMBER_HAS_EDITOR)
      const bool viewportHovered =
          impl_->editorReady ? impl_->ed.wantsViewportFocus() : in.cursorOverViewport();
      const bool emulateNumpad = impl_->editorReady && impl_->ed.emulateNumpad();
#else
      const bool viewportHovered = in.cursorOverViewport();
      const bool emulateNumpad = false;
#endif

      if (impl_->lookThroughCamera) {
        // Camera view is authored by the scene object; orbit gestures are paused
        // rather than silently modifying the hidden orbit-camera state.
        impl_->drag = Impl::DragMode::None;
      } else {
        // --- drag gestures: middle = orbit, Shift+middle = pan (Blender) ------
        if (middleDown && impl_->drag == Impl::DragMode::None && viewportHovered) {
          impl_->drag = shiftDown ? Impl::DragMode::Pan : Impl::DragMode::Orbit;
        } else if (!middleDown) {
          impl_->drag = Impl::DragMode::None;
        }

        Vec2 delta = in.mouseDelta();
        if (impl_->drag == Impl::DragMode::Orbit) {
          // D27: honour the editor's persisted "invert orbit direction" preference.
          // Negated HERE rather than inside the camera: the camera is deliberately
          // input-agnostic (its header says so), and App is the only place that
          // polls input anyway, so the preference costs one branch and no new
          // camera API. Both axes are mirrored — if the preference is meant to flip
          // only the horizontal sense, that is a one-line change here.
#if defined(EMBER_HAS_EDITOR)
          if (impl_->editorReady && impl_->ed.invertOrbit()) delta = Vec2{-delta.x, -delta.y};
#endif
          camera.orbit(delta.x, delta.y);
        } else if (impl_->drag == Impl::DragMode::Pan) {
          // pan() takes FRAMEBUFFER pixels + framebuffer height; cursor deltas
          // arrive in WINDOW coordinates (D17/D20), so scale once, here.
          const int windowW = impl_->window.windowWidth();
          const int windowH = impl_->window.windowHeight();
          const float scaleX =
              windowW > 0 ? static_cast<float>(width) / static_cast<float>(windowW) : 1.0f;
          const float scaleY =
              windowH > 0 ? static_cast<float>(height) / static_cast<float>(windowH) : 1.0f;
          camera.pan(delta.x * scaleX, delta.y * scaleY, static_cast<float>(height));
        }

        // Zoom is a viewport gesture too: scrolling over a panel must not dolly.
        const Vec2 scroll = in.scrollDelta();
        if (scroll.y != 0.0f && (viewportHovered || impl_->drag != Impl::DragMode::None)) {
          camera.dolly(scroll.y);
        }

        // --- Blender navigation keys: edge-triggered, gated on the SAME hover --
        if (viewportHovered) {
          using Key = platform::Key;
          constexpr Key kNavigationKeys[] = {Key::KP_1, Key::KP_3, Key::KP_5, Key::KP_7,
                                              Key::KP_9, Key::KP_Decimal, Key::Home};
          for (Key key : kNavigationKeys)
            if (in.keyPressed(key)) nav.handleNavigationKey(key, ctrlDown);
          if (emulateNumpad) {
            constexpr Key kTopRowNavigationKeys[] = {Key::Num1, Key::Num3, Key::Num5,
                                                       Key::Num7, Key::Num9};
            for (Key key : kTopRowNavigationKeys)
              if (in.keyPressed(key)) nav.handleNavigationKey(key, ctrlDown, true);
          }
        }
      }

#if defined(EMBER_HAS_SCENE) && defined(EMBER_HAS_RENDER)
      // Blender: KP_0 toggles camera view. The active scene camera is explicit;
      // it is never inferred from the selection or the outliner row.
      const bool cameraViewPressed = in.keyPressed(platform::Key::KP_0) ||
          (emulateNumpad && in.keyPressed(platform::Key::Num0));
      if (viewportHovered && cameraViewPressed) {
        if (impl_->lookThroughCamera) {
          impl_->lookThroughCamera = false;
          EMBER_LOG_INFO("view: left active camera view");
        } else {
          const scene::Object* active = impl_->world.get(impl_->world.activeCamera());
          if (active && active->camera.isCamera) {
            impl_->lookThroughCamera = true;
            EMBER_LOG_INFO("view: looking through camera '%s'", active->name.c_str());
          } else {
            EMBER_LOG_WARN("view: no active camera; KP_0 has no camera to look through");
          }
        }
      }
#endif
    }

    // --- packet: scene produces it, App adds the camera (D2, D29) ----------
    RenderPacket packet;
#ifdef EMBER_HAS_SCENE
    packet = impl_->world.buildPacket(area.w, area.h);
#endif
    packet.width            = area.w;
    packet.height           = area.h;
    packet.view             = camera.view();
    packet.projection       = camera.projection(area.h > 0 ? float(area.w) / float(area.h) : 1.0f);
    packet.cameraPosition   = camera.position();
#if defined(EMBER_HAS_SCENE) && defined(EMBER_HAS_RENDER)
    bool usedSceneCamera = false;
    if (impl_->lookThroughCamera) {
      usedSceneCamera = fillPacketFromActiveCamera(impl_->world, area.w, area.h, packet);
      if (!usedSceneCamera) {
        impl_->lookThroughCamera = false;
        EMBER_LOG_WARN("view: active camera disappeared; restored orbit view");
      }
    }
    if (std::getenv("EMBER_CAMERA_SELFTEST") != nullptr) {
      const int stage = impl_->cameraSelfTestFrames;
      const float aspect = area.h > 0 ? static_cast<float>(area.w) / static_cast<float>(area.h) : 1.0f;
      const bool common = usedSceneCamera &&
          ember::nearlyEqual(packet.cameraPosition.x, 2.0f) &&
          ember::nearlyEqual(packet.cameraPosition.y, 3.0f) &&
          ember::nearlyEqual(packet.cameraPosition.z, 4.0f) &&
          ember::nearlyEqual(packet.view.m[12], -2.0f) &&
          ember::nearlyEqual(packet.view.m[13], -3.0f) &&
          ember::nearlyEqual(packet.view.m[14], -4.0f);
      const bool perspectiveExpected =
          ember::nearlyEqual(packet.projection.m[0], 1.0f / aspect) &&
          ember::nearlyEqual(packet.projection.m[5], 1.0f) &&
          ember::nearlyEqual(packet.projection.m[10], (300.0f + 0.25f) / (0.25f - 300.0f)) &&
          ember::nearlyEqual(packet.projection.m[14], (2.0f * 300.0f * 0.25f) / (0.25f - 300.0f));
      const bool orthographicExpected =
          ember::nearlyEqual(packet.projection.m[0], 2.0f / (8.0f * aspect)) &&
          ember::nearlyEqual(packet.projection.m[5], 2.0f / 8.0f) &&
          ember::nearlyEqual(packet.projection.m[10], -2.0f / (300.0f - 0.25f)) &&
          ember::nearlyEqual(packet.projection.m[14], -(300.0f + 0.25f) / (300.0f - 0.25f));
      const bool expected = common && (stage == 0 ? perspectiveExpected : orthographicExpected);
      if (!expected) {
        EMBER_LOG_ERROR("camera self-test FAILED: scene camera did not drive RenderPacket");
        impl_->failed = true;
      } else {
        if (stage == 0) {
          scene::Object* active = impl_->world.get(impl_->world.activeCamera());
          CameraSettings orthographic = active->camera;
          orthographic.orthographic = true;
          if (!impl_->world.setCameraSettings(active->id, orthographic)) {
            EMBER_LOG_ERROR("camera self-test FAILED: could not switch projection mode");
            impl_->failed = true;
          } else {
            EMBER_LOG_INFO("camera self-test PASS: active camera perspective drives packet");
          }
        } else if (stage == 1) {
          EMBER_LOG_INFO("camera self-test PASS: active camera orthographic drives packet");
        }
        ++impl_->cameraSelfTestFrames;
      }
    }
#endif

#if defined(EMBER_HAS_EDITOR) && defined(EMBER_HAS_SCENE) && defined(EMBER_HAS_RENDER)
    if (impl_->editorReady) {
      packet.editOverlay = impl_->ed.editOverlayState(impl_->world);
      const Rect currentViewport = impl_->ed.viewportRect();
      const bool viewportAreaAvailable = currentViewport.w > 1 && currentViewport.h > 1 &&
                                         width > 1 && height > 1;
      impl_->ed.prepareViewportRender(packet, impl_->renderer, now, viewportAreaAvailable);
    }
#endif

    if (impl_->rendererReady) impl_->renderer.renderFrame(packet);
#endif

    // --- editor: owns the ImGui frame, mutates the world through World -------
#ifdef EMBER_HAS_EDITOR
    if (impl_->editorReady) {
#ifdef EMBER_HAS_RENDER
      impl_->ed.draw(impl_->world, impl_->renderer, impl_->renderer.lastStats(), impl_->undo);
#else
      (void)impl_->world;
      (void)impl_->undo;
#endif

      // The editor's transform self-test reports through Editor::selfTestStatus():
      // 0 = not run / still running, 1 = PASS, -1 = FAIL. A FAIL that only prints
      // would leave `EMBER_SELFTEST_TRANSFORM=1 Ember3D.exe --frames 70 --exit`
      // exiting 0, i.e. a gate that cannot fail — the same hole as a smoke run that
      // swallows a broken shader. Negative → fail the run (exit 2 through failed()).
      if (impl_->ed.selfTestStatus() < 0) {
        EMBER_LOG_ERROR("editor transform self-test FAILED (EMBER_SELFTEST_TRANSFORM) — failing the run");
        impl_->failed = true;
      }
    }
#endif

    // --- measure before presenting: what is actually on screen -------------
    // Once, on the last frame of a bounded run (or the first of an unbounded
    // one), so a smoke run produces exactly one image to inspect.
#ifdef EMBER_HAS_RENDER
    const bool dumpNow = (frameLimit > 0ULL) ? (frames + 1ULL == frameLimit) : (frames == 0ULL);
    if (impl_->rendererReady && dumpNow) dumpFrameDiagnostics(impl_->renderer, area);
#endif

    impl_->window.swapBuffers();
    ++frames;
  }

  EMBER_LOG_INFO("frame loop ended after %llu frames", frames);
#ifdef EMBER_HAS_RENDER
  // Report the LAST frame the renderer actually drew, which is the number the
  // M1 smoke test cares about (an empty scene should show drawCalls == 1:
  // grid + axes overlay, no items).
  if (impl_->rendererReady) {
    const RenderStats stats = impl_->renderer.lastStats();
    EMBER_LOG_INFO("last frame: drawCalls=%d triangles=%d frameMs=%.3f", stats.drawCalls,
                   stats.triangles, stats.frameMs);
    // A renderer that ran but drew nothing is a RUNTIME failure the 962 unit
    // checks cannot see — which is the entire reason this path exists.
    if (stats.drawCalls == 0) {
      EMBER_LOG_ERROR("renderer reported 0 draw calls in the last frame — nothing was drawn");
      impl_->failed = true;
    }
#ifdef EMBER_HAS_SCENE
    // The stronger check (D18): with VISIBLE MESHED geometry seeded, triangles
    // must be non-zero. An empty scene (or one holding only groups/empties and
    // hidden objects) can never fail this, which is why --scene and
    // --demo-object exist.
    if (impl_->expectedObjects > 0 && stats.triangles == 0) {
      EMBER_LOG_ERROR("world has %llu visible meshed object(s) but the last frame drew 0 triangles — "
                      "the mesh draw path is broken",
                      static_cast<unsigned long long>(impl_->expectedObjects));
      impl_->failed = true;
    }
#endif
  }
#endif

#ifdef EMBER_HAS_EDITOR
  // A self-test that was REQUESTED but never produced a verdict is a broken gate, not a
  // quiet run: a FAIL would go unreported too, which is the hole this check exists to
  // close. Nobody sets EMBER_SELFTEST_TRANSFORM except to get a verdict, so ending
  // without one fails — including when the frame budget was too short to reach the
  // summary frame (frame 66; the registration owns the budget, >= 70).
  if (std::getenv("EMBER_SELFTEST_TRANSFORM") != nullptr &&
      (!impl_->editorReady || impl_->ed.selfTestStatus() == 0)) {
    EMBER_LOG_ERROR("EMBER_SELFTEST_TRANSFORM was set but no verdict was produced (%s) — "
                    "failing the run; the summary frame needs a large enough --frames",
                    impl_->editorReady ? "status 0" : "editor not ready");
    impl_->failed = true;
  }
#endif

  // The smoke path must fail if the frame budget was not met, otherwise a
  // crashed-early loop would look like a pass.
  if (frameLimit > 0ULL && frames < frameLimit) {
    EMBER_LOG_ERROR("frame budget was %llu but only %llu frames ran", frameLimit, frames);
    impl_->failed = true;
  }
#else
  EMBER_LOG_WARN("nothing to run without the platform module; exiting immediately");
#endif
}

void App::shutdown() {
  if (!impl_) return;

#if defined(EMBER_HAS_EDITOR)
  // Reverse init order: editor (ImGui + backends) needs the GL context alive.
  if (impl_->editorReady) {
    impl_->ed.shutdown();
    impl_->editorReady = false;
  }
#endif

#if defined(EMBER_HAS_RENDER)
  if (impl_->rendererReady) {
    impl_->renderer.shutdown();
    impl_->rendererReady = false;
  }
#endif

#ifdef EMBER_HAS_PLATFORM
  if (impl_->initialized) impl_->window.destroy();
#endif

  if (impl_->initialized) EMBER_LOG_INFO("App shut down cleanly");
  impl_->initialized = false;
}

bool App::failed() const { return impl_ && impl_->failed; }

int runApp(const AppConfig& config) {
  App app;

  if (!app.init(config)) {
    app.shutdown();
    return 1;                  // window or renderer could not start
  }

  app.run();
  const bool failed = app.failed();
  app.shutdown();
  return failed ? 2 : 0;       // 2 = it ran, but the frame produced nothing
}

} // namespace ember

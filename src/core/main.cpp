// Ember3D — thin entry point.
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §2 (main.cpp) and README.md in
// src/core: "parse the command line, construct App, init, run, shutdown."
//
// No wiring lives here — App.cpp is the single place the modules meet.
//
// The smoke path (`--frames N --exit`) is the project's only automated way to
// exercise the runtime: it takes the SAME code path as the interactive loop
// (App::run with a frame budget), so a broken shader/VAO/ImGui init fails here.
//
// Exit codes (Verity's CTest depends on these):
//   0  ran, and the last frame drew something
//   1  startup failed: window or renderer could not initialise
//   2  ran but the frame produced nothing (0 draw calls), or the frame budget
//      was not met, or the editor's transform self-test reported FAIL
//      (EMBER_SELFTEST_TRANSFORM=1)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/App.h"
#include "core/Log.h"

namespace {

void printUsage() {
  std::printf(
      "Ember3D - a Blender-like 3D editor\n"
      "\n"
      "usage: Ember3D [options]\n"
      "\n"
      "  --width  <px>     window width  (default 1280)\n"
      "  --height <px>     window height (default 720)\n"
      "  --title  <text>   window title  (default \"Ember3D\")\n"
      "  --log    <level>  trace|debug|info|warn|error|off (default info)\n"
      "  --frames <n>      run at most n frames, then shut down cleanly\n"
      "  --exit            require --frames; exit non-zero if the budget is not met\n"
      "  --scene <file>    load a .ember scene before the loop (via SceneIO)\n"
      "  --demo-object     add one cube via World::createPrimitive (same call as the menu)\n"
      "  -h, --help        show this help and exit\n"
      "\n"
      "smoke test:  Ember3D --frames 60 --exit --demo-object\n"
      "             (exit 0 = the frame path works AND the cube drew triangles)\n");
}

// Result of trying to consume one argv element.
enum class ParseResult { NotMine, Handled, Failed };

ParseResult parseOption(int argc, char** argv, int& i, ember::AppConfig& cfg, bool& requireExit) {
  const std::string arg = argv[i];
  auto value = [&](const char* name) -> const char* {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "error: %s needs a value\n", name);
      return nullptr;
    }
    return argv[++i];
  };

  if (arg == "--width" || arg == "--height" || arg == "--frames") {
    const char* v = value(arg.c_str());
    if (!v) return ParseResult::Failed;
    const int n = std::atoi(v);
    if (n <= 0) {
      // Hard failure: silently ignoring this would fall through into the
      // interactive loop, i.e. a smoke run that hangs instead of failing.
      std::fprintf(stderr, "error: %s must be a positive integer (got '%s')\n", arg.c_str(), v);
      return ParseResult::Failed;
    }
    if (arg == "--width")       cfg.width  = n;
    else if (arg == "--height") cfg.height = n;
    else                        cfg.frames = n;
    return ParseResult::Handled;
  }
  if (arg == "--title") {
    const char* v = value("--title");
    if (!v) return ParseResult::Failed;
    cfg.title = v;
    return ParseResult::Handled;
  }
  if (arg == "--scene") {
    // Load a .ember scene through SceneIO before the loop starts (D18) — the same
    // path a File>Open would take, so the smoke test exercises real geometry.
    const char* v = value("--scene");
    if (!v) return ParseResult::Failed;
    cfg.scenePath = v;
    return ParseResult::Handled;
  }
  if (arg == "--demo-object") {
    // Spawn one primitive via World::createPrimitive — the call the editor's Add
    // menu makes — so a bounded run has geometry to prove the draw path works.
    cfg.demoObject = true;
    return ParseResult::Handled;
  }
  if (arg == "--log") {
    const char* v = value("--log");
    if (!v) return ParseResult::Failed;
    struct Named { const char* name; ember::log::Level level; };
    static const Named kLevels[] = {
        {"trace", ember::log::Level::Trace}, {"debug", ember::log::Level::Debug},
        {"info", ember::log::Level::Info},   {"warn", ember::log::Level::Warn},
        {"error", ember::log::Level::Error}, {"off", ember::log::Level::Off},
    };
    for (const Named& l : kLevels) {
      if (std::strcmp(v, l.name) == 0) {
        ember::log::setLevel(l.level);
        return ParseResult::Handled;
      }
    }
    std::fprintf(stderr, "error: unknown log level '%s'\n", v);
    return ParseResult::Failed;
  }
  if (arg == "--exit") {
    // The App already exits when a frame budget is set; --exit makes the intent
    // explicit and is required by the documented smoke invocation.
    requireExit = true;
    return ParseResult::Handled;
  }

  return ParseResult::NotMine;
}

} // namespace

int main(int argc, char** argv) {
  ember::AppConfig cfg;
  bool requireExit = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage();
      return EXIT_SUCCESS;
    }
    switch (parseOption(argc, argv, i, cfg, requireExit)) {
      case ParseResult::Handled:  continue;
      case ParseResult::Failed:   return EXIT_FAILURE;
      case ParseResult::NotMine:  break;
    }

    std::fprintf(stderr, "error: unknown option '%s'\n\n", arg.c_str());
    printUsage();
    return EXIT_FAILURE;
  }

  if (requireExit && cfg.frames <= 0) {
    std::fprintf(stderr, "error: --exit needs --frames <n>\n");
    printUsage();
    return EXIT_FAILURE;
  }

  return ember::runApp(cfg);
}

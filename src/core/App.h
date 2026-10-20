// Ember3D — the composition root.
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §4.6.
//
// App is the ONLY place the modules are wired together. Every other module
// knows only `core`. If you need something wired here, ask the Lead.

#pragma once

#include <memory>
#include <string>

namespace ember {

struct AppConfig {
  int         width  = 1280;
  int         height = 720;
  std::string title  = "Ember3D";

  // Headless smoke-test budget (decision D14): run at most this many frames and
  // then shut down cleanly. 0 (the default) = run until the window closes. Only
  // main.cpp and App read it; no module constructs an AppConfig.
  int frames = 0;

  // Seed the world for a bounded run (decision D18), so the runtime test can
  // prove geometry actually draws (triangles > 0) instead of passing on an empty
  // scene forever. Both paths use the same World API the editor's menu uses, so
  // they exercise the real draw path rather than a special one.
  std::string scenePath;            // load a .ember file through scene::load()
  bool        demoObject = false;   // spawn one primitive (a cube)
};

class App {
public:
  App();
  ~App();

  App(const App&)            = delete;
  App& operator=(const App&) = delete;

  // Creates the window (and, as modules land, initialises render/scene/editor).
  // Returns false — with the reason logged — if the app cannot start.
  bool init(const AppConfig&);

  // Runs the frame loop until the window closes.
  void run();

  // Safe to call more than once, and safe to call after a failed init().
  void shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // True when the last run() hit a runtime failure (nothing drawn). Private:
  // run() keeps the §4.6 `void` signature; runApp() turns this into an exit code.
  bool failed() const;
  friend int runApp(const AppConfig&);
};

// Convenience for main.cpp: init, run, shutdown, with the exit code.
int runApp(const AppConfig&);

} // namespace ember

# Ember3D

Ember3D is an early-stage, Blender-inspired 3D editor built with C++20, CMake,
GLFW, Dear ImGui, and OpenGL 4.5. Its Raster viewport and CPU path-tracing
prototype are separate backends; this project does **not** claim Blender, Cycles,
or Eevee parity.

## Highlights

- Docked editor areas, viewport navigation, Object/Edit modes, and modal transforms.
- Raster/Cycles A/B wipe: compare both renderers in one Rendered viewport with a
  shared camera and draggable divider.
- CPU path-tracing preview with progressive and per-pixel adaptive sampling,
  image textures, environment sampling, and optional denoising.
- `.ember` v8 scenes with editable mesh and per-face material-slot persistence.
- Dependency-free PPM/PFM/BMP image input and PFM/PPM preview output.

These are prototypes with known gaps. In particular, temporal AA, node-based
materials, many Blender operators, broad image-format support, and a full-resolution
production render workflow are not implemented. See [WHAT-REMAINS.md](WHAT-REMAINS.md)
and [docs/RENDERING-ROADMAP.md](docs/RENDERING-ROADMAP.md) for scope and limitations.

## Build (Windows / MSVC)

Requirements: CMake 3.25+, Visual Studio C++ tools, and an OpenGL 4.5-capable GPU.

```powershell
cmake -B build
cmake --build build --config Release
.\bin\Ember3D.exe --frames 60 --exit --demo-object
```

Run the registered tests with:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

GPU tests require a working graphics context. Recent verification limitations and
test results are recorded in [BUILD-STATUS.md](BUILD-STATUS.md).

## License

Ember3D is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party
components and their licenses are in [`vendor/`](vendor/).

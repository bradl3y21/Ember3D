find_package(Threads REQUIRED)

add_library(ember_render STATIC
    Renderer.cpp
    PathTracer.cpp
    PathImageIO.cpp
    PathBsdf.cpp
    PathSampling.cpp
    PathDenoiser.cpp
    OfflineRenderJob.cpp
    TextureImage.cpp
    Shader.cpp
    Mesh.cpp
    Material.cpp
    Camera.cpp
)
target_include_directories(ember_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ember_render PUBLIC ember_core glad Threads::Threads)

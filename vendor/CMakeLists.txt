# Third-party sources, committed so builds work offline. Exactly one copy of
# each library; do not add a second vendor tree.
#
#   glad   — GL 4.5 core loader, generated once and committed (never at build
#            time). Target: glad (alias glad::glad).
#   glfw   — windowing/context. Target: glfw (upstream target name).
#   imgui  — Dear ImGui, docking branch, with the GLFW + OpenGL3 backends
#            compiled in. Target: imgui (alias imgui::imgui).

# --------------------------------------------------------------------------
# glad
# --------------------------------------------------------------------------
add_library(glad STATIC "${CMAKE_CURRENT_SOURCE_DIR}/glad/src/gl.c")
add_library(glad::glad ALIAS glad)
target_include_directories(glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/glad/include")

# glad loads opengl32.dll at runtime but its wgl entry points come from
# opengl32.lib; link it explicitly so the exe has no unresolved externals.
find_package(OpenGL REQUIRED)
target_link_libraries(glad PUBLIC OpenGL::GL)

# --------------------------------------------------------------------------
# glfw
# --------------------------------------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE    "STATIC" CACHE STRING "" FORCE)
add_subdirectory(glfw)
add_library(glfw::glfw ALIAS glfw)
# Consumers get GL through glad (vendor/glad), never through GLFW's own
# include of <GL/gl.h> — that would fight glad's gl.h.
target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

# --------------------------------------------------------------------------
# imgui (docking branch) + official GLFW / OpenGL3 backends
# --------------------------------------------------------------------------
set(EMBER_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

add_library(imgui STATIC
    "${EMBER_IMGUI_DIR}/imgui.cpp"
    "${EMBER_IMGUI_DIR}/imgui_demo.cpp"
    "${EMBER_IMGUI_DIR}/imgui_draw.cpp"
    "${EMBER_IMGUI_DIR}/imgui_tables.cpp"
    "${EMBER_IMGUI_DIR}/imgui_widgets.cpp"
    "${EMBER_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${EMBER_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)
add_library(imgui::imgui ALIAS imgui)
target_include_directories(imgui PUBLIC "${EMBER_IMGUI_DIR}" "${EMBER_IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw)
target_compile_features(imgui PUBLIC cxx_std_17)

# Vendor targets stay warning-quiet; our own targets get /W4 via
# ember_enable_warnings.
foreach(vt glad glfw imgui)
    if(MSVC)
        target_compile_options(${vt} PRIVATE /W0 /wd4996)
    endif()
endforeach()

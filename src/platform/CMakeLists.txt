# src/platform — window, input, GL loading. Owned by Kiln · Platform.
# Contract: ARCHITECTURE.md §4.2. Each module declares its own sources.
# Warnings come from the root CMakeLists (root owns flags).

add_library(ember_platform STATIC
    Window.cpp
    Input.cpp
    GLLoader.cpp
)

target_include_directories(ember_platform PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(ember_platform PUBLIC glad glfw ember_core)

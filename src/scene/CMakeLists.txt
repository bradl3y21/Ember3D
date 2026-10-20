# src/scene — owned by Slate · Scene.
# Scene graph, primitives, RenderPacket production, .ember I/O.
set(EMBER_SCENE_SOURCES
  Scene.cpp
  Primitives.cpp
  EditableMesh.cpp
  Modifiers.cpp
  SceneIO.cpp
)

add_library(ember_scene STATIC ${EMBER_SCENE_SOURCES})
target_include_directories(ember_scene PUBLIC ${CMAKE_SOURCE_DIR}/src)
# Contract: scene links downward only (core).
target_link_libraries(ember_scene PUBLIC ember_core)
source_group("scene" FILES ${EMBER_SCENE_SOURCES})

add_library(ember_editor STATIC
    Editor.cpp
    Icons.cpp
    Outliner.cpp
    Properties.cpp
    Menu.cpp
    Viewport.cpp
    Gizmo.cpp
    Sidebar.cpp
    Toolbar.cpp
    Preferences.cpp
    StatusBar.cpp
    History.cpp
    Workspace.cpp
    Timeline.cpp
    Modal.cpp
    Pie.cpp
    SelfTest.cpp
    ModifierPanel.cpp
    EditModeTools.cpp
    Commands.cpp
    Undo.cpp
)
target_include_directories(ember_editor PUBLIC ${CMAKE_SOURCE_DIR}/src)
# glfw / glad / imgui are vendor targets (D5), defined once in vendor/. The
# editor names them but never includes a platform header (D13).
target_link_libraries(ember_editor PUBLIC ember_core ember_render ember_scene glfw glad imgui)

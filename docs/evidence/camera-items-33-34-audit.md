# Camera backlog audit: items 33 and 34

**Audited:** 2026-10-18, against the `Ember3D - Copy` source tree before camera
storage, v3 persistence, look-through, or Properties UI changes. This is a tree
audit, not a claim about earlier history.

Items 33 and 34 were marked satisfied, but the inspected tree did not implement
their camera behavior:

| Item | Claimed behavior | Evidence in the pre-fix tree | Finding |
| --- | --- | --- | --- |
| 33 | A camera object appears in the Outliner and packet like other objects. | `core/Types.h` declares `CameraSettings`, but `scene/Scene.h::Object` has no camera component. `World::buildPacket` skips objects without `hasMesh`, so there is no camera datum in the packet. `editor/Outliner.cpp` lists objects generically and has no camera distinction. | The shared type declaration was mistaken for scene storage and editor/render wiring. Not implemented. |
| 34 | Look through the active camera. | `core/App.cpp` fills `RenderPacket::view`, `projection`, and `cameraPosition` from `render::OrbitCamera`; the numpad navigation dispatch has no `KP_0` branch. | The persisted `activeCamera_` ID was mistaken for a usable camera projection. Not implemented. |

The camera-specific round-trip test added for #209 also reproduced the gap:
`test_scene_camera` rejected a v2 file containing the typed `camera` record.
`SceneIO.cpp` accepted only v1/v2, and `FORMAT.md` described v2 as canonical with
no camera record.

This finding is deliberately preserved after the implementation lands. It is a
reminder to verify other "satisfied" items against code and reachable call sites,
not against the backlog label or a type declaration alone.

## Follow-up after #209 implementation

`CameraSettings` is now stored on `scene::Object`, camera objects can be added
from the viewport Add menu/F3 palette and appear as ordinary Outliner rows, and
v3 `.ember` records persist the five projection fields. `App` now consumes the
active camera's world pose and perspective/orthographic settings for the render
packet while KP_0 look-through is active; the real frame-loop self-test checks
both projection modes. The Properties Camera category edits all five fields
and can designate the active scene camera. Item 34 is therefore wired in the
real App path.

**Item 33 residue:** `RenderPacket::items` remains the mesh-draw list; an
unmeshed camera object is not a `DrawItem` and no camera wireframe glyph is
drawn in the viewport. This change does not claim that an editor-only camera
icon is renderable geometry. The camera drives packet view/projection/position
when active, and the Outliner lists the object; if item 33 meant a visible
viewport camera glyph or a camera object entry in `DrawItem`, that behavior
still needs a separately scoped implementation.

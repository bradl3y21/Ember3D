# The `.ember` scene file format

**Version 8** · owner: `Slate · Scene` (`src/scene/SceneIO.cpp`)
**API:** `bool save(const ember::scene::World&, const std::string& path);`
**`bool load(ember::scene::World&, const std::string& path);`**

> QA writes tests against this document, not against the implementation. If
> the two disagree, that is a defect on one side — file it against the *scene*
> owner.

## 1. Overview

- Plain ASCII text, one record per line, whitespace-separated fields,
  newline-terminated.
- A `.ember` file stores **the whole `World` state**: objects (name, id,
  parent, transform, material, mesh kind, visibility), the parent links, the
  per-object light components (D19: lights are scene objects), camera
  projection components (D47), the selection, the 3D cursor position, scene
  render-engine/settings state, image references, editable mesh topology and
  the next free object id.
- An unedited mesh stores only its primitive kind and regenerates through
  `makePrimitiveMesh`. An object with an `editable` record stores authored
  topology; on load that data, not regenerated primitive indices, is
  authoritative for both `EditableMesh` and render `MeshData`.
- **Lights are objects** (D19): a light is an ordinary record with
  `mesh none`, an authored `light` component record (colour, intensity,
  radius, kind), and its aim/position carried by the object's ordinary
  Transform — exactly as in Blender: a directional light shines along its
  object's local −Z, a point light sits at its object's position. There is no
  world-level light any more; v1 files are migrated on load (§5).
- `save` never mutates the world. `load` parses into a temporary world and
  swaps it into the target only on full success: a rejected file leaves the
  target world **completely unchanged** (no partial population, merge, or
  clearing — it is replace-wholesale or nothing).
- All floats are written with **9 significant digits** (`setprecision(9)` on
  a `float` ostream), which round-trips every IEEE-754 `float` exactly.

## 2. Grammar

```
file(v8)    := v7 record order plus editable topology records before images
                  ; v8 canonical
file(v7)    := header NL nextidrec NL selectionrec NL
                 [ activerec NL ] [ nextcollectionrec NL ]
                 [ worldbgrec NL ] [ framerec NL ] [ cursorrec NL ]
                 renderrec NL
                 { objectrec NL } { lightrec NL } { camerarec NL }
                 { collectionrec NL } { incollectionrec NL }
                 { materialslotrec NL } { image records NL } end ; legacy reader
file(v6)    := v5 record order plus required renderrec
                 (no image records)                           ; legacy reader
file(v5)    := same record order as v4, with v5 material fields
                (transmission + IOR, no render settings)   ; legacy reader
file(v4)    := same record order as v5, with v4 material fields
                (emission, but no transmission/IOR)       ; legacy reader
file(v3)    := same record order and material field widths as v2,
                plus optional camera records               ; legacy reader
file(v2)    := header NL nextidrec NL selectionrec NL
               [ activerec NL ] [ nextcollectionrec NL ]
               [ worldbgrec NL ] [ framerec NL ] [ cursorrec NL ]
               { objectrec NL } { lightrec NL }
               { collectionrec NL } { incollectionrec NL }
               { materialslotrec NL } end                 ; version 2
file(v1)    := header NL nextidrec NL worldlightrec NL selectionrec NL
               { objectrec NL } end                      ; version 1 (load only)
  header      := "EMBER3D.SCENE" "v" VERSION          ; VERSION := "v1"|"v2"|"v3"|"v4"|"v5"|"v6"|"v7"|"v8"
nextidrec   := "nextid" UINT                        ; UINT := decimal integer
worldlightrec := "light" FLOAT×8                     ; v1 ONLY (migrated, §5)
selectionrec:= "selection" UINT { UINT }             ; trailing: selected ids
  activerec   := "active" UINT                         ; v2-v8: active-camera id
  nextcollectionrec := "nextcollection" UINT           ; v2-v8, OPTIONAL (tolerant)
  worldbgrec  := "worldbg" FLOAT×5 UINT                ; v2-v8, OPTIONAL (tolerant)
  framerec    := "frame" INT INT INT                   ; v2-v8, OPTIONAL (tolerant)
  cursorrec   := "cursor" FLOAT FLOAT FLOAT             ; v2-v8, OPTIONAL (tolerant)
  renderrec   := "render" ENGINE UINT UINT UINT          ; v6-v8, REQUIRED
objectrec(v1-v3) := "object" UINT STRING UINT MESH VIS
                    FLOAT×3 FLOAT×3 FLOAT×3 FLOAT×4 FLOAT FLOAT
objectrec(v4)    := objectrec(v1-v3) FLOAT×3 FLOAT
                     ; emission RGB + strength
objectrec(v5-v8) := objectrec(v4) FLOAT FLOAT
                     ; transmission + IOR
lightrec    := "light" UINT KIND FLOAT×4 FLOAT FLOAT  ; v2-v8, after objects
camerarec   := "camera" UINT FLOAT FLOAT FLOAT UINT FLOAT ; v3-v8, after lights
collectionrec    := "collection" UINT UINT STRING VIS ; D22 (v2-v8, optional)
incollectionrec  := "incollection" UINT UINT         ; D22 (v2-v8, optional)
materialslotrec(v2-v3) := "materialslot" UINT UINT STRING FLOAT×4 FLOAT FLOAT
materialslotrec(v4)    := materialslotrec(v2-v3) FLOAT×3 FLOAT
                                                      ; emission RGB + strength
materialslotrec(v5-v8) := materialslotrec(v4) FLOAT FLOAT
                                                       ; transmission + IOR
basetexturerec := "basetexture" UINT STRING COLORSPACE ; v7, slot 0
slottexturerec := "slottexture" UINT UINT STRING COLORSPACE ; v7, slot >= 1
environmenttexturerec := "environmenttexture" STRING COLORSPACE ; v7
editablemeshrec := "editable" UINT UINT UINT UINT UINT UINT UINT ; v8
editpositionrec := "editposition" UINT FLOAT FLOAT FLOAT ; v8
editnormalrec := "editnormal" UINT FLOAT FLOAT FLOAT ; v8
editedgerec := "editedge" UINT UINT UINT ; v8
editfacerec := "editface" UINT UINT UINT UINT ; v8
editlooprec := "editloop" UINT UINT UINT UINT UINT UINT FLOAT FLOAT ; v8
endeditrec := "endedit" ; v8
COLORSPACE := "srgb" | "linear"
end         := "end"
MESH        := "none" | "cube" | "sphere" | "plane" | "cylinder" | "torus"
VIS         := "visible" | "hidden"
KIND        := "directional" | "point"
ENGINE      := "raster" | "cycles"
```

Record order is **fixed and mandatory** for records that are present. In v2-v8:
header, `nextid`, `selection`, optional `active`, optional `nextcollection`,
optional `worldbg`, optional `frame`, optional `cursor`, then (v6-v8 only) exactly
one `render` record, any number of `object` records, any number of `light`
records, then (v3-v8 only) `camera` records, any
number of `collection`, `incollection` and `materialslot` records, then v8
editable mesh groups, then v7-v8 image reference records, `end`. In v1:
header, `nextid`,
the world-level `light` record, `selection`, `object` records, `end` — v1 is
a **reader-supported** version: its world `light` record is migrated into a
light object on load (§5). A v2-v8 file must not contain the world-level `light`
record; a v1 file must not contain per-object `light` records, an `active`,
`nextcollection`, `worldbg`, `cursor` or `camera` record, or any collection/slot records
(the grammar leaves no place for them, so those keywords in a v1 file are parse
errors).

**Tolerant-reader rule (established mid-v2, kept):** records ADDED after the
first v2 writer shipped — `active`, `nextcollection`, `worldbg`, `frame`,
`cursor` —
may be
ABSENT from v2-v8 files; the reader applies the documented defaults (0 = no
active camera, 1 = fresh collection allocator, background = the core
`WorldBackground` default, cursor = world origin) instead of failing. The v8
writer always emits them and requires the `render` record. v1-v5 remain readable;
their render settings default to Raster, 64 samples, 4 bounces and seed 1.
v2's legacy nonzero `active` id is
migrated to a camera component with default projection values. Versions other
than v1/v2/v3/v4/v5/v6/v7/v8 are refused outright — `load` returns
false with the target world untouched, never a silent mis-read with guessed
semantics (backlog 31).

Integer literals are decimal. FLOAT literals are decimal ASCII (the
`std::ostream` 9-digit form; exponents may appear; bare integers like `1` are
legal FLOATs and must be accepted).

## 3. Records

### header — `EMBER3D.SCENE v1` / `v2` / `v3` / `v4` / `v5` / `v6` / `v7` / `v8`
- Exact uppercase text `EMBER3D.SCENE`,
- `v1` files load (world light record migrates per §5);
- `v2` is a legacy readable version; its `active` id is upgraded to default camera settings;
- `v8` is the canonical current version (what `save` writes); v1-v7 remain
  readable and their dielectric values migrate to transmission 0 / IOR 1.45;
  v1-v3 additionally migrate emission to black / zero strength;
- any other version (`v0`, `v9`, …) makes `load` **fail** (return false,
  target world untouched).

### `nextid UINT`
The world's next unused object id; `load` restores numbering from it, so a
save/load cycle never produces colliding ids. Validation: must be ≥ 1 and
**strictly greater than every object id in the file** — INCLUDING light
objects created by migration (§5/§6) — else `load` fails.

### `selection N id...`
N followed by exactly N object ids. Always present (`selection 0` when
nothing is selected). The order IS the restored selection order. Every listed
id must name an object in the file, else `load` fails.

### `active objectId`   (v2-v8, optional/tolerant)
The selected scene camera id; 0 means none. A v3-v8 active id must also have a
matching `camera` component record. For legacy v2 files, a nonzero active id
names an object that is upgraded to a camera with the default `CameraSettings`
when loaded. If absent, the active camera defaults to none.

### `worldbg r g b a strength lighting`   (v2-v8, optional/tolerant)
The scene's world background (D22(d), item 57): colour RGBA (4 floats),
`strength` (1 float), `lighting` (`0` or `1` — the flat-ambient switch).
Defaults when absent: the core `WorldBackground` default (colour
0.05/0.05/0.06, strength 1, lighting off).

### `frame start end current`   (v2-v8, optional/tolerant)
The scene's frame state (item 86) — what the Timeline area displays and
edits. Three integers. Defaults when absent: `1 250 1`. On load the CURRENT
frame is clamped into `[start, end]` exactly as `setFrameCurrent` does
(start > end cannot be produced by the writer; a hand-edited file that
carries it is normalised by the same invariants rather than failing the
load). Playback is NOT part of this state — a `.ember` file records no
  animation, and the UI must say so plainly.

### `cursor x y z`   (v2-v8, optional/tolerant; item 157)
The scene's 3D cursor position in world coordinates (D22). The three decimal
floats are persisted with the file's 9-significant-digit float rule. When the
record is absent from a v2-v8 file, the cursor defaults to the world origin
`(0, 0, 0)`. The canonical v8 writer always emits exactly one cursor record,
after the optional `frame` record and before the `render` record. The cursor is
scene data only; this record does not persist viewport pivot mode or editor UI
state.

### `render ENGINE samplesPerPixel maxBounces seed` (v6-v8, required)

The scene-level render engine and path-tracing quality settings. `ENGINE` is
`raster` or `cycles`; `samplesPerPixel`, `maxBounces`, and `seed` are decimal
unsigned integers. The record is written once, after `cursor` and before all
objects. The engine choice belongs to the scene, not viewport UI state; it only
selects the Cycles-style CPU prototype while viewport shading is Rendered. Other
shading modes remain raster.

- `samplesPerPixel` is clamped to `[1, 16777216]` on setter, save and load.
- `maxBounces` is clamped to `[0, 64]` on setter, save and load.
- `seed` preserves its full u32 range. Unknown engine words, duplicate records,
  missing v6-v8 records and malformed integer fields fail load atomically.
- v1-v5 files migrate to `raster 64 4 1`; v6 retains its serialized render settings.

### `object ... material fields`
| field | meaning |
| ----- | ------- |
| `id` | decimal u32, unique in the file; `0` is **illegal** |
| `"name"` | the object name in double quotes (escaping per §4) |
| `parent` | parent's object id; `0` = root object |
| `mesh` | `none`, or a kind word (`cube` `sphere` `plane` `cylinder` `torus`) |
| `visible` | `visible` or `hidden`; hidden objects emit no `DrawItem` |
| `px py pz` | transform position |
| `rx ry rz` | transform rotation (euler **radians**, XYZ order, exactly `Transform.rotation`) |
| `sx sy sz` | transform scale |
| `r g b a` | material `baseColor` (4 floats, 0..1 typical, not clamped) |
| `metallic` | material metallic (1 float) |
| `roughness` | material roughness (1 float) |
| `emission r g b` | v4-v8: linear-RGB emission colour; components are non-negative and may exceed 1 for HDR |
| `emissionStrength` | v4-v8: non-negative emission multiplier, default 0 |
| `transmission` | v5-v8: dielectric transmission weight, clamped to `[0,1]` when saved/materialized |
| `ior` | v5-v8: finite index of refraction in `[1,3]`; otherwise defaults to `1.45` |

The v4 object record appends emission RGB and strength after `roughness`; v5
and v5-v8 append transmission and IOR after those emission fields. Historical v1-v3
records end after `roughness` and migrate to black emission with zero strength;
v1-v4 use transmission 0 and IOR 1.45. Emission RGB has no meaningful alpha;
`MaterialDesc` uses alpha 1. Negative or non-finite in-memory emission
components are sanitized to zero when saved or materialized. Non-finite base
colour values are replaced with their defaults; finite base colour (including
HDR) is preserved. Non-finite metallic/roughness values use defaults and
finite values are clamped to `[0,1]`. Transmission uses non-finite fallback 0
and otherwise clamps to `[0,1]`; invalid or out-of-range IOR uses 1.45. File
FLOAT tokens must be finite; non-finite tokens make loading fail. Finite HDR
emission values above 1 are preserved.

- A light object is persisted as an ordinary object record with `mesh none`
  plus ONE following `light` record (below). `mesh none` WITHOUT a light
  record is an unmeshed group/empty object (`hasMesh = false`).
- `parent` may reference any object in the file (order does not matter —
  resolved after all records); must not be the object's own id, must not
  create a cycle; `load` walks the chains and fails on cycles.
- `load` rebuilds `children` lists from the parent fields; they are never
  stored.

### `materialslot objectId slotIndex "name" material fields` (v2-v8)

Slot indices start at 1 because slot 0 is stored in the object's `object`
record. The legacy material fields are base RGBA, metallic and roughness; v4
appends linear emission RGB and emission strength and v5-v8 append transmission
and IOR, in the same order as the object material. v2/v3 slots omit emission
and migrate to black / zero; v2-v4 slots use dielectric defaults of 0 and
1.45. The same sanitization and finite-token validation rules apply to slots.
Duplicate indices or an absent owning object fail load.

### Image reference records (v7-v8)

Image references are separate from the historical material field layouts:

- `basetexture objectId "path" COLORSPACE` assigns the object's slot-0 base-color image.
- `slottexture objectId slotIndex "path" COLORSPACE` assigns an image to an additional material slot (`slotIndex >= 1`); that slot must have a corresponding `materialslot` record.
- `environmenttexture "path" COLORSPACE` assigns the world's environment image.
- `COLORSPACE` is exactly `srgb` or `linear`; any other value is malformed.
- An empty path means unassigned. Canonical writers omit unassigned records.
  Legacy v1-v6 files have no image records and migrate to empty paths with the
  `ImageColorSpace::SRgb` default.
- Paths may contain spaces and quotes/backslashes using §4 escaping. A NUL byte
  is rejected. Relative paths resolve against the parent directory of the
  loaded `.ember` file; absolute paths remain absolute. Paths are not required
  to exist, and relative `..` components are permitted (the asset root is not a
  security sandbox).
- On save-as, a relative reference is resolved against the world's current
  `assetBaseDirectory`, then written relative to the destination scene's
  directory. This preserves the referenced file's identity without mutating
  the in-memory path or base directory. Absolute references remain absolute.

The path's image encoding is interpreted by the renderer's image loader, not by
scene I/O. `srgb` requests sRGB color decoding; `linear` requests scene-linear
data. Environment image handling follows the same tag. Scene I/O validates the
tag and path syntax only; it does not open or decode the image.

### Editable mesh records (v8)

One optional group per edited mesh object, after all `materialslot` records and
before any image reference records:

```
editable objectId vertexCount normalCount edgeCount faceCount loopCount topologyVersion
editposition index x y z                         ; one per vertex, dense index order
editnormal index x y z                           ; normalCount records, dense order
editedge index v0 v1                             ; one per edge, dense order
editface index firstLoop loopCount materialIndex ; one per face, dense order
editloop index vertex edge next prev face u v    ; one per loop, dense order
endedit
```

`normalCount` is either 0 (no authored vertex normals) or `vertexCount`. Each
count is at most 1,000,000. Faces own dense, contiguous loop ranges; each face
has at least three distinct vertices. Loop `next`/`prev` must match its face's
ring order, and each loop's edge must name the canonical undirected edge from
its vertex to the next loop's vertex. Edge endpoints are in-range, unique and
stored with `v0 < v1`; every edge is used by one or two faces (no orphan or
three-face edges), every vertex is used, and every loop belongs to exactly one
face. Position, normal and UV values must be finite. Face material indices
must refer to slot 0 or an explicitly serialized slot on the same object.
`topologyVersion` is preserved; CSR adjacency is derived from these records and
rebuilt on load, then checked against the editable-mesh invariants.

An editable group must name one existing meshed, non-light object and can
appear only once for that object. It includes authored geometry even when the
object's primitive word still names its original source primitive. On load, the
editable data is the source of truth: the reader does **not** regenerate a
primitive and apply serialized indices to it. Instead, it restores the
`EditableMesh` and generates `MeshData` from that exact topology. v1-v7 have no
editable records and retain their historical migration: primitive meshes are
regenerated and face assignments default to slot 0.

Malformed/incomplete groups, oversized counts, invalid indices, non-finite
attributes, invalid rings, inconsistent edge use, or missing material slots
fail the entire load without changing the target world. The writer refuses
invalid in-memory editable topology before opening the destination.

### `light objectId KIND r g b a intensity radius`   (v2-v8, after objects)
The authored light component (D19) of ONE light object:

| field | meaning |
| ----- | ------- |
| `objectId` | must name an object record **in this file** (the object exists) |
| `KIND` | `directional` or `point` |
| `r g b a` | light color, RGBA (4 floats) |
| `intensity` | light strength (1 float) |
| `radius` | point-light falloff radius, world units (1 float; stored for all kinds) |

- Exactly **one** `light` record per light object; duplicates fail. A `light`
  record for an id with no object record **fails**. The object gets
  `isLight = true` and the component; its `mesh` field is `none` (validated:
  a mesh record's kind word is not allowed to be a light — lights are
  unmeshed by construction since `createLight` never generates geometry).
- Aim/position are NOT in the record: they are the object's Transform.
  A directional light resolves to world direction
  `normalize(R * (0,0,-1))` (its object's rotation); a point light resolves to
  world position = its object's transform position acted on by the full world
  matrix. Nothing else is stored, exactly like Blender.
- **Lights are not pickable in the viewport** (they are unmeshed and
  `World::pick`/`pickBox` skip unmeshed objects per the contract); they are
  selected through the outliner.

### `camera objectId fovY clipNear clipFar orthographic orthoScale`
(v3-v8, after all `light` records)

The camera projection component of one object. Its transform is the camera
pose (local −Z forward, local +Y up); the record carries projection only.

| field | meaning |
| ----- | ------- |
| `objectId` | must name an object record in this file |
| `fovY` | vertical field of view in radians; `0 < fovY < π` |
| `clipNear` | near clip distance; strictly positive |
| `clipFar` | far clip distance; greater than `clipNear` |
| `orthographic` | `0` = perspective, `1` = orthographic |
| `orthoScale` | orthographic vertical span in world units; strictly positive |

Exactly one record marks an object as a camera (`CameraSettings::isCamera`);
duplicate records, dangling ids, invalid projection values, and active ids
without camera components are rejected atomically. A camera record is never
accepted under a v1/v2 header. The canonical writer emits one record for each
camera object, in object creation order.

### `end`
Terminates the file. Anything after it makes `load` fail. A missing `end`
(truncated file) fails `load`.

## 4. Strings and escaping

Names and image paths are written inside double quotes. Inside the quotes:

- `\` (backslash) is written `\\`
- `"` (double quote) is written `\"`

The writer escapes exactly those two bytes; the reader unescapes exactly those
two; an unrecognized escape (backslash followed by anything else) is a parse
error → `load` returns false. Any other byte is stored verbatim between the
quotes. The escaping is canonical: re-saving a loaded world yields
byte-identical output for equal worlds (with the fixed float encoding of §1).

## 5. Loading v1 (migration rules)

A `v1` header is accepted and loads to v8 semantics:

- The v1 world-level record `light dx dy dz r g b a intensity` (direction
  possibly unnormalized; colour; strength) is REPLACED by one migrated light
  object, appended after all file objects as a root:
  - name `"Sun"`, uniquified `"Sun.001"`… exactly like mesh names;
  - kind `directional`, colour and intensity **exactly** from the v1 record;
  - `radius = 1.0`; visible; no mesh; parent = root; id = `nextid`'s value
    (then `nextid` advances past it);
  - transform position (0,0,0), rotation (**p', y', 0**) with
    `p' = asin(clamp(dŷ,-1,1))` and `y' = atan2(-dx̂, -dẑ)` on the
    **normalized** v1 direction d̂ — the exact euler (degrees in radians,
    XYZ) whose local −Z resolves to d̂, so the migrated light produces the
    same world direction the v1 world light carried.
- v1 files that also have no world `light` record fall under the
  default-sun rule (§6) like any other load.
- After migration, v1-v6 loads satisfy the same invariants: at least one
  light exists (§6), all resolves are consistent, transmission is 0, IOR is
  1.45, and render settings default to Raster, 64 samples, 4 bounces and seed 1;
  `save` always writes v8. v1-v3 also use black emission with zero
  strength; v4-v7 retain their serialized emission values. Every v1-v6 image
  reference is empty/unassigned with the default sRGB tag. v1-v7 have no
  editable topology, so primitive faces load with slot 0.

## 6. The default sun (documented default for lightless scenes)

**After materialising all objects, applying the v1 migration (§5) and any
`light` records, if the world still contains no light object, `load` appends
exactly one default sun:**

- name `"Sun"` (uniquified like every name);
- kind `directional`, colour (1,1,1,1), intensity 1.0, radius 1.0;
- visible, unmeshed, root, id = `nextid`'s value (then `nextid` advances);
- transform position (0,0,0), rotation `(-1.5707963, 0, 0)` — i.e. local −Z
  resolves straight down (0,−1,0), the same direction the v1 world default
  carried.

So every load produces at least one light; download-light-ness is not a state
a loaded world can be in (and `RenderPacket::lights` is never empty after a
load unless the single light's constructor was hidden). Note **the byte-round
trip makes a two-step plateau for lightless inputs**: `load(T)` of a lightless
file adds the default sun, therefore `save(load(T))` differs from `T` exactly
by the sun; but `save(load(save(load(T)))) == save(load(T))` byte-for-byte and
`load(save(load(T))) == load(T)` state-wise — the canonical form is reached
from the second hop and is stable from then on.

## 7. load failure rules (what QA can assert)

`load` returns `false` and leaves the target world **completely unchanged**
when:

1. the file cannot be opened or read;
2. the header is missing, malformed, or names a version outside v1-v8;
3. any record has the wrong field count, wrong field kind (UINT vs FLOAT vs
   word), or unknown keyword;
4. records appear out of the fixed order of §2 (v2-v8 must NOT have the world
   `light` record; v6-v8 require one `render` record before objects; `light`
   records follow objects and v3-v8 camera records follow lights);
5. an object id is 0, or duplicated;
6. `nextid` is missing, 0, or ≤ some object id in the file;
7. a `parent` id names an absent object, equals the object's own id, or
   creates a cycle;
8. a selection id names an absent object;
9. a `light` record names an absent object, repeats an object, or uses an
   unknown KIND word;
10. a v3-v8 `camera` record names an absent object, repeats an object, has an
    invalid projection value, or an active id lacks a camera record;
11. any file float is non-finite (`inf`/`nan`), including cursor, camera,
    emission, transmission and IOR fields;
12. more than one `cursor`, `render` or `environmenttexture` record exists, or
    a required render record is missing or has an unknown engine;
13. a texture record is pre-v7, has an unknown color-space word, contains a NUL
   path, duplicates a material assignment, or names an absent object/slot;
14. an editable mesh record is pre-v8, incomplete, duplicated, oversized,
   names a missing/unmeshed/light object, contains invalid topology/indices or
   refers to a missing material slot;
15. trailing content exists after `end`, or `end` is missing.

On success, the target world is replaced wholesale: objects in file order
(with regenerated primitive meshes or restored editable meshes), parents/children rebuilt, light and camera
components attached, legacy v2 active-camera migration and/or the v1 migration
and default sun applied (§5/§6), then selection, cursor (or its absent-record
origin default) and `nextid` restored as specified.

## 8. Byte-exactness and the packet shape (for QA and Render)

For any world w **with at least one light**: `save(w)` → T; `load(T)` → w′
satisfies

- object count, order, ids, unescaped names, parent links, children lists,
  selection order, `nextid`: equal;
- every transform/material float (including emission, transmission and IOR),
  render setting, cursor coordinate and every light component float (colour,
  intensity, radius), plus camera `fovY`/`clipNear`/`clipFar`/`orthoScale`:
  **bitwise equal** (9-digit encoding, §1); camera booleans equal;
- `hasMesh`/`mesh` kind/`visible`/`isLight`: equal per object; unedited
  primitive meshes equal to the current generator's output, edited mesh data
  and `EditableMesh` topology equal to the authored records (including
  positions, normals, UV loops, face rings and material indices);
- resolved light instances in `RenderPacket::lights` appear in object
  creation order, `directional` first in creation order among directionals,
  point lights after (order within kind by creation order — no sorting is
  applied beyond retaining creation order); the deprecated
  `RenderPacket::light` is filled from the FIRST directional in creation
  order (exactly its resolved colour/intensity/direction), or untouched core
  default when a world has no directional light.

And the canonical-writer plateau of §6: `save(load(T))` refers to a world
that passes every contract above, and repeated save/load cycles beyond the
first produce byte-identical files.

## 9. Example

```
EMBER3D.SCENE v7
nextid 5
selection 1 1
cursor 1.25 -2.5 0.75
render raster 64 4 1
object 1 "Cube" 0 cube visible 0 0.5 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45
object 2 "Group" 1 none visible 1.5 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45
object 3 "Sun" 0 none visible 0 0 0 0 -1.5707963 0 1 1 1 0 0 0 1 0 0.5 0 0 0 0 0 1.45
light 3 directional 1 1 1 1 1 1
end
```

Reading it in prose: the cursor is at world position (1.25, -2.5, 0.75).
Object 1 is a meshed, selected cube root parents object 2
("Group"). Object 3 is a light object "Sun" aimed straight down
(rotation x = −90°), with a directional white component at intensity 1 and
point radius 1 (unused, carried). Exactly one `light` record, applied to
object 3.

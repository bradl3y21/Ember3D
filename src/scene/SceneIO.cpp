// SceneIO.cpp — save/load for the .ember format.
// Owned by Slate · Scene.
//
// The format is specified in src/scene/FORMAT.md *beside this file*. Any
// change to the text format changes both in the same change, nothing else —
// QA writes the round-trip test from the document.
//
// Failure rule (FORMAT.md §1/§5): the target world is never touched until the
// whole file has parsed, validated and materialised into a temporary World;
// any parse/validation defect throws this file's ParseError and load() turns
// it into a clean `false`.
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <istream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ember::scene {

namespace {

constexpr int kFormatVersion = 8;  // v8 adds editable mesh topology
constexpr u32 kMaxEditableElements = 1'000'000;

struct ParseError : std::runtime_error {
  explicit ParseError(const std::string& what) : std::runtime_error(what) {}
};

bool validCameraProjection(const ember::CameraSettings& settings) {
  return std::isfinite(settings.fovY) && std::isfinite(settings.clipNear) &&
         std::isfinite(settings.clipFar) && std::isfinite(settings.orthoScale) &&
         settings.fovY > 0.0f && settings.fovY < ember::kPi &&
         settings.clipNear > 0.0f && settings.clipFar > settings.clipNear &&
         settings.orthoScale > 0.0f;
}

// ---------------------------------------------------------------------------
// Token scanner. Whitespace separates records/fields; a quoted region (which
// may contain ANY raw bytes, per FORMAT.md §4) is one token. \\ and \" are the
// only escapes; any other escape is a parse error.

class Scanner {
public:
  explicit Scanner(const std::string& text) : text_(text) {}

  // Next opaque token. Returns false at end of input.
  bool next(std::string& tok) {
    if (hasPushed_) { tok = pushed_; hasPushed_ = false; return true; }
    if (!skipWhitespace()) return false;    tok.clear();
    if (text_[pos_] == '"') return scanQuoted(tok);
    while (pos_ < text_.size() && !isWhitespace(text_[pos_]))
      tok += text_[pos_++];
    return true;
  }

  bool atEnd() { skipWhitespace(); return pos_ >= text_.size(); }
  std::size_t position() const { return pos_; }

  // Re-offer a consumed token to the next next() call (tolerant-record
  // lookaheads need it; one slot only).
  void pushBack(const std::string& tok) { pushed_ = tok; hasPushed_ = true; }

  [[noreturn]] void fail(const std::string& why) const {
    throw ParseError{"ember: parse error at byte " +
                     std::to_string(pos_) + ": " + why};
  }

private:
  static bool isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
  }

  bool skipWhitespace() {
    while (pos_ < text_.size() && isWhitespace(text_[pos_])) ++pos_;
    return pos_ < text_.size();
  }

  bool scanQuoted(std::string& tok) {
    ++pos_;  // opening quote
    for (;;) {
      if (pos_ >= text_.size()) fail("unterminated quoted name");
      const char c = text_[pos_++];
      if (c == '"') return true;                       // closing quote
      if (c != '\\') { tok += c; continue; }
      if (pos_ >= text_.size()) fail("unterminated escape");
      const char n = text_[pos_++];
      if (n == '\\')      { tok += '\\'; continue; }
      if (n == '"')       { tok += '"';  continue; }
      fail("unknown escape sequence");
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
  std::string pushed_;
  bool hasPushed_ = false;
};

// ---------------------------------------------------------------------------
// Field readers (FORMAT.md §2/§3). Every violation throws ParseError.

void needWord(Scanner& sc, const char* expected) {
  std::string tok;
  if (!sc.next(tok))
    sc.fail(std::string{"unexpected end of file, expected '"} + expected + "'");
  if (tok != expected)
    sc.fail(std::string{"expected '"} + expected + "', got '" + tok + "'");
}

void readUint(Scanner& sc, u32& out) {
  std::string tok;
  if (!sc.next(tok)) sc.fail("unexpected end of file (integer)");
  if (tok.empty()) sc.fail("empty integer token");
  unsigned long long acc = 0;
  for (const char c : tok) {
    if (c < '0' || c > '9') sc.fail("not an integer: '" + tok + "'");
    acc = acc * 10 + (c - '0');
    if (acc > 0xFFFFFFFFull) sc.fail("integer overflow: '" + tok + "'");
  }
  out = static_cast<u32>(acc);
}

// Signed decimal (the `frame` record, item 86): optional '-'/'+', then digits.
void readInt(Scanner& sc, int& out) {
  std::string tok;
  if (!sc.next(tok)) sc.fail("unexpected end of file (integer)");
  if (tok.empty()) sc.fail("empty integer token");
  std::size_t i = (tok[0] == '-' || tok[0] == '+') ? 1 : 0;
  if (i >= tok.size()) sc.fail("not an integer: '" + tok + "'");
  long long acc = 0;
  for (; i < tok.size(); ++i) {
    const char c = tok[i];
    if (c < '0' || c > '9') sc.fail("not an integer: '" + tok + "'");
    acc = acc * 10 + (c - '0');
    if (acc > 2147483647ll) sc.fail("integer overflow: '" + tok + "'");
  }
  out = static_cast<int>(tok[0] == '-' ? -acc : acc);
}

void readFloat(Scanner& sc, float& out) {
  std::string tok;
  if (!sc.next(tok)) sc.fail("unexpected end of file (float)");
  std::istringstream is(tok);
  is.imbue(std::locale::classic());
  float v = 0.0f;
  is >> v;
  if (is.fail() || !is.eof()) sc.fail("not a float: '" + tok + "'");
  if (!std::isfinite(v)) sc.fail("non-finite float: '" + tok + "'");
  out = v;
}

bool validTexturePath(const std::string& path) {
  return path.find('\0') == std::string::npos;
}

const char* imageColorSpaceWord(ember::ImageColorSpace colorSpace) {
  switch (colorSpace) {
    case ember::ImageColorSpace::SRgb: return "srgb";
    case ember::ImageColorSpace::Linear: return "linear";
  }
  return nullptr;
}

ember::ImageColorSpace readImageColorSpace(Scanner& sc) {
  std::string word;
  if (!sc.next(word)) sc.fail("missing image color space");
  if (word == "srgb") return ember::ImageColorSpace::SRgb;
  if (word == "linear") return ember::ImageColorSpace::Linear;
  sc.fail("unknown image color space: '" + word + "'");
}

// Produce a path relative to the destination scene while preserving the file
// addressed by a source-relative reference. Absolute authored paths remain
// absolute. No filesystem canonicalization (and therefore no existence check)
// is performed: assets may be unavailable while a scene is being edited.
bool pathForSavedScene(const ember::ImageTextureRef& texture,
                       const std::filesystem::path& sourceBase,
                       const std::filesystem::path& destinationBase,
                       std::string& savedPath) {
  if (!validTexturePath(texture.path) ||
      imageColorSpaceWord(texture.colorSpace) == nullptr)
    return false;
  if (texture.path.empty()) {
    savedPath.clear();
    return true;
  }

  try {
    const std::filesystem::path authored(texture.path);
    if (authored.is_absolute()) {
      savedPath = authored.lexically_normal().generic_string();
      return true;
    }
    const std::filesystem::path resolved = (sourceBase / authored).lexically_normal();
    const std::filesystem::path relative = resolved.lexically_relative(destinationBase);
    savedPath = relative.empty() ? resolved.generic_string() : relative.generic_string();
    return true;
  } catch (const std::filesystem::filesystem_error&) {
    return false;
  }
}

// The mesh-kind words (FORMAT.md §2/§3) and their display names. Object::mesh
// name uses the display word in BOTH paths (createPrimitive and .ember load)
// so a round trip is bit-equal down to MeshData::name.
const char* meshKindWord(Primitive kind) {
  switch (kind) {
  case Primitive::Cube:     return "cube";
  case Primitive::Sphere:   return "sphere";
  case Primitive::Plane:    return "plane";
  case Primitive::Cylinder: return "cylinder";
  case Primitive::Torus:    return "torus";
  }
  return "none";  // unreachable via the enum's domain
}

const char* meshKindDisplayName(Primitive kind) {
  switch (kind) {
  case Primitive::Cube:     return "Cube";
  case Primitive::Sphere:   return "Sphere";
  case Primitive::Plane:    return "Plane";
  case Primitive::Cylinder: return "Cylinder";
  case Primitive::Torus:    return "Torus";
  }
  return "None";  // unreachable via the enum's domain
}

bool meshKindFromWord(const std::string& word, bool& hasMesh, Primitive& out) {
  hasMesh = true;
  if (word == "cube")     { out = Primitive::Cube;     return true; }
  if (word == "sphere")   { out = Primitive::Sphere;   return true; }
  if (word == "plane")    { out = Primitive::Plane;    return true; }
  if (word == "cylinder") { out = Primitive::Cylinder; return true; }
  if (word == "torus")    { out = Primitive::Torus;    return true; }
  hasMesh = false;
  if (word == "none") return true;
  return false;
}

void escapeInto(const std::string& name, std::string& out) {
  for (const char c : name) {
    if (c == '\\') { out += "\\\\"; continue; }
    if (c == '"')  { out += "\\\""; continue; }
    out += c;
  }
}

std::string escapeQuoted(const std::string& name) {
  std::string out = "\"";
  escapeInto(name, out);
  out += '"';
  return out;
}

// Uniquify a name against an object list (Blender ".001" suffix style) —
// used by the v1 migration and the default-sun rule, where load() appends a
// light object with an ordinary name.
void uniquifyForLight(const std::vector<Object>& objects, std::string& name) {
  const auto exists = [&](const std::string& n) {
    return std::any_of(objects.begin(), objects.end(),
                       [&](const Object& o) { return o.name == n; });
  };
  if (!exists(name)) return;
  for (u32 i = 1;; ++i) {
    std::ostringstream os;
    os << name << '.' << std::setfill('0') << std::setw(3) << i;
    if (!exists(os.str())) { name = os.str(); return; }
  }
}

// ---------------------------------------------------------------------------
// In-memory shape of a parsed .ember file.

struct ObjectRecord {
  u32 id = 0;
  u32 parent = 0;
  std::string name;
  bool hasMesh = false;
  bool visible = true;
  Primitive primitive = Primitive::Cube;
  float pos[3] = {0, 0, 0};
  float rot[3] = {0, 0, 0};
  float scl[3] = {1, 1, 1};
  float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float metallic = 0.0f;
  float roughness = 0.5f;
  float emissionColor[3] = {0.0f, 0.0f, 0.0f};
  float emissionStrength = 0.0f;
  float transmission = 0.0f;
  float ior = ember::kDefaultMaterialIor;
};

struct LightRecord {
  u32 object = 0;
  // v2 per-object record: kind + authored component (D19)
  bool isDirectional = true;         // KIND word → bool
  float color[4] = {1, 1, 1, 1};
  float intensity = 1.0f;
  float radius = 1.0f;
};

struct CameraRecord {
  u32 object = 0;
  ember::CameraSettings settings;
};

struct SlotRecord {
  u32 object = 0;
  u32 index = 0;                    // >= 1; slot 0 is the object record
  std::string name;
  float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float metallic = 0.0f;
  float roughness = 0.5f;
  float emissionColor[3] = {0.0f, 0.0f, 0.0f};
  float emissionStrength = 0.0f;
  float transmission = 0.0f;
  float ior = ember::kDefaultMaterialIor;
};

struct TextureRecord {
  u32 object = 0;
  u32 slot = 0;                      // base material is slot 0
  ember::ImageTextureRef texture;
};

struct EditableRecord {
  u32 object = 0;
  EditableMesh mesh;
};

struct MembershipRecord {
  u32 object = 0;
  u32 collection = 0;
};

struct CollectionRecord {
  u32 id = 0;
  u32 parent = 0;
  std::string name;
  bool visible = true;
};

struct SceneFile {
  u32 nextId = 1;
  int version = kFormatVersion;
  bool hadWorldLight = false;        // v1 world `light` record (may migrate)
  float worldLight[8] = {0, -1, 0, 1, 1, 1, 1, 1};  // dxyz, rgba, strength
  u32 activeCamera = 0;              // v2+: active-camera id, 0 = none
  u32 nextCollectionId = 1;          // v2: id allocator for collections
  float background[5] = {0.05f, 0.05f, 0.06f, 1.0f, 1.0f};  // core default
  bool backgroundLighting = false;   // D22(d): flat-ambient switch
  bool hadBackground = false;        // worldbg record actually present?
  int frameStart = 1, frameEnd = 250, frameCurrent = 1;     // item 86
  float cursor[3] = {0.0f, 0.0f, 0.0f};                    // item 157, world origin default
  bool hadCursor = false;                                   // optional/tolerant v2 record
  ember::SceneRenderSettings renderSettings;
  bool hadRenderSettings = false;
  bool hadEnvironmentTexture = false;
  ember::ImageTextureRef environmentTexture;
  std::vector<u32> selection;
  std::vector<ObjectRecord> objects;
  std::vector<LightRecord> lights;   // v2 per-object records
  std::vector<CameraRecord> cameras; // v3 per-object projection records
  std::vector<CollectionRecord> collections;
  std::vector<MembershipRecord> memberships;
  std::vector<SlotRecord> slots;
  std::vector<TextureRecord> textures;
  std::vector<EditableRecord> editables;
};

// The fixed record order of FORMAT.md §2:
//   v1-v3: prior layouts (migrate material emission to black/zero)
//   v4: v3 layout with emission fields in every material record
//   v5: v4 layout with transmission and IOR in every material record
//   v6: v5 layout plus the scene-level render settings record
//   v7: v6 layout plus image-reference records
//   v8: v7 layout plus editable mesh records
// Anything else fails.
SceneFile parseAll(const std::string& text) {
  Scanner sc(text);
  SceneFile f;

  needWord(sc, "EMBER3D.SCENE");
  {
    std::string version;
    if (!sc.next(version)) sc.fail("missing version token");
    if (version.size() < 2 || version[0] != 'v')
      sc.fail("malformed version token: '" + version + "'");
    int v = 0;
    for (std::size_t i = 1; i < version.size(); ++i) {
      if (version[i] < '0' || version[i] > '9')
        sc.fail("malformed version token: '" + version + "'");
      v = v * 10 + (version[i] - '0');
    }
    if (v < 1 || v > 8)
      sc.fail("unsupported version: '" + version + "'");
    f.version = v;
  }

  needWord(sc, "nextid");
  readUint(sc, f.nextId);

  if (f.version == 1) {
    needWord(sc, "light");                       // v1 world record (FORMAT §5)
    for (float& x : f.worldLight) readFloat(sc, x);
    f.hadWorldLight = true;
  }

  needWord(sc, "selection");
  {
    u32 count = 0;
    readUint(sc, count);
    f.selection.resize(count);
    for (u32& id : f.selection) readUint(sc, id);
  }

  if (f.version >= 2) {
    // Tolerant world records (FORMAT §2 note): `active` (active-camera id),
    // `nextcollection` (allocator state) and `worldbg` (D22(d) World state)
    // were added mid-v2, so a v2 reader accepts their ABSENCE (0 / 1 /
    // core default) wherever they are not present; the canonical writer
    // always emits them here, in this order.
    std::string rec;
    for (;;) {
      if (!sc.next(rec))
        sc.fail("unexpected end of file (camera records)");
      if (rec == "active") {
        if (f.hadRenderSettings)
          sc.fail("render settings must follow scene metadata");
        readUint(sc, f.activeCamera);
        continue;
      }
      if (rec == "nextcollection") {
        if (f.hadRenderSettings)
          sc.fail("render settings must follow scene metadata");
        readUint(sc, f.nextCollectionId);
        continue;
      }
      if (rec == "worldbg") {
        if (f.hadRenderSettings)
          sc.fail("render settings must follow scene metadata");
        // r g b a strength + lighting(0|1) (FORMAT §3)
        for (float& x : f.background) readFloat(sc, x);
        u32 lighting = 0;
        readUint(sc, lighting);
        f.backgroundLighting = lighting != 0;
        f.hadBackground = true;
        continue;
      }
      if (rec == "frame") {
        if (f.hadRenderSettings)
          sc.fail("render settings must follow scene metadata");
        // item 86: start end current (INTs; clamped on materialise).
        readInt(sc, f.frameStart);
        readInt(sc, f.frameEnd);
        readInt(sc, f.frameCurrent);
        continue;
      }
      if (rec == "cursor") {
        if (f.hadRenderSettings)
          sc.fail("render settings must follow scene metadata");
        // Item 157, D22: one optional v2 world-space position record.
        if (f.hadCursor) sc.fail("duplicate cursor record");
        for (float& x : f.cursor) readFloat(sc, x);
        f.hadCursor = true;
        continue;
      }
      if (rec == "render") {
        if (f.version < 6)
          sc.fail("render settings require scene format v6");
        if (f.hadRenderSettings) sc.fail("duplicate render settings record");
        std::string engine;
        if (!sc.next(engine)) sc.fail("missing render engine");
        if (engine == "raster")
          f.renderSettings.engine = ember::RenderEngine::Raster;
        else if (engine == "cycles")
          f.renderSettings.engine = ember::RenderEngine::Cycles;
        else
          sc.fail("unknown render engine: '" + engine + "'");
        readUint(sc, f.renderSettings.samplesPerPixel);
        readUint(sc, f.renderSettings.maxBounces);
        readUint(sc, f.renderSettings.seed);
        f.renderSettings = ember::sanitizedSceneRenderSettings(f.renderSettings);
        f.hadRenderSettings = true;
        continue;
      }
      sc.pushBack(rec);
      break;
    }
  }

  bool sawEnd = false;
  bool sawImageRecords = false;
  bool sawEnvironmentTexture = false;
  bool sawEditableRecords = false;
  std::string tok;
  while (sc.next(tok)) {
    if (tok == "end") { sawEnd = true; break; }
    if (sawImageRecords && tok != "basetexture" && tok != "slottexture" &&
        tok != "environmenttexture")
      sc.fail("image reference records must follow all scene records");
    if (sawEditableRecords && tok != "editable" && tok != "basetexture" &&
        tok != "slottexture" && tok != "environmenttexture")
      sc.fail("editable mesh records must follow all scene records");
    if (tok == "light") {
      // v2 per-object light record (FORMAT §3).
      LightRecord r;
      readUint(sc, r.object);
      if (r.object == 0) sc.fail("light record for object id 0 is illegal");
      std::string kind;
      if (!sc.next(kind)) sc.fail("missing light kind");
      if (kind == "directional")   r.isDirectional = true;
      else if (kind == "point")    r.isDirectional = false;
      else                         sc.fail("unknown light kind: '" + kind + "'");
      for (float& x : r.color) readFloat(sc, x);
      readFloat(sc, r.intensity);
      readFloat(sc, r.radius);
      for (const LightRecord& seen : f.lights)
        if (seen.object == r.object)
          sc.fail("duplicate light record for one object");
      f.lights.push_back(std::move(r));
      continue;
    }
    if (tok == "camera") {
      if (f.version < 3)
        sc.fail("camera records require scene format v3");
      CameraRecord r;
      readUint(sc, r.object);
      if (r.object == 0) sc.fail("camera record for object id 0 is illegal");
      r.settings.isCamera = true;
      readFloat(sc, r.settings.fovY);
      readFloat(sc, r.settings.clipNear);
      readFloat(sc, r.settings.clipFar);
      u32 orthographic = 0;
      readUint(sc, orthographic);
      if (orthographic > 1) sc.fail("orthographic camera value must be 0 or 1");
      r.settings.orthographic = orthographic != 0;
      readFloat(sc, r.settings.orthoScale);
      for (const CameraRecord& seen : f.cameras)
        if (seen.object == r.object)
          sc.fail("duplicate camera record for one object");
      f.cameras.push_back(std::move(r));
      continue;
    }
    if (tok == "collection") {
      // D22: the named grouping tree record (v2 only — see FORMAT §3).
      CollectionRecord c;
      readUint(sc, c.id);
      if (c.id == 0) sc.fail("collection id 0 is illegal");
      readUint(sc, c.parent);
      if (!sc.next(c.name)) sc.fail("missing collection name");
      std::string vis;
      if (!sc.next(vis)) sc.fail("missing collection visibility");
      if (vis == "visible")       c.visible = true;
      else if (vis == "hidden")   c.visible = false;
      else                        sc.fail("unknown collection visibility: '" +
                                          vis + "'");
      for (const CollectionRecord& seen : f.collections)
        if (seen.id == c.id) sc.fail("duplicate collection id");
      f.collections.push_back(std::move(c));
      continue;
    }
    if (tok == "incollection") {
      // D22: an object's membership record (absent = implicit root).
      MembershipRecord m;
      readUint(sc, m.object);
      if (m.object == 0) sc.fail("membership for object id 0 is illegal");
      readUint(sc, m.collection);
      for (const MembershipRecord& seen : f.memberships)
        if (seen.object == m.object)
          sc.fail("duplicate membership record for one object");
      f.memberships.push_back(std::move(m));
      continue;
    }
    if (tok == "materialslot") {
      // D22(c): a slot index >= 1 (slot 0 is the object record's floats).
      SlotRecord s;
      readUint(sc, s.object);
      if (s.object == 0) sc.fail("materialslot for object id 0 is illegal");
      readUint(sc, s.index);
      if (s.index == 0)
        sc.fail("materialslot slot index 0 is illegal (use the object record)");
      if (!sc.next(s.name)) sc.fail("missing material slot name");
      for (float& x : s.color) readFloat(sc, x);
      readFloat(sc, s.metallic);
      readFloat(sc, s.roughness);
      if (f.version >= 4) {
        for (float& x : s.emissionColor) readFloat(sc, x);
        readFloat(sc, s.emissionStrength);
      }
      if (f.version >= 5) {
        readFloat(sc, s.transmission);
        readFloat(sc, s.ior);
      }
      for (const SlotRecord& seen : f.slots)
        if (seen.object == s.object && seen.index == s.index)
          sc.fail("duplicate material slot index for one object");
      f.slots.push_back(std::move(s));
      continue;
    }
    if (tok == "basetexture" || tok == "slottexture") {
      if (f.version < 7)
        sc.fail("image texture records require scene format v7");
      if (sawEnvironmentTexture)
        sc.fail("material texture records must precede environmenttexture");
      sawImageRecords = true;
      TextureRecord texture;
      readUint(sc, texture.object);
      if (texture.object == 0)
        sc.fail("image texture record for object id 0 is illegal");
      if (tok == "slottexture") {
        readUint(sc, texture.slot);
        if (texture.slot == 0)
          sc.fail("slottexture index 0 is illegal (use basetexture)");
      }
      if (!sc.next(texture.texture.path)) sc.fail("missing image path");
      if (!validTexturePath(texture.texture.path))
        sc.fail("image path contains a null byte");
      texture.texture.colorSpace = readImageColorSpace(sc);
      for (const TextureRecord& seen : f.textures)
        if (seen.object == texture.object && seen.slot == texture.slot)
          sc.fail("duplicate image texture record for one material slot");
      f.textures.push_back(std::move(texture));
      continue;
    }
    if (tok == "environmenttexture") {
      if (f.version < 7)
        sc.fail("environment texture records require scene format v7");
      if (f.hadEnvironmentTexture)
        sc.fail("duplicate environment texture record");
      sawImageRecords = true;
      sawEnvironmentTexture = true;
      if (!sc.next(f.environmentTexture.path)) sc.fail("missing environment image path");
      if (!validTexturePath(f.environmentTexture.path))
        sc.fail("environment image path contains a null byte");
      f.environmentTexture.colorSpace = readImageColorSpace(sc);
      f.hadEnvironmentTexture = true;
      continue;
    }
    if (tok == "editable") {
      if (f.version < 8)
        sc.fail("editable mesh records require scene format v8");
      if (sawImageRecords)
        sc.fail("editable mesh records must precede image references");
      sawEditableRecords = true;
      EditableRecord record;
      readUint(sc, record.object);
      if (record.object == 0) sc.fail("editable record for object id 0 is illegal");
      for (const EditableRecord& seen : f.editables)
        if (seen.object == record.object)
          sc.fail("duplicate editable mesh record for one object");

      u32 vertexCount = 0, normalCount = 0, edgeCount = 0;
      u32 faceCount = 0, loopCount = 0;
      readUint(sc, vertexCount);
      readUint(sc, normalCount);
      readUint(sc, edgeCount);
      readUint(sc, faceCount);
      readUint(sc, loopCount);
      readUint(sc, record.mesh.topologyVersion);
      if (vertexCount > kMaxEditableElements || normalCount > kMaxEditableElements ||
          edgeCount > kMaxEditableElements || faceCount > kMaxEditableElements ||
          loopCount > kMaxEditableElements)
        sc.fail("editable mesh count exceeds the v8 safety limit");
      if (normalCount != 0 && normalCount != vertexCount)
        sc.fail("editable normals must be absent or parallel to positions");

      record.mesh.positions.resize(vertexCount);
      record.mesh.normals.resize(normalCount);
      record.mesh.edges.resize(edgeCount);
      record.mesh.faces.resize(faceCount);
      record.mesh.loops.resize(loopCount);
      const auto readIndexed = [&](const char* word, u32 expected) {
        needWord(sc, word);
        u32 index = 0;
        readUint(sc, index);
        if (index != expected)
          sc.fail(std::string(word) + " indices must be dense and ordered");
      };
      for (u32 i = 0; i < vertexCount; ++i) {
        readIndexed("editposition", i);
        Vec3& p = record.mesh.positions[i];
        readFloat(sc, p.x); readFloat(sc, p.y); readFloat(sc, p.z);
      }
      for (u32 i = 0; i < normalCount; ++i) {
        readIndexed("editnormal", i);
        Vec3& n = record.mesh.normals[i];
        readFloat(sc, n.x); readFloat(sc, n.y); readFloat(sc, n.z);
      }
      for (u32 i = 0; i < edgeCount; ++i) {
        readIndexed("editedge", i);
        readUint(sc, record.mesh.edges[i].v0);
        readUint(sc, record.mesh.edges[i].v1);
      }
      for (u32 i = 0; i < faceCount; ++i) {
        readIndexed("editface", i);
        Face& face = record.mesh.faces[i];
        readUint(sc, face.firstLoop);
        readUint(sc, face.loopCount);
        readUint(sc, face.materialIndex);
      }
      for (u32 i = 0; i < loopCount; ++i) {
        readIndexed("editloop", i);
        Loop& loop = record.mesh.loops[i];
        readUint(sc, loop.vertex);
        readUint(sc, loop.edge);
        readUint(sc, loop.next);
        readUint(sc, loop.prev);
        readUint(sc, loop.face);
        readFloat(sc, loop.uv.x);
        readFloat(sc, loop.uv.y);
      }
      needWord(sc, "endedit");
      f.editables.push_back(std::move(record));
      continue;
    }
    if (tok != "object")
      sc.fail("unknown record: '" + tok + "'");

    ObjectRecord r;
    readUint(sc, r.id);
    if (r.id == 0) sc.fail("object id 0 is illegal");

    if (!sc.next(r.name)) sc.fail("missing object name");

    readUint(sc, r.parent);

    std::string kind;
    if (!sc.next(kind)) sc.fail("missing mesh kind");
    if (!meshKindFromWord(kind, r.hasMesh, r.primitive))
      sc.fail("unknown mesh kind: '" + kind + "'");

    std::string vis;
    if (!sc.next(vis)) sc.fail("missing visibility");
    if (vis == "visible")       r.visible = true;
    else if (vis == "hidden")   r.visible = false;
    else                        sc.fail("unknown visibility: '" + vis + "'");

    for (float& x : r.pos) readFloat(sc, x);
    for (float& x : r.rot) readFloat(sc, x);
    for (float& x : r.scl) readFloat(sc, x);
    for (float& x : r.color) readFloat(sc, x);
    readFloat(sc, r.metallic);
    readFloat(sc, r.roughness);
    if (f.version >= 4) {
      for (float& x : r.emissionColor) readFloat(sc, x);
      readFloat(sc, r.emissionStrength);
    }
    if (f.version >= 5) {
      readFloat(sc, r.transmission);
      readFloat(sc, r.ior);
    }

    for (const ObjectRecord& seen : f.objects)
      if (seen.id == r.id) sc.fail("duplicate object id");
    f.objects.push_back(std::move(r));
  }
  if (!sawEnd) sc.fail("missing 'end' terminator");
  if (!sc.atEnd()) sc.fail("trailing content after 'end'");  // FORMAT §3/§5.10
  if (f.version >= 6 && !f.hadRenderSettings)
    sc.fail("v6-v8 file is missing its render settings record");

  // nextid must be a legal id larger than every object id (FORMAT §3).
  if (f.nextId == 0) sc.fail("nextid 0 is illegal");
  for (const ObjectRecord& r : f.objects)
    if (r.id >= f.nextId) sc.fail("nextid must exceed every object id");

  // Parent links: must reference existing ids, never self, never cyclic.
  std::unordered_map<u32, const ObjectRecord*> byId;
  byId.reserve(f.objects.size());
  for (const ObjectRecord& r : f.objects) byId[r.id] = &r;
  for (const ObjectRecord& r : f.objects) {
    if (r.parent == 0) continue;
    const auto it = byId.find(r.parent);
    if (it == byId.end())
      sc.fail("parent id names no object in this file");
    if (r.parent == r.id) sc.fail("object is its own parent");
  }
  for (const ObjectRecord& r : f.objects) {
    u32 hop = r.parent;
    auto steps = f.objects.size() + 1;
    while (hop != 0 && steps--) {          // bounded walk: a cycle fails
      hop = byId.at(hop)->parent;
    }
    if (hop != 0) sc.fail("parent cycle detected");
  }

  // Selection ids must name objects in the file.
  for (u32 id : f.selection)
    if (byId.find(id) == byId.end())
      sc.fail("selection id names no object in this file");

  // Light records (v2+): must name objects in the file. v1 files may not
  // carry them (FORMAT §2: no place in the v1 grammar).
  if (f.version == 1 && !f.lights.empty())
    sc.fail("per-object light records are invalid in a v1 file");
  for (const LightRecord& r : f.lights)
    if (byId.find(r.object) == byId.end())
      sc.fail("light record names no object in this file");

  // Camera records (v3): must name objects and contain a valid projection.
  if (f.version < 3 && !f.cameras.empty())
    sc.fail("camera records are invalid before v3");
  for (const CameraRecord& r : f.cameras) {
    if (byId.find(r.object) == byId.end())
      sc.fail("camera record names no object in this file");
    if (!validCameraProjection(r.settings))
      sc.fail("camera projection values are outside their valid ranges");
  }

  // Collections (D22; v2 only — v1 has no place for them in the grammar).
  const auto forbidCollectionsInV1 = [&]() {
    if (f.version == 1 &&
        (!f.collections.empty() || !f.memberships.empty() ||
         !f.slots.empty()))
      sc.fail("collection/material-slot records are invalid in a v1 file");
    if (f.version == 1 && f.activeCamera != 0)
      sc.fail("an active record is invalid in a v1 file");
  };
  forbidCollectionsInV1();

  std::unordered_map<u32, const CollectionRecord*> byColId;
  byColId.reserve(f.collections.size());
  for (const CollectionRecord& c : f.collections) byColId[c.id] = &c;
  if (f.nextCollectionId == 0)
    sc.fail("nextcollection 0 is illegal");
  for (const CollectionRecord& c : f.collections)
    if (c.id >= f.nextCollectionId)
      sc.fail("nextcollection must exceed every collection id");
  for (const CollectionRecord& c : f.collections) {
    if (c.parent == 0) continue;
    if (byColId.find(c.parent) == byColId.end())
      sc.fail("collection parent names no collection in this file");
    if (c.parent == c.id) sc.fail("collection is its own parent");
  }
  for (const CollectionRecord& c : f.collections) {   // bounded cycle walk
    u32 hop = c.parent;
    auto steps = f.collections.size() + 1;
    while (hop != 0 && steps--) hop = byColId.at(hop)->parent;
    if (hop != 0) sc.fail("collection parent cycle detected");
  }
  // membership + slot records must name objects; memberships a collection.
  for (const MembershipRecord& m : f.memberships) {
    if (byId.find(m.object) == byId.end())
      sc.fail("membership names no object in this file");
    if (m.collection != 0 && byColId.find(m.collection) == byColId.end())
      sc.fail("membership names no collection in this file");
  }
  for (const SlotRecord& s : f.slots)
    if (byId.find(s.object) == byId.end())
      sc.fail("materialslot names no object in this file");
  for (const TextureRecord& texture : f.textures) {
    if (byId.find(texture.object) == byId.end())
      sc.fail("image texture names no object in this file");
    if (texture.slot != 0 &&
        std::none_of(f.slots.begin(), f.slots.end(), [&](const SlotRecord& s) {
          return s.object == texture.object && s.index == texture.slot;
        }))
      sc.fail("slot texture names no serialized material slot");
  }
  for (EditableRecord& record : f.editables) {
    const auto objectIt = byId.find(record.object);
    if (objectIt == byId.end()) sc.fail("editable mesh names no object in this file");
    if (!objectIt->second->hasMesh)
      sc.fail("editable mesh must belong to a meshed object");
    if (std::any_of(f.lights.begin(), f.lights.end(), [&](const LightRecord& light) {
          return light.object == record.object;
        }))
      sc.fail("editable mesh cannot belong to a light object");

    EditableMesh& mesh = record.mesh;
    if (mesh.normals.size() != 0 && mesh.normals.size() != mesh.positions.size())
      sc.fail("editable normals must be absent or parallel to positions");
    for (const Vec3& p : mesh.positions)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        sc.fail("editable position is non-finite");
    for (const Vec3& n : mesh.normals)
      if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z))
        sc.fail("editable normal is non-finite");

    std::unordered_map<u64, u32> edgeIds;
    edgeIds.reserve(mesh.edges.size());
    for (u32 i = 0; i < mesh.edges.size(); ++i) {
      const Edge& edge = mesh.edges[i];
      if (edge.v0 >= edge.v1 || edge.v1 >= mesh.positions.size())
        sc.fail("editable edge endpoints are invalid");
      const u64 key = (u64(edge.v0) << 32) | edge.v1;
      if (!edgeIds.emplace(key, i).second)
        sc.fail("editable mesh contains a duplicate edge");
    }

    u64 nextLoop = 0;
    std::vector<u32> edgeUses(mesh.edges.size(), 0);
    for (u32 fi = 0; fi < mesh.faces.size(); ++fi) {
      const Face& face = mesh.faces[fi];
      if (face.loopCount < 3 || face.firstLoop != nextLoop ||
          u64(face.firstLoop) + face.loopCount > mesh.loops.size())
        sc.fail("editable face loop ranges must be dense, in-range rings of at least 3");
      nextLoop += face.loopCount;
      if (face.materialIndex != 0 &&
          std::none_of(f.slots.begin(), f.slots.end(), [&](const SlotRecord& slot) {
            return slot.object == record.object && slot.index == face.materialIndex;
          }))
        sc.fail("editable face names a missing material slot");
      std::unordered_map<u32, bool> ringVertices;
      for (u32 k = 0; k < face.loopCount; ++k) {
        const u32 li = face.firstLoop + k;
        const u32 next = face.firstLoop + (k + 1) % face.loopCount;
        const u32 prev = face.firstLoop + (k + face.loopCount - 1) % face.loopCount;
        const Loop& loop = mesh.loops[li];
        const Loop& nextElem = mesh.loops[next];
        if (loop.face != fi || loop.next != next || loop.prev != prev ||
            loop.vertex >= mesh.positions.size() || loop.edge >= mesh.edges.size() ||
            !std::isfinite(loop.uv.x) || !std::isfinite(loop.uv.y))
          sc.fail("editable loop ring has an invalid index or attribute");
        if (!ringVertices.emplace(loop.vertex, true).second)
          sc.fail("editable face repeats a vertex");
        const u32 va = loop.vertex, vb = nextElem.vertex;
        if (va == vb) sc.fail("editable face contains a zero-length edge");
        const u32 lo = std::min(va, vb), hi = std::max(va, vb);
        const auto edge = edgeIds.find((u64(lo) << 32) | hi);
        if (edge == edgeIds.end() || edge->second != loop.edge)
          sc.fail("editable loop edge does not match its ring vertices");
        if (++edgeUses[loop.edge] > 2)
          sc.fail("editable edge has more than two incident faces");
      }
    }
    if (nextLoop != mesh.loops.size())
      sc.fail("editable loop pool contains unclaimed records");
    for (const u32 uses : edgeUses)
      if (uses == 0) sc.fail("editable mesh contains an orphan edge");
    std::vector<bool> usedVertices(mesh.positions.size(), false);
    for (const Loop& loop : mesh.loops) usedVertices[loop.vertex] = true;
    for (const bool used : usedVertices)
      if (!used) sc.fail("editable mesh contains an orphan vertex");

    // CSR is derived state. Rebuild only after validating every serialized
    // index, then run the EditableMesh invariant checker before materialising.
    mesh.rebuildAdjacency();
    std::string why;
    if (!mesh.validate(&why)) sc.fail("invalid editable mesh topology: " + why);
  }
  // Active camera pointer: 0 = none, else must name an object in the file.
  if (f.activeCamera != 0 && byId.find(f.activeCamera) == byId.end())
    sc.fail("active camera names no object in this file");
  if (f.version >= 3 && f.activeCamera != 0 &&
      std::none_of(f.cameras.begin(), f.cameras.end(),
                   [&](const CameraRecord& r) { return r.object == f.activeCamera; }))
    sc.fail("active camera id does not name a camera component");

  return f;
}

} // namespace

// ---------------------------------------------------------------- save

bool save(const World& world, const std::string& path) {
  for (const Object& o : world.objects_) {
    if (!o.hasEdit) continue;
    if (!o.hasMesh || o.isLight || o.materials.empty()) return false;
    EditableMesh mesh = o.edit;
    if (mesh.normals.size() != 0 && mesh.normals.size() != mesh.positions.size())
      return false;
    for (const Face& face : mesh.faces)
      if (face.materialIndex >= o.materials.size()) return false;
    // Validate the topology with a bounded, safe preflight before invoking the
    // checker, whose CSR path assumes all primary indices are already valid.
    if (mesh.positions.size() > kMaxEditableElements ||
        mesh.normals.size() > kMaxEditableElements ||
        mesh.edges.size() > kMaxEditableElements ||
        mesh.faces.size() > kMaxEditableElements ||
        mesh.loops.size() > kMaxEditableElements)
      return false;
    std::unordered_map<u64, u32> edgeIds;
    for (u32 i = 0; i < mesh.edges.size(); ++i) {
      const Edge& edge = mesh.edges[i];
      if (edge.v0 >= edge.v1 || edge.v1 >= mesh.positions.size()) return false;
      if (!edgeIds.emplace((u64(edge.v0) << 32) | edge.v1, i).second) return false;
    }
    u64 nextLoop = 0;
    std::vector<u32> edgeUses(mesh.edges.size(), 0);
    for (u32 fi = 0; fi < mesh.faces.size(); ++fi) {
      const Face& face = mesh.faces[fi];
      if (face.loopCount < 3 || face.firstLoop != nextLoop ||
          u64(face.firstLoop) + face.loopCount > mesh.loops.size()) return false;
      nextLoop += face.loopCount;
      std::unordered_map<u32, bool> ringVertices;
      for (u32 k = 0; k < face.loopCount; ++k) {
        const u32 li = face.firstLoop + k;
        const u32 next = face.firstLoop + (k + 1) % face.loopCount;
        const u32 prev = face.firstLoop + (k + face.loopCount - 1) % face.loopCount;
        const Loop& loop = mesh.loops[li];
        const Loop& nextElem = mesh.loops[next];
        if (loop.face != fi || loop.next != next || loop.prev != prev ||
            loop.vertex >= mesh.positions.size() || loop.edge >= mesh.edges.size() ||
            !std::isfinite(loop.uv.x) || !std::isfinite(loop.uv.y) ||
            !ringVertices.emplace(loop.vertex, true).second || loop.vertex == nextElem.vertex)
          return false;
        const u32 lo = std::min(loop.vertex, nextElem.vertex);
        const u32 hi = std::max(loop.vertex, nextElem.vertex);
        const auto edge = edgeIds.find((u64(lo) << 32) | hi);
        if (edge == edgeIds.end() || edge->second != loop.edge ||
            ++edgeUses[loop.edge] > 2) return false;
      }
    }
    if (nextLoop != mesh.loops.size() ||
        std::any_of(edgeUses.begin(), edgeUses.end(), [](u32 n) { return n == 0; }))
      return false;
    std::vector<bool> used(mesh.positions.size(), false);
    for (const Loop& loop : mesh.loops) {
      if (loop.vertex >= used.size()) return false;
      used[loop.vertex] = true;
    }
    if (std::any_of(used.begin(), used.end(), [](bool value) { return !value; })) return false;
    for (const Vec3& p : mesh.positions)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    for (const Vec3& n : mesh.normals)
      if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) return false;
    mesh.rebuildAdjacency();
    if (!mesh.validate()) return false;
  }
  for (const Object& o : world.objects_)
    if (o.camera.isCamera && !validCameraProjection(o.camera)) return false;
  if (world.activeCamera_.valid()) {
    const Object* active = world.get(world.activeCamera_);
    if (!active || !active->camera.isCamera) return false;
  }

  struct SavedTexture {
    u32 object;
    u32 slot;
    std::string path;
    std::string colorSpace;
  };
  std::vector<SavedTexture> savedTextures;
  std::string savedEnvironmentPath;
  std::filesystem::path outputPath;
  std::filesystem::path destinationBase;
  std::filesystem::path sourceBase;
  try {
    std::error_code error;
    const std::filesystem::path scenePath =
        std::filesystem::absolute(std::filesystem::path(path), error);
    if (error) return false;
    outputPath = scenePath;
    destinationBase = scenePath.parent_path().lexically_normal();
    sourceBase = world.assetBaseDirectory_.empty()
        ? std::filesystem::current_path(error)
        : std::filesystem::path(world.assetBaseDirectory_);
    if (error) return false;
    sourceBase = std::filesystem::absolute(sourceBase, error).lexically_normal();
    if (error) return false;

    const auto addTexture = [&](u32 object, u32 slot,
                                const ember::ImageTextureRef& texture) {
      std::string rebased;
      if (!pathForSavedScene(texture, sourceBase, destinationBase, rebased))
        return false;
      if (texture.path.empty()) return true;  // empty means unassigned
      const char* colorSpace = imageColorSpaceWord(texture.colorSpace);
      savedTextures.push_back(SavedTexture{object, slot, std::move(rebased),
                                           colorSpace});
      return true;
    };
    for (const Object& object : world.objects_) {
      if (!addTexture(object.id.value, 0, object.material.baseColorTexture))
        return false;
      for (u32 slot = 1; slot < object.materials.size(); ++slot)
        if (!addTexture(object.id.value, slot,
                        object.materials[slot].material.baseColorTexture))
          return false;
    }
    if (!pathForSavedScene(world.background_.environmentTexture, sourceBase,
                           destinationBase, savedEnvironmentPath))
      return false;
  } catch (const std::filesystem::filesystem_error&) {
    return false;
  }

  std::ofstream os(outputPath, std::ios::binary | std::ios::trunc);
  if (!os.is_open()) return false;

  os << "EMBER3D.SCENE v" << kFormatVersion << '\n';
  os << "nextid " << world.nextId_ << '\n';

  os << "selection " << world.selection_.size();
  for (ObjectId id : world.selection_) os << ' ' << id.value;
  os << '\n';

  // v8 retains the active-camera pointer + collection id allocator (FORMAT §3).
  os << "active " << world.activeCamera_.value << '\n';
  os << "nextcollection " << world.nextCollectionId_ << '\n';
  // D22(d) world background: r g b a strength lighting(0|1) (FORMAT §3).
  os << "worldbg " << world.background_.color.r << ' '
     << world.background_.color.g << ' ' << world.background_.color.b << ' '
     << world.background_.color.a << ' ' << world.background_.strength << ' '
     << (world.background_.lighting ? 1 : 0) << '\n';
  // item 86: the scene's frame state (start end current; FORMAT §3).
  os << "frame " << world.frameStart_ << ' ' << world.frameEnd_ << ' '
     << world.frameCurrent_ << '\n';

  os << std::setprecision(9);  // FORMAT.md §1: 9 significant digits, floats
  os << "cursor " << world.cursor_.x << ' ' << world.cursor_.y << ' '
     << world.cursor_.z << '\n';
  const ember::SceneRenderSettings renderSettings =
      ember::sanitizedSceneRenderSettings(world.renderSettings_);
  os << "render "
     << (renderSettings.engine == ember::RenderEngine::Cycles ? "cycles" : "raster")
     << ' ' << renderSettings.samplesPerPixel << ' '
     << renderSettings.maxBounces << ' ' << renderSettings.seed << '\n';
  for (const Object& o : world.objects_) {
    const MaterialDesc material = ember::sanitizedMaterialDesc(o.material);
    os << "object " << o.id.value << ' ' << escapeQuoted(o.name) << ' '
       << o.parent.value << ' '
       << (o.hasMesh ? meshKindWord(o.primitive) : "none") << ' '
       << (o.visible ? "visible" : "hidden") << ' ';
    os << o.transform.position.x << ' ' << o.transform.position.y << ' '
       << o.transform.position.z << ' ';
    os << o.transform.rotation.x << ' ' << o.transform.rotation.y << ' '
       << o.transform.rotation.z << ' ';
    os << o.transform.scale.x << ' ' << o.transform.scale.y << ' '
       << o.transform.scale.z << ' ';
    os << material.baseColor.r << ' ' << material.baseColor.g << ' '
       << material.baseColor.b << ' ' << material.baseColor.a << ' ';
    os << material.metallic << ' ' << material.roughness << ' '
       << material.emissionColor.r << ' ' << material.emissionColor.g << ' '
       << material.emissionColor.b << ' ' << material.emissionStrength << ' '
       << material.transmission << ' ' << material.ior << '\n';
  }
  // v2 per-object light records (FORMAT §3): exactly one per light object,
  // in creation order, after all object records.
  for (const Object& o : world.objects_) {
    if (!o.isLight) continue;
    os << "light " << o.id.value << ' '
       << (o.light.type == ember::LightType::Directional ? "directional"
                                                         : "point") << ' ';
    os << o.light.color.r << ' ' << o.light.color.g << ' '
       << o.light.color.b << ' ' << o.light.color.a << ' ';
    os << o.light.intensity << ' ' << o.light.radius << '\n';
  }

  // v3 camera projection records, in object creation order.
  for (const Object& o : world.objects_) {
    if (!o.camera.isCamera) continue;
    os << "camera " << o.id.value << ' ' << o.camera.fovY << ' '
       << o.camera.clipNear << ' ' << o.camera.clipFar << ' '
       << (o.camera.orthographic ? 1 : 0) << ' ' << o.camera.orthoScale << '\n';
  }

  // D22 collections + memberships (FORMAT §3): creation order each; only
  // memberships that are not the implicit root are recorded.
  for (const Collection& c : world.collections_) {
    os << "collection " << c.id.value << ' ' << c.parent.value << ' '
       << escapeQuoted(c.name) << ' ' << (c.visible ? "visible" : "hidden")
       << '\n';
  }
  for (const Object& o : world.objects_) {
    if (!o.collection.valid()) continue;
    os << "incollection " << o.id.value << ' ' << o.collection.value << '\n';
  }

  // D22(c) material slots: every slot index >= 1, creation order.
  for (const Object& o : world.objects_) {
    for (std::size_t i = 1; i < o.materials.size(); ++i) {
      os << "materialslot " << o.id.value << ' ' << i << ' '
         << escapeQuoted(o.materials[i].name) << ' ';
      const MaterialDesc material =
          ember::sanitizedMaterialDesc(o.materials[i].material);
      os << material.baseColor.r << ' ' << material.baseColor.g << ' '
         << material.baseColor.b << ' ' << material.baseColor.a << ' ';
      os << material.metallic << ' ' << material.roughness << ' '
         << material.emissionColor.r << ' ' << material.emissionColor.g << ' '
         << material.emissionColor.b << ' ' << material.emissionStrength << ' '
         << material.transmission << ' ' << material.ior << '\n';
    }
  }
  // v8 editable topology. Derived CSR adjacency is rebuilt on load; the
  // authored positions, normals, edges, face rings, UVs and face slot indices
  // remain authoritative and are never applied to a regenerated primitive.
  for (const Object& o : world.objects_) {
    if (!o.hasEdit) continue;
    const EditableMesh& mesh = o.edit;
    os << "editable " << o.id.value << ' ' << mesh.positions.size() << ' '
       << mesh.normals.size() << ' ' << mesh.edges.size() << ' '
       << mesh.faces.size() << ' ' << mesh.loops.size() << ' '
       << mesh.topologyVersion << '\n';
    for (u32 i = 0; i < mesh.positions.size(); ++i) {
      const Vec3& p = mesh.positions[i];
      os << "editposition " << i << ' ' << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    for (u32 i = 0; i < mesh.normals.size(); ++i) {
      const Vec3& n = mesh.normals[i];
      os << "editnormal " << i << ' ' << n.x << ' ' << n.y << ' ' << n.z << '\n';
    }
    for (u32 i = 0; i < mesh.edges.size(); ++i)
      os << "editedge " << i << ' ' << mesh.edges[i].v0 << ' '
         << mesh.edges[i].v1 << '\n';
    for (u32 i = 0; i < mesh.faces.size(); ++i) {
      const Face& face = mesh.faces[i];
      os << "editface " << i << ' ' << face.firstLoop << ' '
         << face.loopCount << ' ' << face.materialIndex << '\n';
    }
    for (u32 i = 0; i < mesh.loops.size(); ++i) {
      const Loop& loop = mesh.loops[i];
      os << "editloop " << i << ' ' << loop.vertex << ' ' << loop.edge << ' '
         << loop.next << ' ' << loop.prev << ' ' << loop.face << ' '
         << loop.uv.x << ' ' << loop.uv.y << '\n';
    }
    os << "endedit\n";
  }
  // v7 image references are separate records so v1-v6 material layouts stay
  // byte-for-byte readable. Paths have been rebased to the destination scene.
  for (const SavedTexture& texture : savedTextures) {
    os << (texture.slot == 0 ? "basetexture " : "slottexture ")
       << texture.object;
    if (texture.slot != 0) os << ' ' << texture.slot;
    os << ' ' << escapeQuoted(texture.path) << ' ' << texture.colorSpace << '\n';
  }
  if (!savedEnvironmentPath.empty()) {
    const char* colorSpace =
        imageColorSpaceWord(world.background_.environmentTexture.colorSpace);
    os << "environmenttexture " << escapeQuoted(savedEnvironmentPath) << ' '
       << colorSpace << '\n';
  }
  os << "end";
  os.flush();
  return os.good();
}

// ---------------------------------------------------------------- load

bool load(World& world, const std::string& path) {
  std::filesystem::path scenePath;
  try {
    scenePath = std::filesystem::absolute(std::filesystem::path(path)).lexically_normal();
  } catch (const std::filesystem::filesystem_error&) {
    return false;
  }
  std::ifstream is(scenePath, std::ios::binary);
  if (!is.is_open()) return false;
  const std::string text((std::istreambuf_iterator<char>(is)),
                         std::istreambuf_iterator<char>());
  if (is.bad()) return false;
  is.close();

  try {
    const SceneFile f = parseAll(text);

    // Materialise into a fresh world and swap on success only (FORMAT §1/§7).
    // This body lives inside load() — the member access is friend-granted
    // (friendship is not transitive, so no helper may do this for us).
    World w;
    w.assetBaseDirectory_ = scenePath.parent_path().generic_string();
    w.nextId_ = f.nextId;

    for (const ObjectRecord& r : f.objects) {
      Object o;
      o.id = ObjectId{r.id};
      o.name = r.name;
      o.parent = ObjectId{r.parent};
      o.hasMesh = r.hasMesh;
      o.visible = r.visible;
      o.primitive = r.primitive;
      if (o.hasMesh) {
        const auto editIt = std::find_if(f.editables.begin(), f.editables.end(),
                                         [&](const EditableRecord& e) {
                                           return e.object == r.id;
                                         });
        if (editIt != f.editables.end()) {
          o.hasEdit = true;
          o.edit = editIt->mesh;
          o.mesh = o.edit.toMeshData();
        } else {
          o.mesh = makePrimitiveMesh(o.primitive);
        }
        o.mesh.name = meshKindDisplayName(o.primitive);
      }
      o.transform.position = Vec3{r.pos[0], r.pos[1], r.pos[2]};
      o.transform.rotation = Vec3{r.rot[0], r.rot[1], r.rot[2]};
      o.transform.scale    = Vec3{r.scl[0], r.scl[1], r.scl[2]};
      o.material.baseColor = Color{r.color[0], r.color[1], r.color[2], r.color[3]};
      o.material.metallic  = r.metallic;
      o.material.roughness = r.roughness;
      o.material.emissionColor = Color{r.emissionColor[0], r.emissionColor[1],
                                      r.emissionColor[2], 1.0f};
      o.material.emissionStrength = r.emissionStrength;
      o.material.transmission = r.transmission;
      o.material.ior = r.ior;
      o.material = ember::sanitizedMaterialDesc(o.material);
      ember::MaterialSlot baseSlot;
      baseSlot.material = o.material;
      o.materials.push_back(std::move(baseSlot));
      w.objects_.push_back(std::move(o));
    }

    // children lists re-derived from the parent fields (never stored).
    for (const Object& o : w.objects_) {
      if (!o.parent.valid()) continue;
      const auto it = std::find_if(w.objects_.begin(), w.objects_.end(),
                                   [&](const Object& e) {
                                     return e.id == o.parent;
                                   });
      // (parent existence/cycles were validated in parseAll; cannot miss)
      it->children.push_back(o.id);
    }

    for (u32 id : f.selection)
      w.selection_.push_back(ObjectId{id});

    if (f.hadBackground) {
      // Only a present record overwrites the core-default member; absent
      // records leave the World's default-constructed background intact.
      w.background_.color = Color{f.background[0], f.background[1],
                                  f.background[2], f.background[3]};
      w.background_.strength = f.background[4];
      w.background_.lighting = f.backgroundLighting;
    }
    // item 86: frame state, normalised by the setters' invariants on load.
    w.setFrameStart(f.frameStart);
    w.setFrameEnd(f.frameEnd);
    w.setFrameCurrent(f.frameCurrent);
    if (f.hadCursor)
      w.cursor_ = Vec3{f.cursor[0], f.cursor[1], f.cursor[2]};
    w.setRenderSettings(f.renderSettings);

    // D22 collections + memberships + slots materialize after the objects.
    w.nextCollectionId_ = f.nextCollectionId;
    for (const CollectionRecord& c : f.collections) {
      Collection col;
      col.id = CollectionId{c.id};
      col.parent = CollectionId{c.parent};
      col.name = c.name;
      col.visible = c.visible;
      w.collections_.push_back(std::move(col));
    }
    for (const MembershipRecord& m : f.memberships)
      if (Object* o =
              w.get(ObjectId{m.object}))
        o->collection = CollectionId{m.collection};
    for (const SlotRecord& s : f.slots) {
      if (Object* o = w.get(ObjectId{s.object})) {
        ember::MaterialSlot slot;
        slot.name = s.name;
        slot.material.baseColor = Color{s.color[0], s.color[1], s.color[2],
                                        s.color[3]};
        slot.material.metallic = s.metallic;
        slot.material.roughness = s.roughness;
        slot.material.emissionColor = Color{s.emissionColor[0],
                                            s.emissionColor[1],
                                            s.emissionColor[2], 1.0f};
        slot.material.emissionStrength = s.emissionStrength;
        slot.material.transmission = s.transmission;
        slot.material.ior = s.ior;
        slot.material = ember::sanitizedMaterialDesc(slot.material);
        const auto at = static_cast<std::size_t>(s.index);
        if (o->materials.size() <= at)
          o->materials.resize(at + 1);
        o->materials[at] = std::move(slot);
      }
    }

    // v7 image references are independent records. v1-v6 leave these default-
    // constructed (empty path, sRGB), which is the legacy migration contract.
    for (const TextureRecord& texture : f.textures) {
      Object* object = w.get(ObjectId{texture.object});
      if (!object) return false;  // validated above; defensive only
      if (texture.slot == 0) {
        object->material.baseColorTexture = texture.texture;
        if (!object->materials.empty())
          object->materials[0].material.baseColorTexture = texture.texture;
      } else {
        if (texture.slot >= object->materials.size()) return false;
        object->materials[texture.slot].material.baseColorTexture = texture.texture;
      }
    }
    if (f.hadEnvironmentTexture)
      w.background_.environmentTexture = f.environmentTexture;

    // v2 light records (FORMAT §3): attach the authored component.
    for (const LightRecord& r : f.lights) {
      const auto it = std::find_if(w.objects_.begin(), w.objects_.end(),
                                   [&](const Object& e) {
                                     return e.id.value == r.object;
                                   });
      // (object existence was validated in parseAll; cannot miss)
      it->isLight = true;
      it->light.type = r.isDirectional ? ember::LightType::Directional
                                       : ember::LightType::Point;
      it->light.color = Color{r.color[0], r.color[1], r.color[2], r.color[3]};
      it->light.intensity = r.intensity;
      it->light.radius = r.radius;
    }

    // v3 camera records attach projection settings to their owning objects.
    for (const CameraRecord& r : f.cameras) {
      Object* o = w.get(ObjectId{r.object});
      o->camera = r.settings;   // object existence was validated in parseAll
    }
    // v2's active id predated typed camera records. Preserve its meaning by
    // migrating the referenced object to a default camera component.
    if (f.version == 2 && f.activeCamera != 0) {
      if (Object* o = w.get(ObjectId{f.activeCamera})) o->camera.isCamera = true;
    }
    if (f.activeCamera != 0) {
      w.setActiveCamera(ObjectId{f.activeCamera});
      if (w.activeCamera().value != f.activeCamera) return false;
    }

    // §5 migration: a v1 world `light` record becomes one "Sun" object with a
    // rotation whose local -Z aims at the recorded direction. Derivation (XYZ
    // euler, R = Rz·Ry·Rx): d = (-sin y cos p, sin p, -cos y cos p) for the
    // rotation (p, y, 0), so p = asin(dy), y = atan2(-dx, -dz) on the
    // NORMALIZED direction — the migrated light resolves to the exact world
    // direction the v1 record carried.
    if (f.version == 1 && f.hadWorldLight) {
      Vec3 dir{f.worldLight[0], f.worldLight[1], f.worldLight[2]};
      const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
      if (len > 1e-8f) { dir.x /= len; dir.y /= len; dir.z /= len; }
      const float pitch = std::asin(dir.y);
      const float yaw   = std::atan2(-dir.x, -dir.z);

      std::string name = "Sun";
      uniquifyForLight(w.objects_, name);
      Object sun;
      sun.id = ObjectId{w.nextId_++};
      sun.name = name;
      sun.isLight = true;
      sun.light.type = ember::LightType::Directional;
      sun.light.color = Color{f.worldLight[3], f.worldLight[4],
                              f.worldLight[5], f.worldLight[6]};
      sun.light.intensity = f.worldLight[7];
      sun.light.radius = 1.0f;
      sun.transform.rotation = Vec3{pitch, yaw, 0.0f};
      w.objects_.push_back(std::move(sun));
    }

    // §6 default sun: every load produces at least one light. Applied when
    // the file carried no light object (and no v1 world record migrated):
    // "Sun", directional, white, strength 1, aimed straight down via the
    // documented default rotation x = -90 degrees = local (0,0,-1) -> down.
    if (std::none_of(w.objects_.begin(), w.objects_.end(),
                     [](const Object& e) { return e.isLight; })) {
      std::string name = "Sun";
      uniquifyForLight(w.objects_, name);
      Object sun;
      sun.id = ObjectId{w.nextId_++};
      sun.name = name;
      sun.isLight = true;
      sun.light.type = ember::LightType::Directional;
      sun.light.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
      sun.light.intensity = 1.0f;
      sun.light.radius = 1.0f;
      sun.transform.rotation = Vec3{-1.5707963f, 0.0f, 0.0f};
      w.objects_.push_back(std::move(sun));
    }

    world = std::move(w);
    return true;
  } catch (const ParseError&) {
    return false;  // target world untouched (FORMAT §1/§7)
  } catch (const std::filesystem::filesystem_error&) {
    return false;  // malformed/unrepresentable path, target remains untouched
  }
}

} // namespace ember::scene

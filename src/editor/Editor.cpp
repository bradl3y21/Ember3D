#include "editor/Editor.h"

#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "core/Log.h"
#include "editor/Panels.h"
#include "editor/Preferences.h"
#include "editor/SelfTest.h"
#include "editor/EditorLabels.h"
#include "editor/EditModeTools.h"
#include "editor/ThemeStyle.h"
#include "core/Theme.h"
#include "render/PathImageIO.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace ember::editor {

namespace {

constexpr double kCyclesSettleSeconds = 0.18;

// FNV-1a over packet values, never pointer addresses or std::hash. This key is
// deliberately content-based: it catches camera, viewport, geometry, material,
// light, background and render-setting edits without relying on a revision API
// that does not exist for every packet producer yet.
class PacketContentKey {
public:
    void addByte(std::uint8_t value) {
        value_ ^= value;
        value_ *= 1099511628211ull;
    }
    template <typename T>
    void addInteger(T value) {
        using U = std::make_unsigned_t<T>;
        U bits = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(U); ++i) {
            addByte(static_cast<std::uint8_t>(bits & U{0xff}));
            bits >>= 8;
        }
    }
    void addBool(bool value) { addByte(value ? 1u : 0u); }
    void addFloat(float value) { addInteger(std::bit_cast<std::uint32_t>(value)); }
    void addVec3(const Vec3& value) { addFloat(value.x); addFloat(value.y); addFloat(value.z); }
    void addColor(const Color& value) {
        addFloat(value.r); addFloat(value.g); addFloat(value.b); addFloat(value.a);
    }
    void addMatrix(const Mat4& value) {
        for (float component : value.m) addFloat(component);
    }
    void addMaterial(const MaterialDesc& material) {
        addColor(material.baseColor);
        addFloat(material.metallic);
        addFloat(material.roughness);
        addColor(material.emissionColor);
        addFloat(material.emissionStrength);
        addFloat(material.transmission);
        addFloat(material.ior);
    }
    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_ = 14695981039346656037ull;
};

std::uint64_t packetContentKey(const RenderPacket& packet) {
    PacketContentKey key;
    key.addInteger(packet.width);
    key.addInteger(packet.height);
    key.addInteger(static_cast<u8>(packet.renderSettings.engine));
    key.addInteger(packet.renderSettings.samplesPerPixel);
    key.addInteger(packet.renderSettings.maxBounces);
    key.addInteger(packet.renderSettings.seed);
    key.addVec3(packet.cameraPosition);
    key.addMatrix(packet.view);
    key.addMatrix(packet.projection);
    key.addColor(packet.background.color);
    key.addFloat(packet.background.strength);
    key.addBool(packet.background.lighting);

    key.addInteger(packet.lights.size());
    for (const LightInstance& light : packet.lights) {
        key.addInteger(static_cast<u8>(light.type));
        key.addVec3(light.position);
        key.addVec3(light.direction);
        key.addColor(light.color);
        key.addFloat(light.intensity);
        key.addFloat(light.radius);
    }

    key.addInteger(packet.items.size());
    for (const DrawItem& item : packet.items) {
        key.addInteger(item.object.value);
        key.addMatrix(item.model);
        key.addMaterial(item.material);
        key.addBool(item.mesh != nullptr);
        if (item.mesh) {
            key.addBool(item.hasGeometryVersion);
            if (item.hasGeometryVersion) {
                key.addInteger(item.geometryVersion);
                key.addInteger(item.modifierVersion);
            } else {
                // External or hand-built packets may not have scene mutation
                // stamps, so retain a content-hash fallback for correctness.
                key.addInteger(item.mesh->positions.size());
                for (float value : item.mesh->positions) key.addFloat(value);
                key.addInteger(item.mesh->normals.size());
                for (float value : item.mesh->normals) key.addFloat(value);
                key.addInteger(item.mesh->uvs.size());
                for (float value : item.mesh->uvs) key.addFloat(value);
                key.addInteger(item.mesh->indices.size());
                for (u32 value : item.mesh->indices) key.addInteger(value);
            }
        }
        key.addInteger(item.materialCount);
        for (u32 i = 0; item.materials && i < item.materialCount; ++i) {
            key.addMaterial(item.materials[i].material);
        }
        key.addInteger(item.triangleMaterialSlotCount);
        for (u32 i = 0; item.triangleMaterialSlots && i < item.triangleMaterialSlotCount; ++i)
            key.addInteger(item.triangleMaterialSlots[i]);
    }
    return key.value();
}

void drawCyclesViewportStatus(const CyclesViewportProgress& progress,
                              bool* cancelRequested, const ImVec4& viewportBody,
                              char* savePath, int* saveFormat,
                              ViewportPreviewSaveState saveState,
                              const std::filesystem::path& savedPath,
                              bool* saveRequested) {
    if (!progress.visible || viewportBody.z <= 1.0f || viewportBody.w <= 1.0f) return;

    const bool cancellable = progress.settling || progress.state == render::OfflineRenderState::Running;
    char message[96]{};
    if (progress.settling) {
        std::snprintf(message, sizeof(message), "Cycles CPU: waiting for interaction to settle");
    } else if (progress.state == render::OfflineRenderState::Running) {
        std::snprintf(message, sizeof(message), "Cycles CPU: %u / %u samples",
                      progress.samplesDone, progress.targetSamples);
    } else if (progress.state == render::OfflineRenderState::Completed) {
        std::snprintf(message, sizeof(message), "Cycles CPU: complete (%u samples)",
                      progress.samplesDone);
    } else if (progress.state == render::OfflineRenderState::Cancelled) {
        std::snprintf(message, sizeof(message), "Cycles CPU: cancelled");
    } else if (progress.state == render::OfflineRenderState::Failed) {
        std::snprintf(message, sizeof(message), "Cycles CPU: render failed");
    } else {
        std::snprintf(message, sizeof(message), "Cycles CPU: preparing");
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(viewportBody.x, viewportBody.y),
                           ImVec2(viewportBody.x + viewportBody.z,
                                  viewportBody.y + viewportBody.w), true);
    const ImVec2 min(viewportBody.x + 12.0f, viewportBody.y + 12.0f);
    const bool canSave = progress.hasPreview &&
                         progress.state == render::OfflineRenderState::Completed && savePath &&
                         saveFormat && saveRequested;
    const float cardWidth = std::min(440.0f, std::max(0.0f, viewportBody.z - 24.0f));
    const float cardHeight = canSave && cardWidth >= 340.0f ? 76.0f : 30.0f;
    const ImVec2 max(min.x + cardWidth, min.y + cardHeight);
    drawList->AddRectFilled(min, max,
                            ImGui::GetColorU32(theme::color(theme::tokens().headerBackground)),
                            theme::metric(theme::tokens().regionRounding));
    const ImVec2 oldCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f, min.y + 6.0f));
    ImGui::TextDisabled("%s", message);
    if (cancellable && cancelRequested) {
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::SmallButton("Cancel")) *cancelRequested = true;
    }
    if (canSave && cardWidth >= 340.0f) {
        ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f, min.y + 39.0f));
        ImGui::SetNextItemWidth(148.0f);
        ImGui::InputText("##viewport-preview-path", savePath, 260);
        ImGui::SameLine(0.0f, 4.0f);
        static constexpr const char* formatNames[] = {"PFM", "PPM"};
        ImGui::SetNextItemWidth(62.0f);
        if (ImGui::Combo("##viewport-preview-format", saveFormat, formatNames, 2)) {
            std::filesystem::path currentPath(savePath);
            const std::string extension = currentPath.extension().string();
            if (extension == ".pfm" || extension == ".ppm") {
                currentPath.replace_extension(*saveFormat == 1 ? ".ppm" : ".pfm");
                std::snprintf(savePath, 260, "%s", currentPath.string().c_str());
            }
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (saveState == ViewportPreviewSaveState::Running) {
            ImGui::BeginDisabled();
            ImGui::Button("Saving…");
            ImGui::EndDisabled();
        } else if (ImGui::Button("Save Viewport Preview")) {
            *saveRequested = true;
        }
        if (saveState == ViewportPreviewSaveState::Failed) {
            ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f, min.y + 58.0f));
            ImGui::TextDisabled("Could not save viewport preview.");
        } else if (saveState == ViewportPreviewSaveState::Completed) {
            ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f, min.y + 58.0f));
            ImGui::TextDisabled("Saved viewport preview: %s", savedPath.string().c_str());
        }
    }
    ImGui::SetCursorScreenPos(oldCursor);
    drawList->PopClipRect();
}

float clampedCompareSplit(const ember::ViewportSettings& viewport) {
    const float split = std::isfinite(viewport.compareSplit) ? viewport.compareSplit : 0.5f;
    return std::clamp(split, 0.05f, 0.95f);
}

bool compareViewportActive(const ember::ViewportSettings& viewport) {
    return viewport.shading == ShadingMode::Rendered && viewport.compareRasterCycles;
}

const char* cyclesCompareLabel(const CyclesViewportProgress& progress) {
    if (progress.hasPreview) return "Cycles CPU";
    if (progress.state == render::OfflineRenderState::Failed) return "Cycles CPU - failed";
    if (progress.state == render::OfflineRenderState::Cancelled) return "Cycles CPU - cancelled";
    return "Cycles CPU - warming up";
}

void updateCompareDividerInput(ember::ViewportSettings& viewport, bool& dragging,
                               const ImVec4& viewportBody,
                               const CyclesViewportProgress& progress) {
    if (!compareViewportActive(viewport) || viewportBody.z <= 1.0f || viewportBody.w <= 1.0f) {
        dragging = false;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float dividerX = viewportBody.x + viewportBody.z * clampedCompareSplit(viewport);
    const bool overBody = ImGui::IsMouseHoveringRect(
        ImVec2(viewportBody.x, viewportBody.y),
        ImVec2(viewportBody.x + viewportBody.z, viewportBody.y + viewportBody.w), false);
    const bool overDivider = overBody && std::fabs(io.MousePos.x - dividerX) <= 9.0f;
    if (!dragging && progress.visible &&
        (progress.settling || progress.state == render::OfflineRenderState::Running ||
         (progress.state == render::OfflineRenderState::Completed && progress.hasPreview))) {
        const ImVec2 cardMin(viewportBody.x + 12.0f, viewportBody.y + 12.0f);
        const float cardWidth = std::min(440.0f, std::max(0.0f, viewportBody.z - 24.0f));
        const float cardHeight = progress.state == render::OfflineRenderState::Completed &&
                                 progress.hasPreview && cardWidth >= 340.0f ? 88.0f : 30.0f;
        if (ImGui::IsMouseHoveringRect(cardMin,
                ImVec2(cardMin.x + cardWidth, cardMin.y + cardHeight), false)) return;
    }
    if (!dragging && overDivider && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        dragging = true;
    if (dragging) {
        if (io.MouseDown[ImGuiMouseButton_Left]) {
            viewport.compareSplit = std::clamp((io.MousePos.x - viewportBody.x) / viewportBody.z,
                                               0.05f, 0.95f);
        } else {
            dragging = false;
        }
    }
}

void drawCompareViewportOverlay(const ember::ViewportSettings& viewport,
                               const CyclesViewportProgress& progress,
                               const ImVec4& body) {
    if (!compareViewportActive(viewport) || body.z <= 1.0f || body.w <= 1.0f) return;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(body.x, body.y), ImVec2(body.x + body.z, body.y + body.w), true);
    const float splitX = body.x + body.z * clampedCompareSplit(viewport);
    const float top = body.y + 8.0f;
    auto label = [&](const char* text, ImVec2 pos) {
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 labelMax(pos.x + textSize.x + 12.0f, pos.y + textSize.y + 8.0f);
        drawList->AddRectFilled(pos, labelMax,
            ImGui::GetColorU32(theme::color(theme::tokens().headerBackground)),
            theme::metric(theme::tokens().widgetRounding));
        drawList->AddText(ImVec2(pos.x + 6.0f, pos.y + 4.0f),
            ImGui::GetColorU32(theme::color(theme::tokens().textPrimary)), text);
    };
    label("Raster", ImVec2(body.x + 10.0f, top));
    const char* rightLabel = cyclesCompareLabel(progress);
    const float rightWidth = ImGui::CalcTextSize(rightLabel).x + 12.0f;
    label(rightLabel, ImVec2(body.x + body.z - rightWidth - 10.0f, top));
    drawList->AddRectFilled(ImVec2(splitX - 3.0f, body.y), ImVec2(splitX + 3.0f, body.y + body.w),
        ImGui::GetColorU32(theme::color(theme::tokens().widgetButtonBackground)));
    drawList->AddLine(ImVec2(splitX, body.y), ImVec2(splitX, body.y + body.w),
        ImGui::GetColorU32(theme::color(theme::tokens().textDisabled)), 1.0f);
    drawList->PopClipRect();
}

// Blender default dark, as one theme for every panel, region and popup. Orange
// belongs to selection; active editor controls use blue, and hover stays neutral.
void applyTheme(const float* accent) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    const ember::ThemeTokens& t = theme::tokens();
    // GetFontSize() is zero until ImGui has run its first NewFrame (Editor::init
    // applies the theme before then). Use the configured font's base size when
    // available, otherwise D42's documented 13px baseline, and apply the same
    // global font scales that GetFontSize() would apply after NewFrame.
    float baseFontSize = style.FontSizeBase;
    if (baseFontSize <= 0.0f) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImFont* font = io.FontDefault;
        if (!font && io.Fonts && io.Fonts->Fonts.Size > 0) font = io.Fonts->Fonts[0];
        if (font && font->LegacySize > 0.0f) baseFontSize = font->LegacySize;
    }
    if (baseFontSize <= 0.0f) baseFontSize = 13.0f;
    const float fontSize = baseFontSize * style.FontScaleMain * style.FontScaleDpi;

    // Apply the shared logical metrics uniformly. Window and frame padding are
    // derived from the spacing/widget-height tokens, so header and widget sizing
    // remain tied to the same source instead of accumulating local literals.
    style.WindowRounding = theme::metric(t.regionRounding);
    style.ChildRounding = theme::metric(t.regionRounding);
    style.PopupRounding = theme::metric(t.regionRounding);
    style.ScrollbarRounding = theme::metric(t.regionRounding);
    style.FrameRounding = theme::metric(t.widgetRounding);
    style.GrabRounding = theme::metric(t.widgetRounding);
    style.TabRounding = theme::metric(t.regionRounding);
    style.WindowBorderSize = theme::metric(1.0f);
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(theme::metric(t.itemSpacing.x), theme::metric(t.itemSpacing.x));
    style.FramePadding = ImVec2(theme::metric(t.itemSpacing.x - t.itemInnerSpacing.x + 1.0f),
                                std::max(0.0f, (theme::metric(t.widgetHeight) - fontSize) * 0.5f));
    style.ItemSpacing = theme::metric(t.itemSpacing);
    style.ItemInnerSpacing = theme::metric(t.itemInnerSpacing);
    style.IndentSpacing = theme::metric(t.itemSpacing.x + t.itemInnerSpacing.x + 2.0f);
    style.ScrollbarSize = theme::metric(t.widgetHeight * (12.0f / 19.0f));
    style.GrabMinSize = theme::metric(t.widgetHeight * (8.0f / 19.0f));

    const ImVec4 region = theme::color(t.regionBackground, t.regionBackgroundAlpha);
    const ImVec4 header = theme::color(t.headerBackground);
    const ImVec4 field = theme::color(t.widgetFieldBackground);
    const ImVec4 button = theme::color(t.widgetButtonBackground);
    const ImVec4 text = theme::color(t.textPrimary);
    const ImVec4 textDisabled = theme::color(t.textDisabled);
    const ImVec4 selectionFill = theme::accent(accent, t.selectionFillAlpha);
    const ImVec4 active = theme::color(t.active);
    const ImVec4 activeFill = theme::activeFill();
    const ImVec4 hover = theme::hoverFill();

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]          = region;
    c[ImGuiCol_ChildBg]           = region;
    c[ImGuiCol_PopupBg]           = region;
    c[ImGuiCol_MenuBarBg]         = header;
    c[ImGuiCol_Border]            = field;
    c[ImGuiCol_BorderShadow]      = theme::color(t.viewportBodyBackground, t.viewportBodyAlpha);
    c[ImGuiCol_TitleBg]           = header;
    c[ImGuiCol_TitleBgActive]     = activeFill;
    c[ImGuiCol_TitleBgCollapsed]  = header;
    c[ImGuiCol_DockingEmptyBg]    = region;
    c[ImGuiCol_Separator]         = field;
    // Splitters use their dedicated editor chrome tokens, not selection orange
    // or the saturated D42 active-blue token. The active RGB is pixel-pinned by
    // item 141's GPU probe.
    c[ImGuiCol_SeparatorActive]   = theme::color(theme::splitterActive);
    c[ImGuiCol_ResizeGrip]        = theme::color(t.widgetButtonBackground, t.regionBackgroundAlpha * 0.4f);
    c[ImGuiCol_ResizeGripActive]  = theme::color(theme::splitterActive);
    c[ImGuiCol_SeparatorHovered]  = theme::color(theme::splitterHovered);
    c[ImGuiCol_ResizeGripHovered] = theme::color(theme::splitterHovered);
    c[ImGuiCol_Text]              = text;
    c[ImGuiCol_TextDisabled]      = textDisabled;
    c[ImGuiCol_TextSelectedBg]    = selectionFill;
    c[ImGuiCol_FrameBg]           = field;
    c[ImGuiCol_FrameBgHovered]    = hover;
    c[ImGuiCol_FrameBgActive]     = activeFill;
    c[ImGuiCol_Button]            = button;
    c[ImGuiCol_ButtonHovered]     = hover;
    c[ImGuiCol_ButtonActive]      = activeFill;
    c[ImGuiCol_CheckMark]         = active;
    c[ImGuiCol_SliderGrab]        = active;
    c[ImGuiCol_SliderGrabActive]  = active;
    c[ImGuiCol_Header]            = header;
    c[ImGuiCol_HeaderHovered]     = hover;
    c[ImGuiCol_HeaderActive]      = activeFill;
    c[ImGuiCol_Tab]               = region;
    c[ImGuiCol_TabHovered]        = hover;
    c[ImGuiCol_TabSelected]       = activeFill;
    c[ImGuiCol_TabDimmed]         = region;
    c[ImGuiCol_TabDimmedSelected] = activeFill;
    c[ImGuiCol_DockingPreview]    = activeFill;
    c[ImGuiCol_NavHighlight]      = active;
    c[ImGuiCol_NavWindowingHighlight] = activeFill;
    c[ImGuiCol_NavWindowingDimBg] = theme::color(t.regionBackground, 0.2f);
    c[ImGuiCol_ModalWindowDimBg]  = theme::color(t.regionBackground, 0.5f);
    c[ImGuiCol_DragDropTarget]    = active;
    c[ImGuiCol_PlotLines]         = textDisabled;
    c[ImGuiCol_PlotHistogram]     = active;
    c[ImGuiCol_ScrollbarBg]       = region;
    c[ImGuiCol_ScrollbarGrab]     = button;
    c[ImGuiCol_ScrollbarGrabHovered] = hover;
    c[ImGuiCol_ScrollbarGrabActive]  = activeFill;
}

template <std::size_t N>
float contentSizedComboWidth(const char* const (&options)[N]) {
    float labelWidth = 0.0f;
    for (const char* option : options)
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(option).x);

    // BeginCombo reserves a square arrow button whose width is the frame height.
    // Use the theme's widget height and the active theme style's horizontal
    // frame padding, so both DPI scale and theme spacing are reflected here.
    const float arrowWidth = theme::metric(theme::tokens().widgetHeight);
    const float framePadding = ImGui::GetStyle().FramePadding.x;
    return labelWidth + arrowWidth + 2.0f * framePadding;
}

void drawStats(const RenderStats& stats) {
    beginDockedPanel("Stats");
    ImGui::Text("Draw calls: %d", stats.drawCalls);
    ImGui::Text("Triangles:  %d", stats.triangles);
    // CPU-side renderFrame time only; it is not GPU-synced (see Renderer.h).
    ImGui::Text("CPU frame:  %.2f ms", stats.frameMs);
    ImGui::Text("UI:         %.0f fps", static_cast<double>(ImGui::GetIO().Framerate));
    endDockedPanel();
}

const char* areaTypeName(AreaType type) {
    switch (type) {
        case AreaType::Outliner: return ember::UiVocabulary::Outliner;
        case AreaType::Properties: return ember::UiVocabulary::Properties;
        case AreaType::Stats: return labels::Statistics;
        case AreaType::UndoHistory: return labels::UndoHistory;
        case AreaType::Timeline: return "Timeline";
        case AreaType::Viewport: return "3D Viewport";
    }
    return "Editor";
}

void drawAreaHeaderRight(AreaType type, scene::World& world, const RenderStats& stats,
                         const TimelineState& timeline) {
    if (type == AreaType::Outliner) {
        ImGui::TextDisabled("Objects %zu", world.objects().size());
    } else if (type == AreaType::Properties) {
        const auto& selection = world.selection();
        const scene::Object* active = selection.empty() ? nullptr : world.get(selection.back());
        ImGui::TextDisabled("%s", active ? active->name.c_str() : "No active object");
    } else if (type == AreaType::Stats) {
        ImGui::TextDisabled("Draw calls %d  Triangles %d", stats.drawCalls, stats.triangles);
    } else if (type == AreaType::UndoHistory) {
        ImGui::TextDisabled("%s", labels::UndoHistory);
    } else if (type == AreaType::Timeline) {
        ImGui::TextDisabled("Frame %d", timeline.current);
    }
}

// Item 141's repeatable UI proof: queue a real ImGui drag over the default
// viewport/right-column splitter. Events are deliberately appended after the
// GLFW backend's NewFrame, for the same last-event-wins reason as transform
// self-test input above.
int runSplitterSelfTestInput() {
    if (std::getenv("EMBER_SELFTEST_SPLITTER") == nullptr) return 0;
    static int frame = 0;
    ++frame;
    ImGuiIO& io = ImGui::GetIO();
    const float startX = io.DisplaySize.x * 0.78f - 1.0f;
    const float y = io.DisplaySize.y * 0.42f;
    float x = startX;
    if (frame >= 8) {
        const int step = std::min(7, (frame - 8) / 2 + 1);
        x -= static_cast<float>(step * 10);
    }
    io.AddMousePosEvent(x, y);
    if (frame >= 6) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    return frame;
}

void writeLe16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>(value >> 8)};
    out.write(bytes, sizeof(bytes));
}

void writeLe32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                           static_cast<char>((value >> 16) & 0xff), static_cast<char>(value >> 24)};
    out.write(bytes, sizeof(bytes));
}

// Dump the entire back buffer after ImGui has rendered. The app's normal
// EMBER_FRAME_DUMP is intentionally viewport-local, so this separate opt-in is
// needed to inspect a splitter outside the 3D area.
void dumpEditorFrame(const char* path) {
    if (path == nullptr || *path == '\0') return;
    const ImGuiIO& io = ImGui::GetIO();
    const int width = static_cast<int>(std::lround(io.DisplaySize.x * io.DisplayFramebufferScale.x));
    const int height = static_cast<int>(std::lround(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    if (width <= 0 || height <= 0) return;

    std::vector<u8> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    const std::uint32_t rowBytes = (static_cast<std::uint32_t>(width) * 3u + 3u) & ~3u;
    const std::uint32_t imageBytes = rowBytes * static_cast<std::uint32_t>(height);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        EMBER_LOG_WARN("editor frame dump: cannot open '%s'", path);
        return;
    }
    out.write("BM", 2);
    writeLe32(out, 54u + imageBytes);
    writeLe16(out, 0);
    writeLe16(out, 0);
    writeLe32(out, 54);
    writeLe32(out, 40);
    writeLe32(out, static_cast<std::uint32_t>(width));
    writeLe32(out, static_cast<std::uint32_t>(height));
    writeLe16(out, 1);
    writeLe16(out, 24);
    writeLe32(out, 0);
    writeLe32(out, imageBytes);
    writeLe32(out, 2835);
    writeLe32(out, 2835);
    writeLe32(out, 0);
    writeLe32(out, 0);

    const std::size_t rgbaStride = static_cast<std::size_t>(width) * 4u;
    const std::size_t padding = rowBytes - static_cast<std::uint32_t>(width) * 3u;
    const char zero[3] = {};
    for (int y = 0; y < height; ++y) { // GL rows are bottom-up; positive BMP height is bottom-up too
        const u8* row = rgba.data() + static_cast<std::size_t>(y) * rgbaStride;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 4u;
            const char bgr[3] = {static_cast<char>(row[i + 2]), static_cast<char>(row[i + 1]),
                                 static_cast<char>(row[i])};
            out.write(bgr, sizeof(bgr));
        }
        out.write(zero, static_cast<std::streamsize>(padding));
    }
    if (out) EMBER_LOG_INFO("editor frame dump: wrote '%s' (%dx%d BMP)", path, width, height);
}

void drawViewportHeaderRight(scene::World& world, const RenderStats& stats,
                             ember::ViewportSettings& viewport, TransformSpace& space,
                             InteractionMode interaction, SelectMode& selectMode, bool& snapEnabled,
                             SnapTarget& snapTarget) {
    static constexpr const char* shadingNames[] = {"Wireframe", "Solid", "Material Preview", "Rendered"};
    int shading = static_cast<int>(viewport.shading);
    ImGui::SetNextItemWidth(contentSizedComboWidth(shadingNames));
    if (ImGui::Combo("##shade", &shading, shadingNames, 4))
        viewport.shading = static_cast<ember::ShadingMode>(shading);

    ImGui::SameLine(0.0f, theme::metric(4.0f));
    if (ImGui::Button("Overlays")) ImGui::OpenPopup("##viewport-overlays");
    if (ImGui::BeginPopup("##viewport-overlays")) {
        ImGui::Checkbox("Grid", &viewport.showGrid);
        ImGui::Checkbox("Axes", &viewport.showAxes);
        ImGui::Checkbox("Origins", &viewport.showOrigins);
        ImGui::Checkbox("Selection Outline", &viewport.showSelectionOutline);
        ImGui::Separator();
        ImGui::Checkbox("Edit Vertices", &viewport.showEditVertices);
        ImGui::Checkbox("Edit Edges", &viewport.showEditEdges);
        ImGui::Checkbox("Edit Faces", &viewport.showEditFaces);
        ImGui::Checkbox("X-Ray", &viewport.xray);
        ImGui::EndPopup();
    }

    ImGui::SameLine(0.0f, theme::metric(4.0f));
    static constexpr const char* orientationNames[] = {"Global", "Local"};
    int orientation = space == TransformSpace::Global ? 0 : 1;
    ImGui::SetNextItemWidth(contentSizedComboWidth(orientationNames));
    if (ImGui::Combo("##orientation", &orientation, orientationNames, 2))
        space = orientation == 0 ? TransformSpace::Global : TransformSpace::Local;

    ImGui::SameLine(0.0f, theme::metric(4.0f));
    static constexpr const char* pivotNames[] = {"Median", "Cursor", "Individual", "Bounds", "Active"};
    static constexpr ember::PivotMode pivotValues[] = {ember::PivotMode::MedianPoint,
        ember::PivotMode::Cursor3D, ember::PivotMode::IndividualOrigins,
        ember::PivotMode::BoundingBoxCenter, ember::PivotMode::ActiveElement};
    int pivot = 0;
    for (int i = 0; i < 5; ++i) if (viewport.pivot == pivotValues[i]) pivot = i;
    ImGui::SetNextItemWidth(contentSizedComboWidth(pivotNames));
    if (ImGui::Combo("##pivot", &pivot, pivotNames, 5)) viewport.pivot = pivotValues[pivot];

    ImGui::SameLine(0.0f, theme::metric(4.0f));
    ImGui::Checkbox("Magnet", &snapEnabled);
    ImGui::SameLine(0.0f, theme::metric(4.0f));
    static constexpr const char* snapNames[] = {"Increment", "Vertex", "Edge", "Face"};
    int snap = static_cast<int>(snapTarget);
    ImGui::SetNextItemWidth(contentSizedComboWidth(snapNames));
    if (ImGui::BeginCombo("##snaptarget", snapNames[snap])) {
        theme::pushActiveItemColors();
        if (ImGui::Selectable("Increment", snap == 0)) snapTarget = SnapTarget::GridIncrement;
        for (int i = 1; i < 4; ++i) {
            ImGui::BeginDisabled();
            ImGui::Selectable(snapNames[i], false);
            ImGui::EndDisabled();
        }
        theme::popHeaderColors();
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, theme::metric(4.0f));
    ImGui::Checkbox("Gizmo", &viewport.showGizmo);
    ImGui::SameLine(0.0f, theme::metric(4.0f));
    if (interaction == InteractionMode::Edit) {
        static constexpr const char* selectNames[] = {"Vertex", "Edge", "Face"};
        int select = static_cast<int>(selectMode);
        ImGui::SetNextItemWidth(contentSizedComboWidth(selectNames));
        if (ImGui::Combo("##selectmode", &select, selectNames, 3))
            selectMode = static_cast<SelectMode>(select);
        const ObjectId active = world.selection().empty() ? ObjectId{} : world.selection().back();
        const scene::EditableMesh* edit = world.readableEdit(active);
        if (edit) ImGui::TextDisabled("V:%u E:%u F:%u", edit->vertexCount(),
                                      static_cast<u32>(edit->edges.size()), edit->faceCount());
    } else {
        ImGui::TextDisabled("%zu objects  %d tris", world.objects().size(), stats.triangles);
    }
}

// One area: a fixed header strip (type selector at left, that editor's controls
// at right) and the editor content below. The viewport area's body remains
// transparent so the renderer can draw into the central node underneath it.
ImVec4 drawArea(const char* name, AreaType& type, bool viewportHost,
                scene::World& world, UndoStack& undo, const RenderStats& stats,
                TimelineState& timeline, FileDialog& file, DocumentState& doc,
                const Vec3& addAt, ember::ViewportSettings& viewport,
                TransformSpace& space, render::OrbitCamera& camera, ToolState& tools,
                bool sidebarOpen, SidebarTab& sidebarTab,
                  InteractionMode interaction, SelectMode& selectMode,
                  PropertiesTab& propertiesTab,
                  bool& snapEnabled, SnapTarget& snapTarget,
                   ImVec4* areaBounds = nullptr,
                   const CyclesViewportProgress* cyclesProgress = nullptr,
                   bool* cyclesCancelRequested = nullptr,
                   char* previewSavePath = nullptr, int* previewSaveFormat = nullptr,
                   ViewportPreviewSaveState previewSaveState = ViewportPreviewSaveState::Idle,
                   std::filesystem::path savedPreviewPath = {},
                   bool* previewSaveRequested = nullptr) {
    static constexpr AreaType allTypes[] = {AreaType::Outliner, AreaType::Properties,
        AreaType::Stats, AreaType::UndoHistory, AreaType::Timeline, AreaType::Viewport};

    const ember::ThemeTokens& tokens = theme::tokens();
    const bool transparentViewport = viewportHost && type == AreaType::Viewport;
    if (transparentViewport) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
                              theme::color(tokens.viewportBodyBackground, tokens.viewportBodyAlpha));
        // Toolbar, viewport body, and Sidebar are child Regions that tile the
        // Area's body exactly, so the Area D host contributes no padding gaps.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar;
    beginDockedPanel(name, nullptr, flags);
    if (areaBounds) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        *areaBounds = ImVec4(pos.x, pos.y, size.x, size.y);
    }

    const ImVec2 hMin = ImGui::GetCursorScreenPos();
    const float headerHeight = theme::metric(tokens.headerHeight);
    const ImVec4 headerColor = theme::color(tokens.headerBackground);
    ImGui::GetWindowDrawList()->AddRectFilled(hMin,
        ImVec2(hMin.x + ImGui::GetWindowWidth(), hMin.y + headerHeight),
        ImGui::GetColorU32(headerColor));

    ImGui::SetNextItemWidth(theme::metric(112.0f));
    // Before the first NewFrame, the font size is not current yet and the
    // startup FramePadding fallback can make the Timeline selector taller than
    // its themed widget metric. Keep it inside the compact Area Header.
    const bool compactTimelineHeader = type == AreaType::Timeline;
    if (compactTimelineHeader) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float framePaddingY = std::max(0.0f,
            (theme::metric(tokens.widgetHeight) - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, framePaddingY));
    }
    // BeginCombo paints a FrameBg preview behind the editor-type label. That
    // field fill becomes an unexplained grey block in the header; the selector
    // already has an explicit arrow button, so keep the preview flush with its
    // Area Header surface and retain ImGui's normal hover/active feedback.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, headerColor);
    const bool editorTypeOpen = ImGui::BeginCombo("##area-editor", areaTypeName(type));
    ImGui::PopStyleColor();
    if (editorTypeOpen) {
        theme::pushActiveItemColors();
        for (AreaType candidate : allTypes) {
            if (!viewportHost && candidate == AreaType::Viewport) continue;
            const bool selected = type == candidate;
            if (ImGui::Selectable(areaTypeName(candidate), selected)) type = candidate;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        theme::popHeaderColors();
        ImGui::EndCombo();
    }
    if (compactTimelineHeader) ImGui::PopStyleVar();

    if (viewportHost && type == AreaType::Viewport) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", interaction == InteractionMode::Object
                                         ? ember::UiVocabulary::ObjectMode
                                         : labels::EditMode);
        ImGui::SameLine(0.0f, theme::metric(tokens.itemSpacing.x));
        drawViewportObjectMenus(world, undo, addAt);
        // Reserve enough width for every labeled Viewport control, including
        // the Edit Mode Vertex/Edge/Face combo. At 660px the final combo's hit
        // rect was clamped to 39px (27px shorter than its longest label).
        const float right = std::max(ImGui::GetCursorPosX() + theme::metric(tokens.itemSpacing.x),
                                     ImGui::GetWindowWidth() - theme::metric(700.0f));
        ImGui::SameLine(right);
        drawViewportHeaderRight(world, stats, viewport, space, interaction, selectMode,
                                snapEnabled, snapTarget);
    } else {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + theme::metric(tokens.itemSpacing.x),
                                 ImGui::GetWindowWidth() - theme::metric(160.0f)));
        drawAreaHeaderRight(type, world, stats, timeline);
    }
    ImGui::Separator();

    ImVec4 viewportBody{};
    if (viewportHost) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (type == AreaType::Viewport) {
            const float toolbarWidth = tools.panelOpen ? toolbarRegionWidth() : 0.0f;
            const float sidebarWidth = sidebarOpen ? sidebarRegionWidth() : 0.0f;
            const float viewWidth = std::max(0.0f, available.x - toolbarWidth - sidebarWidth);
            viewportBody = ImVec4(p.x + toolbarWidth, p.y, viewWidth, std::max(0.0f, available.y));

            if (tools.panelOpen && toolbarWidth > 0.0f)
                drawToolbar(tools, ImVec4(p.x, p.y, toolbarWidth, available.y));
            if (sidebarOpen && sidebarWidth > 0.0f)
                drawSidebar(world, undo, true,
                            ImVec4(p.x + toolbarWidth + viewWidth, p.y, sidebarWidth, available.y),
                            sidebarTab, viewport, space, tools, snapEnabled, snapTarget,
                            interaction, selectMode, camera);

            // The renderer occupies the middle Region; Toolbar and Sidebar
            // children abut it without overlaying or leaving a pixel gap.
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + available.y));
        } else {
            viewportBody = ImVec4(p.x, p.y, std::max(0.0f, available.x),
                                  std::max(0.0f, available.y));
        }
    }

    if (!(viewportHost && type == AreaType::Viewport)) {
        setEmbeddedArea(true);
        bool alwaysOpen = true;
        switch (type) {
            case AreaType::Outliner: drawOutliner(world, undo); break;
            case AreaType::Properties:
                drawProperties(world, undo, propertiesTab, viewport, file, doc, timeline,
                               interaction);
                break;
            case AreaType::Stats: drawStats(stats); break;
            case AreaType::UndoHistory: drawHistory(undo, alwaysOpen); break;
            case AreaType::Timeline: drawTimeline(timeline); break;
            case AreaType::Viewport:
                ImGui::TextDisabled("3D Viewport is hosted in the central %s.", ember::UiVocabulary::Area);
                break;
        }
        setEmbeddedArea(false);
    }
    if (viewportHost && type == AreaType::Viewport && cyclesProgress) {
        drawCyclesViewportStatus(*cyclesProgress, cyclesCancelRequested, viewportBody,
                                 previewSavePath, previewSaveFormat, previewSaveState,
                                 savedPreviewPath, previewSaveRequested);
        drawCompareViewportOverlay(viewport, *cyclesProgress, viewportBody);
    }
    endDockedPanel();
    if (transparentViewport) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    return viewportBody;
}

// Keep the pre-region entry point for editor-local headless probes that include
// this translation unit directly. The live Editor::draw path passes Toolbar and
// Sidebar state to the Region-aware overload above.
ImVec4 drawArea(const char* name, AreaType& type, bool viewportHost,
                scene::World& world, UndoStack& undo, const RenderStats& stats,
                TimelineState& timeline, const Vec3& addAt, ember::ViewportSettings& viewport,
                TransformSpace& space, InteractionMode interaction, SelectMode& selectMode,
                PropertiesTab& propertiesTab, bool& snapEnabled, SnapTarget& snapTarget) {
    ToolState tools;
    tools.panelOpen = false;
    SidebarTab sidebarTab = SidebarTab::Item;
    static render::OrbitCamera camera;
    static FileDialog file;
    static DocumentState doc;
    return drawArea(name, type, viewportHost, world, undo, stats, timeline, file, doc, addAt, viewport,
                    space, camera, tools, false, sidebarTab, interaction, selectMode, propertiesTab,
                    snapEnabled, snapTarget);
}

// Compatibility for editor-local layout probes that pass the region state but
// do not own Renderer::camera(). The live Editor path supplies its real camera.
ImVec4 drawArea(const char* name, AreaType& type, bool viewportHost,
                scene::World& world, UndoStack& undo, const RenderStats& stats,
                TimelineState& timeline, const Vec3& addAt, ember::ViewportSettings& viewport,
                TransformSpace& space, ToolState& tools, bool sidebarOpen, SidebarTab& sidebarTab,
                InteractionMode interaction, SelectMode& selectMode, PropertiesTab& propertiesTab,
                bool& snapEnabled, SnapTarget& snapTarget) {
    static render::OrbitCamera camera;
    static FileDialog file;
    static DocumentState doc;
    return drawArea(name, type, viewportHost, world, undo, stats, timeline, file, doc, addAt, viewport,
                    space, camera, tools, sidebarOpen, sidebarTab, interaction, selectMode,
                    propertiesTab, snapEnabled, snapTarget);
}

// Repeatable Z-pie input proof. Events are submitted after the GLFW backend's
// NewFrame so the script's final state is authoritative, like the transform
// self-test above. Directions match Blender's pie slot order.
int queuePieSelfTestInput() {
    if (std::getenv("EMBER_SELFTEST_PIE") == nullptr) return 0;
    static int frame = 0;
    ++frame;
    ImGuiIO& io = ImGui::GetIO();
    const float cx = io.DisplaySize.x * 0.5f;
    const float cy = io.DisplaySize.y * 0.5f;
    ImVec2 mouse(cx, cy);
    if (frame == 2 || frame == 3) mouse.x += theme::metric(100.0f);       // right: Solid
    if (frame == 13) mouse.y -= theme::metric(100.0f);                   // up: Rendered
    if (frame == 17 || frame == 18) mouse.y += theme::metric(100.0f);    // down: Material
    io.AddMousePosEvent(mouse.x, mouse.y);

    const bool zDown = frame == 1 || frame == 2 || frame == 5 || frame == 11 ||
                       frame == 16 || frame == 17 || frame == 20 || frame == 25;
    io.AddKeyEvent(ImGuiKey_Z, zDown);
    io.AddKeyEvent(ImGuiKey_LeftArrow, frame == 7);
    io.AddKeyEvent(ImGuiKey_Enter, frame == 8);
    io.AddKeyEvent(ImGuiKey_Escape, frame == 22);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, frame == 13);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, frame == 27);
    return frame;
}

} // namespace

Editor::~Editor() { shutdown(); }

bool Editor::init(void* nativeWindow) {
    if (initialized_) return true;
    if (!nativeWindow) {
        EMBER_LOG_ERROR("editor: init called with a null window handle");
        return false;
    }

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        EMBER_LOG_ERROR("editor: ImGui::CreateContext failed");
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    prefs_ = loadPreferences();
    applyTheme(prefs_.accent);

    if (!ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(nativeWindow), true)) {
        EMBER_LOG_ERROR("editor: ImGui_ImplGlfw_InitForOpenGL failed");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 450")) {
        EMBER_LOG_ERROR("editor: ImGui_ImplOpenGL3_Init failed");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    EMBER_LOG_INFO("editor: ImGui %s initialised (GLFW + OpenGL3)", IMGUI_VERSION);
    return true;
}

void Editor::prepareViewportRender(const RenderPacket& packet, render::Renderer& renderer,
                                   double nowSeconds, bool viewportAreaAvailable) {
    cyclesRenderer_ = &renderer;
    const ViewportSettings& viewportSettings = renderer.viewportSettings();
    const bool compareRequested = compareViewportActive(viewportSettings);
    const bool shouldRenderCycles = viewportAreaAvailable && packet.width > 1 && packet.height > 1 &&
        viewportSettings.shading == ShadingMode::Rendered &&
        (packet.renderSettings.engine == RenderEngine::Cycles || compareRequested);

    auto retireJob = [&]() {
        const render::OfflineRenderState state = cyclesJob_.state();
        if (state == render::OfflineRenderState::Running)
            cyclesJob_.requestCancel(); // signal promptly; reap after the batch ends
        else if (state != render::OfflineRenderState::Idle)
            cyclesJob_.reset();
        cyclesPreview_.reset();
        renderer.setPathTraceImage(nullptr);
    };

    if (!shouldRenderCycles) {
        retireJob();
        cyclesHasPacketKey_ = false;
        cyclesWaitingForSettle_ = false;
        cyclesCancelledByUser_ = false;
        cyclesCancelRequested_ = false;
        cyclesProgress_ = {};
        cyclesProgress_.generation = cyclesGeneration_;
        return;
    }

    cyclesProgress_.visible = true;
    cyclesProgress_.targetSamples = packet.renderSettings.samplesPerPixel;
    if (cyclesCancelRequested_) {
        cyclesCancelRequested_ = false;
        if (cyclesJob_.state() == render::OfflineRenderState::Running ||
            cyclesWaitingForSettle_) {
            if (cyclesJob_.state() == render::OfflineRenderState::Running)
                cyclesJob_.requestCancel();
            cyclesCancelledByUser_ = true;
            cyclesWaitingForSettle_ = false;
            cyclesPreview_.reset();
        }
    }

    const std::uint64_t currentKey = packetContentKey(packet);
    if (!cyclesHasPacketKey_ || currentKey != cyclesPacketKey_) {
        if (cyclesHasPacketKey_) retireJob();
        cyclesPacketKey_ = currentKey;
        cyclesHasPacketKey_ = true;
        cyclesLastChangeSeconds_ = nowSeconds;
        cyclesWaitingForSettle_ = true;
        cyclesCancelledByUser_ = false;
    }

    if (cyclesWaitingForSettle_ && !cyclesCancelledByUser_ &&
        nowSeconds - cyclesLastChangeSeconds_ >= kCyclesSettleSeconds) {
        const render::OfflineRenderState priorState = cyclesJob_.state();
        if (priorState == render::OfflineRenderState::Running) {
            // An invalidated worker has been signalled but must finish its
            // current batch before reset/start can reuse the job object.
            cyclesJob_.requestCancel();
        } else {
            if (priorState != render::OfflineRenderState::Idle)
                cyclesJob_.reset();
            render::OfflineRenderSettings settings;
            settings.width = packet.width;
            settings.height = packet.height;
            settings.targetSamplesPerPixel = packet.renderSettings.samplesPerPixel;
            settings.batchSamples = 1; // bound cancellation latency and publish frequent previews
            settings.pathTrace.maxBounces = packet.renderSettings.maxBounces;
            settings.pathTrace.seed = packet.renderSettings.seed;
            if (cyclesJob_.start(packet, settings)) {
                ++cyclesGeneration_;
                cyclesWaitingForSettle_ = false;
            } else if (cyclesJob_.state() == render::OfflineRenderState::Failed) {
                cyclesWaitingForSettle_ = false;
            }
        }
    }

    const render::OfflineRenderState jobState = cyclesJob_.state();
    cyclesProgress_.state = cyclesCancelledByUser_ ? render::OfflineRenderState::Cancelled : jobState;
    cyclesProgress_.workerRunning = jobState == render::OfflineRenderState::Running;
    cyclesProgress_.samplesDone = cyclesJob_.samplesDone();
    cyclesProgress_.settling = cyclesWaitingForSettle_;
    if (!cyclesCancelledByUser_ && !cyclesWaitingForSettle_ &&
        (jobState == render::OfflineRenderState::Running ||
         jobState == render::OfflineRenderState::Completed)) {
        const bool previewDue = !cyclesPreview_ || jobState == render::OfflineRenderState::Completed ||
                                nowSeconds - cyclesLastPreviewSeconds_ >= 0.1;
        if (previewDue &&
            (!cyclesPreview_ || cyclesPreview_->samplesPerPixel != cyclesProgress_.samplesDone)) {
            std::optional<render::PathTraceImage> updated = cyclesJob_.previewImage();
            if (updated) {
                cyclesPreview_ = std::make_shared<render::PathTraceImage>(std::move(*updated));
                cyclesLastPreviewSeconds_ = nowSeconds;
            }
        }
    }
    cyclesProgress_.hasPreview = static_cast<bool>(cyclesPreview_);
    cyclesProgress_.generation = cyclesGeneration_;
    renderer.setPathTraceImage(cyclesPreview_.get());
}

bool Editor::saveCompletedViewportPreview(const std::filesystem::path& path,
                                         ViewportPreviewFormat format) {
    if (path.empty() || cyclesProgress_.state != render::OfflineRenderState::Completed ||
        !cyclesPreview_) return false;
    if (previewSaveState_.load(std::memory_order_acquire) == ViewportPreviewSaveState::Running)
        return false;
    if (previewSaveWorker_.joinable()) previewSaveWorker_.join();

    const std::shared_ptr<const render::PathTraceImage> image = cyclesPreview_;
    previewSavePathResult_ = path;
    previewSaveState_.store(ViewportPreviewSaveState::Running, std::memory_order_release);
    try {
        previewSaveWorker_ = std::thread([this, image, path, format]() {
            bool written = false;
            try {
                if (format == ViewportPreviewFormat::PFM)
                    written = render::writePathImagePfm(path, *image);
                else if (format == ViewportPreviewFormat::PPM)
                    written = render::writePathImagePpm(path, *image);
            } catch (...) {
                written = false;
            }
            previewSaveState_.store(written ? ViewportPreviewSaveState::Completed
                                            : ViewportPreviewSaveState::Failed,
                                    std::memory_order_release);
        });
    } catch (...) {
        previewSaveState_.store(ViewportPreviewSaveState::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

ViewportPreviewSaveProgress Editor::viewportPreviewSaveProgress() const {
    return {previewSaveState_.load(std::memory_order_acquire), previewSavePathResult_};
}

void Editor::shutdown() {
    if (cyclesRenderer_) cyclesRenderer_->setPathTraceImage(nullptr);
    if (cyclesJob_.state() == render::OfflineRenderState::Running) cyclesJob_.cancel();
    if (cyclesJob_.state() != render::OfflineRenderState::Idle) cyclesJob_.reset();
    if (previewSaveWorker_.joinable()) previewSaveWorker_.join();
    cyclesPreview_.reset();
    cyclesProgress_ = {};
    cyclesProgress_.generation = cyclesGeneration_;
    cyclesHasPacketKey_ = false;
    cyclesWaitingForSettle_ = false;
    cyclesCancelledByUser_ = false;
    cyclesCancelRequested_ = false;
    cyclesRenderer_ = nullptr;
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

EditOverlayState Editor::editOverlayState(const scene::World& world) const {
    if (interactionMode_ != InteractionMode::Edit || world.selection().size() != 1)
        return {};
    const float scale = ImGui::GetCurrentContext()
        ? std::max(ImGui::GetIO().DisplayFramebufferScale.x,
                   ImGui::GetIO().DisplayFramebufferScale.y)
        : 1.0f;
    return buildEditOverlayState(world, world.selection().back(), selectMode_, scale);
}

bool handleEditSelectModeKey(const ImGuiIO& io, InteractionMode mode,
                             bool emulateNumpad, SelectMode& selectMode) {
    if (mode != InteractionMode::Edit || emulateNumpad || io.WantTextInput ||
        io.KeyCtrl || io.KeyAlt || io.KeyShift) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        selectMode = SelectMode::Vertex;
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2)) {
        selectMode = SelectMode::Edge;
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        selectMode = SelectMode::Face;
        return true;
    }
    return false;
}

void Editor::draw(scene::World& world, render::Renderer& renderer, const RenderStats& stats,
                  UndoStack& undo) {
    if (!initialized_) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    const int splitterSelfTestFrame = runSplitterSelfTestInput();
    const int pieSelfTestFrame = queuePieSelfTestInput();

    // Headless modal-transform proof (item 100): the script queues its input AFTER
    // the GLFW backend's NewFrame, because the backend re-posts the physical cursor
    // position every frame and the LAST event wins. Script-last makes the scripted
    // input authoritative; everything still flows through ImGui's real event queue.
    if (std::getenv("EMBER_SELFTEST_TRANSFORM") != nullptr) {
        static int selfTestFrame = 0;
        const int st = runTransformSelfTest(world, undo, renderer.camera(), interactionMode_,
                                            selectMode_, viewport_.pivot, ++selfTestFrame);
        if (st != 0) selfTestStatus_ = st;
    }

    ImGui::NewFrame();
    updateCompareDividerInput(viewport_, compareDividerDragging_, viewportRegion_, cyclesProgress_);

    // The pie owns input from its Z trigger through confirm/cancel. While a modal
    // transform is already active, its Escape/right-click precedence remains first.
    if (pieSelfTestFrame == 1) viewport_.shading = ember::ShadingMode::Wireframe;
    const bool transformAlreadyActive = modal_.active() || gizmo_.dragModal().active();
    bool pieBusy = false;
    if (!transformAlreadyActive) {
        const ObjectId active = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
        const scene::Object* object = world.get(active);
        if (interactionMode_ == InteractionMode::Edit &&
            (!object || !object->hasMesh || !world.editable(active))) {
            interactionMode_ = InteractionMode::Object;
        }
        const bool canEdit = object && object->hasMesh && world.editable(active);
        const bool modePieBusy = (modePie_.open || wantsViewportFocus()) &&
                                 handleModePie(interactionMode_, canEdit, modePie_);
        const bool shadingPieBusy = !modePieBusy && handleShadingPie(viewport_, pie_);
        pieBusy = modePieBusy || shadingPieBusy;
    }
    static int pieChecks = 0;
    static int pieFailures = 0;
    if (pieSelfTestFrame == 1) pieChecks = pieFailures = 0;
    auto checkPie = [&](bool ok, const char* name) {
        ++pieChecks;
        if (!ok) ++pieFailures;
        EMBER_LOG_INFO("[pie-selftest] %s %s", ok ? "PASS" : "FAIL", name);
    };
    switch (pieSelfTestFrame) {
        case 2:
            checkPie(pie_.open && pie_.hoveredSlice == 1 && viewport_.shading == ember::ShadingMode::Wireframe,
                     "hover right highlights Solid without confirming while Z is held");
            break;
        case 4:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Solid,
                     "Z release confirms the hovered Solid slice");
            break;
        case 7:
            checkPie(pie_.open && pie_.keyboardSlice == 0,
                     "left arrow selects Wireframe without mouse movement");
            break;
        case 10:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Wireframe,
                     "Enter confirms the keyboard-selected Wireframe slice");
            break;
        case 13:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Rendered,
                     "left click confirms the hovered Rendered slice");
            break;
        case 19:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Material,
                     "Z release confirms the hovered Material Preview slice");
            break;
        case 24:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Material,
                     "Escape cancels without changing the shading mode");
            break;
        case 29:
            checkPie(!pie_.open && viewport_.shading == ember::ShadingMode::Material,
                     "right click cancels without changing the shading mode");
            break;
        case 30:
            EMBER_LOG_INFO("[pie-selftest] %s: %d/%d checks passed",
                           pieFailures == 0 ? "PASS" : "FAIL", pieChecks - pieFailures, pieChecks);
            selfTestStatus_ = pieFailures == 0 && pieChecks == 8 ? 1 : -1;
            break;
        default: break;
    }

    // New objects appear at the orbit target, so the camera is the placement point.
    const Vec3 addAt = renderer.camera().target();
    // Precedence: a running modal transform owns the input (Esc and right click cancel
    // it), then the shading pie, shortcuts/tool keys, the gizmo, and picking/menus.
    const bool modalBusy = !pieBusy && modal_.update(world, undo, renderer.camera(), transformSpace_,
                                                      interactionMode_, selectMode_, snapEnabled_, viewport_.pivot);
    const ImGuiIO& io = ImGui::GetIO();
    if (!pieBusy && !modalBusy && interactionMode_ == InteractionMode::Object) handleShortcuts(world, undo);
    if (!pieBusy && !modalBusy && interactionMode_ == InteractionMode::Edit &&
        !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A)) {
        const ObjectId active = world.selection().empty() ? ObjectId{} : world.selection().back();
        const scene::EditableMesh* mesh = world.readableEdit(active);
        if (mesh) {
            std::vector<u32> ids;
            if (!io.KeyAlt) {
                const std::size_t count = selectMode_ == SelectMode::Vertex ? mesh->positions.size()
                                         : selectMode_ == SelectMode::Edge ? mesh->edges.size()
                                                                           : mesh->faces.size();
                ids.reserve(count);
                for (u32 i = 0; i < count; ++i) ids.push_back(i);
            }
            world.editSetSelection(active, selectMode_, ids);
        }
    }
    if (!pieBusy && !modalBusy && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_N)) {
        sidebarOpen_ = !sidebarOpen_;
    }
    if (!pieBusy && !modalBusy && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        if (interactionMode_ == InteractionMode::Object) {
            const ObjectId active = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
            const scene::Object* object = world.get(active);
            if (object && object->hasMesh && world.editable(active)) {
                interactionMode_ = InteractionMode::Edit;
                sidebarTab_ = SidebarTab::Tool;
            }
        } else {
            interactionMode_ = InteractionMode::Object;
            sidebarTab_ = SidebarTab::Item;
        }
    }
    if (!pieBusy && !modalBusy && interactionMode_ == InteractionMode::Edit &&
        wantsViewportFocus()) {
        const ObjectId active = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
        (void)handleEditMeshShortcuts(io, world, undo, active, selectMode_);
    }
    if (!pieBusy && !modalBusy && wantsViewportFocus())
        handleEditSelectModeKey(io, interactionMode_, prefs_.emulateNumpad, selectMode_);
    if (!pieBusy && !modalBusy) handleToolKeys(tools_);
    // The active tool picks the gizmo type. Select shows no gizmo and box-selects.
    switch (tools_.active) {
        case Tool::Move:   gizmo_.setMode(GizmoState::Mode::Translate); break;
        case Tool::Rotate: gizmo_.setMode(GizmoState::Mode::Rotate); break;
        case Tool::Scale:  gizmo_.setMode(GizmoState::Mode::Scale); break;
        case Tool::Select: break;
    }
    const bool transformTool = tools_.active != Tool::Select;
    const bool gizmoBusy = pieBusy || modalBusy || gizmo_.update(world, undo, renderer.camera(), transformSpace_,
                                                        viewport_.showGizmo && transformTool,
                                                        snapEnabled_, viewport_.pivot,
                                                        interactionMode_, selectMode_);
    const bool viewportOwnsPointer = wantsViewportFocus();
    const Rect viewportRectFb = viewportRect();
    // Keep the editor's input ownership gate separate from the render rect:
    // ownsPointer excludes headers, Toolbar/Sidebar and popups in window
    // points; the valid framebuffer rect supplies viewport-local camera rays.
    // A collapsed/non-viewport Area must not reuse the previous frame's hit box.
    if (!gizmoBusy && viewportRectFb.valid()) {
        if (interactionMode_ == InteractionMode::Edit) {
            handleViewportEditPick(world, renderer.camera(), selectMode_, viewportOwnsPointer,
                                   viewportRectFb, viewport_.xray);
        } else {
            handleViewportPick(world, renderer.camera(), undo, tools_.active,
                               viewportOwnsPointer, viewportRectFb);
        }
    }
    if (!pieBusy && !modalBusy && !gizmoBusy && interactionMode_ == InteractionMode::Object)
        handleViewportContext(world, undo, renderer.camera(), viewportOwnsPointer,
                              viewportRectFb);
    drawMainMenu(world, undo, file_, doc_, workspace_, sidebarOpen_, tools_.panelOpen,
                 historyOpen_, prefsOpen_, interactionMode_, selectMode_);
    if (workspace_ != appliedWorkspace_) {
        // Small deterministic editor arrangements. The viewport remains Area D;
        // workspace tabs change the right/bottom editor assignments.
        switch (workspace_) {
            case WorkspacePreset::Layout:
                areas_[0] = AreaType::Outliner;
                areas_[1] = AreaType::Properties;
                areas_[2] = AreaType::Timeline;
                break;
            case WorkspacePreset::Modeling:
                areas_[0] = AreaType::Outliner;
                areas_[1] = AreaType::Properties;
                areas_[2] = AreaType::Stats;
                break;
            case WorkspacePreset::Animation:
                areas_[0] = AreaType::Outliner;
                areas_[1] = AreaType::Timeline;
                areas_[2] = AreaType::Properties;
                break;
        }
        appliedWorkspace_ = workspace_;
    }
    if (historyOpen_) {  // Edit > Undo History shows the history in Area B
        areas_[1] = AreaType::UndoHistory;
        historyOpen_ = false;
    }
    drawWorkspace(layoutBuilt_);
    drawStatusBar(tools_, gizmo_, transformSpace_, modal_, snapEnabled_);
    drawArea(labels::DockAreaA, areas_[0], false, world, undo, stats, timeline_, file_, doc_, addAt, viewport_, transformSpace_, renderer.camera(),
             tools_, sidebarOpen_, sidebarTab_,
             interactionMode_, selectMode_, propertiesTab_, snapEnabled_, snapTarget_);
    drawArea(labels::DockAreaB, areas_[1], false, world, undo, stats, timeline_, file_, doc_, addAt, viewport_, transformSpace_, renderer.camera(),
             tools_, sidebarOpen_, sidebarTab_,
             interactionMode_, selectMode_, propertiesTab_, snapEnabled_, snapTarget_);
    drawArea(labels::DockAreaC, areas_[2], false, world, undo, stats, timeline_, file_, doc_, addAt, viewport_, transformSpace_, renderer.camera(),
             tools_, sidebarOpen_, sidebarTab_,
             interactionMode_, selectMode_, propertiesTab_, snapEnabled_, snapTarget_);
    bool previewSaveRequested = false;
    const ImVec4 viewportBody = drawArea(labels::DockAreaD, areas_[3], true, world, undo, stats, timeline_, file_, doc_, addAt,
                                           viewport_, transformSpace_, renderer.camera(), tools_, sidebarOpen_, sidebarTab_,
                                            interactionMode_, selectMode_,
                                            propertiesTab_, snapEnabled_, snapTarget_,
                                            &viewportAreaRegion_, &cyclesProgress_,
                                            &cyclesCancelRequested_, previewSavePath_, &previewSaveFormat_,
                                            previewSaveState_.load(std::memory_order_acquire),
                                            previewSavePathResult_, &previewSaveRequested);
    if (areas_[3] == AreaType::Viewport && viewportBody.z > 1.0f && viewportBody.w > 1.0f) {
        viewportRegion_ = viewportBody;
    } else {
        // Keep a valid, effectively hidden renderer area while the central editor
        // type is not the 3D Viewport. The selected editor fills the area window.
        viewportRegion_ = ImVec4(viewportBody.x, viewportBody.y, 1.0f, 1.0f);
    }
    if (previewSaveRequested) {
        const ViewportPreviewFormat format = previewSaveFormat_ == 1
            ? ViewportPreviewFormat::PPM : ViewportPreviewFormat::PFM;
        if (!saveCompletedViewportPreview(std::filesystem::path(previewSavePath_), format))
            EMBER_LOG_WARN("viewport preview: save could not be started");
    }
    renderer.setViewportSettings(viewport_);
    drawFileDialog(world, undo, file_, doc_);

    if (!io.WantTextInput && io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_Comma)) {
        prefsOpen_ = !prefsOpen_;
    }
    bool prefsChanged = false;
    drawPreferences(prefs_, prefsOpen_, prefsChanged);
    if (prefsChanged) {
        applyTheme(prefs_.accent);
        if (!savePreferences(prefs_)) EMBER_LOG_WARN("preferences: could not write '%s'", preferencesPath().c_str());
    }
    gizmo_.setHandleLength(prefs_.gizmoSize);
    if (!pieBusy) drawCommandPalette(world, undo, addAt, file_, doc_, viewport_, transformSpace_,
                                     sidebarOpen_, palette_);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (splitterSelfTestFrame == 22) dumpEditorFrame(std::getenv("EMBER_EDITOR_FRAME_DUMP"));
}

Rect Editor::viewportRect() const {
    if (!initialized_ || viewportRegion_.z <= 0.0f || viewportRegion_.w <= 0.0f) return Rect{};
    const ImGuiIO& io = ImGui::GetIO();
    Rect rect;
    rect.x = static_cast<int>(std::lround(viewportRegion_.x * io.DisplayFramebufferScale.x));
    rect.y = static_cast<int>(std::lround(viewportRegion_.y * io.DisplayFramebufferScale.y));
    rect.w = static_cast<int>(std::lround(viewportRegion_.z * io.DisplayFramebufferScale.x));
    rect.h = static_cast<int>(std::lround(viewportRegion_.w * io.DisplayFramebufferScale.y));
    return rect;
}

Rect Editor::viewportAreaRect() const {
    if (!initialized_ || viewportAreaRegion_.z <= 0.0f || viewportAreaRegion_.w <= 0.0f)
        return Rect{};
    const ImGuiIO& io = ImGui::GetIO();
    Rect rect;
    rect.x = static_cast<int>(std::lround(viewportAreaRegion_.x * io.DisplayFramebufferScale.x));
    rect.y = static_cast<int>(std::lround(viewportAreaRegion_.y * io.DisplayFramebufferScale.y));
    rect.w = static_cast<int>(std::lround(viewportAreaRegion_.z * io.DisplayFramebufferScale.x));
    rect.h = static_cast<int>(std::lround(viewportAreaRegion_.w * io.DisplayFramebufferScale.y));
    return rect;
}

bool Editor::wantsViewportFocus() const {
    if (!initialized_) return false;
    if (viewportRegion_.z <= 1.0f || viewportRegion_.w <= 1.0f) return false;

    // The viewport is itself a docked ImGui window, so WantCaptureMouse is TRUE over it as well.
    // Gating on `!WantCaptureMouse` — which this did — therefore meant the camera could never
    // move at all: no orbit, no pan, no wheel zoom, no numpad views, anywhere in the window.
    // What the camera actually needs is:
    //   * the pointer inside the viewport BODY (not the Outliner/Properties/Timeline), and
    //   * no popup or modal owning the mouse.
    // clip=false because this is read before ImGui::NewFrame() (App polls input ahead of the
    // editor's frame), where there is no current window to clip against.
    const ImVec2 min(viewportRegion_.x, viewportRegion_.y);
    const ImVec2 max(viewportRegion_.x + viewportRegion_.z, viewportRegion_.y + viewportRegion_.w);
    if (!ImGui::IsMouseHoveringRect(min, max, false)) return false;
    if (cyclesProgress_.visible &&
        (cyclesProgress_.settling || cyclesProgress_.state == render::OfflineRenderState::Running ||
         (cyclesProgress_.state == render::OfflineRenderState::Completed && cyclesProgress_.hasPreview))) {
        // The status/save card is viewport chrome, not a scene click/orbit target.
        const ImVec2 statusMin(viewportRegion_.x + 12.0f, viewportRegion_.y + 12.0f);
        const float statusWidth = std::min(440.0f, std::max(0.0f, viewportRegion_.z - 24.0f));
        const float statusHeight = cyclesProgress_.state == render::OfflineRenderState::Completed &&
                                   cyclesProgress_.hasPreview && statusWidth >= 340.0f ? 88.0f : 30.0f;
        const ImVec2 statusMax(statusMin.x + statusWidth, statusMin.y + statusHeight);
        if (ImGui::IsMouseHoveringRect(statusMin, statusMax, false)) return false;
    }
    if (compareViewportActive(viewport_)) {
        if (compareDividerDragging_) return false;
        const float dividerX = viewportRegion_.x + viewportRegion_.z * clampedCompareSplit(viewport_);
        const ImVec2 dividerMin(dividerX - 9.0f, viewportRegion_.y);
        const ImVec2 dividerMax(dividerX + 9.0f, viewportRegion_.y + viewportRegion_.w);
        if (ImGui::IsMouseHoveringRect(dividerMin, dividerMax, false)) return false;
        const char* rightLabel = cyclesCompareLabel(cyclesProgress_);
        const float labelTop = viewportRegion_.y + 8.0f;
        const ImVec2 rasterMin(viewportRegion_.x + 10.0f, labelTop);
        const ImVec2 rasterMax(rasterMin.x + ImGui::CalcTextSize("Raster").x + 12.0f,
                               labelTop + ImGui::GetTextLineHeight() + 8.0f);
        const float rightWidth = ImGui::CalcTextSize(rightLabel).x + 12.0f;
        const ImVec2 cyclesMin(viewportRegion_.x + viewportRegion_.z - rightWidth - 10.0f,
                               labelTop);
        const ImVec2 cyclesMax(cyclesMin.x + rightWidth,
                               labelTop + ImGui::GetTextLineHeight() + 8.0f);
        if (ImGui::IsMouseHoveringRect(rasterMin, rasterMax, false) ||
            ImGui::IsMouseHoveringRect(cyclesMin, cyclesMax, false)) return false;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
    return true;
}

} // namespace ember::editor

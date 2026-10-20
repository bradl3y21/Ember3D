#include "editor/Icons.h"

#include <algorithm>
#include <array>

namespace ember::editor {

namespace {

constexpr float kGrid = 16.0f;

struct Canvas {
    ImDrawList& drawList;
    ImU32 color;
    ImVec2 origin;
    float scale;

    ImVec2 point(float x, float y) const {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    }

    void line(float x0, float y0, float x1, float y1, float width = 1.4f) const {
        drawList.AddLine(point(x0, y0), point(x1, y1), color, width * scale);
    }

    void rect(float x0, float y0, float x1, float y1, float width = 1.4f) const {
        drawList.AddRect(point(x0, y0), point(x1, y1), color, 1.0f * scale,
                         ImDrawFlags_RoundCornersAll, width * scale);
    }

    void circle(float x, float y, float radius, float width = 1.4f) const {
        drawList.AddCircle(point(x, y), radius * scale, color, 16, width * scale);
    }

    void polyline(const ImVec2* points, int count, bool closed = false, float width = 1.4f) const {
        std::array<ImVec2, 8> mapped{};
        const int used = std::min(count, static_cast<int>(mapped.size()));
        for (int i = 0; i < used; ++i) mapped[static_cast<std::size_t>(i)] = point(points[i].x, points[i].y);
        drawList.AddPolyline(mapped.data(), used, color,
                             closed ? ImDrawFlags_Closed : ImDrawFlags_None, width * scale);
    }

};

void drawRender(const Canvas& c) {
    c.rect(1.5f, 2.0f, 14.5f, 14.0f);
    c.line(1.5f, 5.0f, 14.5f, 5.0f);
    c.circle(3.5f, 3.5f, 0.45f, 1.1f);
    c.circle(5.0f, 3.5f, 0.45f, 1.1f);
    c.circle(6.5f, 3.5f, 0.45f, 1.1f);
    c.circle(6.0f, 9.0f, 1.6f);
    c.line(6.0f, 6.6f, 6.0f, 5.9f, 1.1f);
    c.line(6.0f, 12.1f, 6.0f, 11.4f, 1.1f);
    const ImVec2 preview[] = {{2.7f, 13.0f}, {9.4f, 7.4f}, {13.3f, 13.0f}};
    c.polyline(preview, 3, false, 1.2f);
}

void drawOutput(const Canvas& c) {
    c.line(3.0f, 1.5f, 10.0f, 1.5f);
    c.line(3.0f, 1.5f, 3.0f, 14.5f);
    c.line(3.0f, 14.5f, 10.0f, 14.5f);
    c.line(10.0f, 1.5f, 10.0f, 5.0f);
    c.line(10.0f, 5.0f, 13.0f, 5.0f);
    c.line(10.0f, 1.5f, 13.0f, 5.0f);
    c.line(5.0f, 7.0f, 8.5f, 7.0f);
    c.line(5.0f, 9.0f, 8.5f, 9.0f);
    c.line(7.0f, 12.0f, 14.0f, 12.0f);
    c.line(11.5f, 9.5f, 14.0f, 12.0f);
    c.line(11.5f, 14.5f, 14.0f, 12.0f);
}

void drawScene(const Canvas& c) {
    c.line(8.0f, 3.5f, 8.0f, 7.5f);
    c.line(3.0f, 7.5f, 13.0f, 7.5f);
    c.line(3.0f, 7.5f, 3.0f, 10.5f);
    c.line(8.0f, 7.5f, 8.0f, 10.5f);
    c.line(13.0f, 7.5f, 13.0f, 10.5f);
    c.rect(5.5f, 1.5f, 10.5f, 5.0f);
    c.rect(1.0f, 10.5f, 5.0f, 14.5f);
    c.rect(6.0f, 10.5f, 10.0f, 14.5f);
    c.rect(11.0f, 10.5f, 15.0f, 14.5f);
}

void drawWorld(const Canvas& c) {
    c.circle(8.0f, 8.0f, 6.3f);
    c.line(2.0f, 8.0f, 14.0f, 8.0f, 1.1f);
    c.line(3.1f, 5.0f, 12.9f, 5.0f, 1.1f);
    c.line(3.1f, 11.0f, 12.9f, 11.0f, 1.1f);
    c.line(8.0f, 1.7f, 8.0f, 14.3f, 1.1f);
    c.line(5.0f, 2.5f, 4.0f, 8.0f, 1.1f);
    c.line(4.0f, 8.0f, 5.0f, 13.5f, 1.1f);
    c.line(11.0f, 2.5f, 12.0f, 8.0f, 1.1f);
    c.line(12.0f, 8.0f, 11.0f, 13.5f, 1.1f);
}

void drawObject(const Canvas& c) {
    const ImVec2 outline[] = {{8.0f, 1.5f}, {14.5f, 5.0f}, {14.5f, 11.0f}, {8.0f, 14.5f},
                              {1.5f, 11.0f}, {1.5f, 5.0f}};
    c.polyline(outline, 6, true, 1.5f);
    c.line(1.5f, 5.0f, 8.0f, 8.5f);
    c.line(14.5f, 5.0f, 8.0f, 8.5f);
    c.line(8.0f, 8.5f, 8.0f, 14.5f);
    c.line(8.0f, 1.5f, 8.0f, 8.5f, 1.1f);
}

void drawMaterial(const Canvas& c) {
    c.circle(8.0f, 8.0f, 6.0f, 1.5f);
    c.line(8.0f, 2.0f, 8.0f, 14.0f);
    c.line(8.0f, 8.0f, 12.4f, 4.0f, 1.1f);
    c.line(8.0f, 8.0f, 12.4f, 12.0f, 1.1f);
    c.line(9.0f, 3.0f, 12.7f, 5.2f, 1.0f);
    c.line(10.0f, 6.2f, 13.7f, 8.0f, 1.0f);
    c.line(10.0f, 9.8f, 13.7f, 8.0f, 1.0f);
    c.line(9.0f, 13.0f, 12.7f, 10.8f, 1.0f);
}

void drawCamera(const Canvas& c) {
    c.line(2.0f, 5.0f, 14.0f, 5.0f);
    c.line(14.0f, 5.0f, 14.0f, 14.0f);
    c.line(14.0f, 14.0f, 2.0f, 14.0f);
    c.line(2.0f, 14.0f, 2.0f, 5.0f);
    c.line(4.0f, 5.0f, 6.0f, 3.0f);
    c.line(6.0f, 3.0f, 10.0f, 3.0f);
    c.line(10.0f, 3.0f, 12.0f, 5.0f);
    c.line(8.0f, 7.0f, 10.0f, 9.5f);
    c.line(10.0f, 9.5f, 8.0f, 12.0f);
    c.line(8.0f, 12.0f, 6.0f, 9.5f);
    c.line(6.0f, 9.5f, 8.0f, 7.0f);
}

} // namespace

void drawIcon(ImDrawList& drawList, IconId icon, const ImVec2& buttonMin,
              const ImVec2& buttonMax, ImU32 color, float uiScale) {
    const float width = std::max(0.0f, buttonMax.x - buttonMin.x);
    const float height = std::max(0.0f, buttonMax.y - buttonMin.y);
    const float requestedSize = kGrid * std::max(uiScale, 0.01f);
    const float size = std::min({width, height, requestedSize});
    if (size <= 0.0f) return;

    const float scale = size / kGrid;
    const ImVec2 center((buttonMin.x + buttonMax.x) * 0.5f,
                        (buttonMin.y + buttonMax.y) * 0.5f);
    const Canvas canvas{drawList, color,
                        ImVec2(center.x - size * 0.5f, center.y - size * 0.5f), scale};
    switch (icon) {
        case IconId::Render:   drawRender(canvas); break;
        case IconId::Output:   drawOutput(canvas); break;
        case IconId::Scene:    drawScene(canvas); break;
        case IconId::World:    drawWorld(canvas); break;
        case IconId::Object:   drawObject(canvas); break;
        case IconId::Material: drawMaterial(canvas); break;
        case IconId::Camera:   drawCamera(canvas); break;
    }
}

} // namespace ember::editor

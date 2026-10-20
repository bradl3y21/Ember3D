#include "platform/Input.h"

namespace ember::platform {

namespace {
Input g_input; // process-wide; Window feeds it
inline bool inRange(int k) { return k >= 0 && k < Input::kMaxKeys; }
} // namespace

bool Input::keyDown(Key k) const {
    const int i = static_cast<int>(k);
    return inRange(i) && keyDown_[i];
}

bool Input::keyPressed(Key k) const {
    const int i = static_cast<int>(k);
    return inRange(i) && keyPressed_[i];
}

bool Input::mouseDown(MouseButton b) const {
    const int i = static_cast<int>(b);
    return i >= 0 && i < 8 && mouseDown_[i];
}

bool Input::mousePressed(MouseButton b) const {
    const int i = static_cast<int>(b);
    return i >= 0 && i < 8 && mousePressed_[i];
}

Vec2 Input::mousePosition() const { return mousePosition_; }
Vec2 Input::mouseDelta() const    { return mouseDelta_; }
Vec2 Input::scrollDelta() const   { return scrollDelta_; }

bool Input::cursorOverViewport() const {
    if (viewportW_ <= 0 || viewportH_ <= 0)
        return true; // no rect configured yet: whole window is the viewport
    const float x = mousePosition_.x, y = mousePosition_.y;
    return x >= static_cast<float>(viewportX_) &&
           y >= static_cast<float>(viewportY_) &&
           x <  static_cast<float>(viewportX_ + viewportW_) &&
           y <  static_cast<float>(viewportY_ + viewportH_);
}

void Input::reset() {
    *this = Input{};
}

void Input::beginFrame() {
    for (bool& v : keyPressed_) v = false;
    for (bool& v : mousePressed_) v = false;
    scrollDelta_ = {0.0f, 0.0f};
    frameStartPos_ = mousePosition_;
}

void Input::endFrame() {
    mouseDelta_ = {mousePosition_.x - frameStartPos_.x,
                   mousePosition_.y - frameStartPos_.y};
}

void Input::onKey(int glfwKey, bool down) {
    if (!inRange(glfwKey))
        return;
    if (down && !keyDown_[glfwKey])
        keyPressed_[glfwKey] = true;
    keyDown_[glfwKey] = down;
}

void Input::onMouseButton(int glfwButton, bool down) {
    if (glfwButton < 0 || glfwButton >= 8)
        return;
    if (down && !mouseDown_[glfwButton])
        mousePressed_[glfwButton] = true;
    mouseDown_[glfwButton] = down;
}

void Input::onCursorPos(float x, float y) {
    mousePosition_ = {x, y};
    hasMousePosition_ = true;
}

void Input::onScroll(float dx, float dy) {
    scrollDelta_.x += dx;
    scrollDelta_.y += dy;
}

void Input::setViewportRect(int x, int y, int w, int h,
                            int windowWidth, int windowHeight) {
    viewportX_ = x; viewportY_ = y; viewportW_ = w; viewportH_ = h;
    viewportCaptureW_ = windowWidth < 0 ? 0 : windowWidth;
    viewportCaptureH_ = windowHeight < 0 ? 0 : windowHeight;
}

Input::ViewportRect Input::viewportRect() const {
    return {viewportX_, viewportY_, viewportW_, viewportH_,
            viewportCaptureW_, viewportCaptureH_};
}

bool Input::rectIsStaleFor(int windowWidth, int windowHeight) const {
    if (viewportCaptureW_ == 0 || viewportCaptureH_ == 0)
        return false;   // capture size unknown -> cannot be stale
    if (windowWidth <= 0 || windowHeight <= 0)
        return false;   // no live window size to compare against
    return viewportCaptureW_ != windowWidth || viewportCaptureH_ != windowHeight;
}

Input& input() { return g_input; }

} // namespace ember::platform

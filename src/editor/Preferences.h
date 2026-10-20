// Preferences.h — user preferences: theme accent, navigation keymap, gizmo size.
// Owner: Chisel · Editor.
//
// There is no browser and no localStorage in this native app. Preferences are
// saved to a plain text file on this machine (%APPDATA%\Ember3D\preferences.ini),
// and the window says so. They do not follow the user to another computer.
#pragma once

#include <string>

#include "core/Theme.h"

namespace ember::editor {

struct Preferences {
    float accent[3] = {
        static_cast<float>(ember::kDefaultTheme.selection.r) / 255.0f,
        static_cast<float>(ember::kDefaultTheme.selection.g) / 255.0f,
        static_cast<float>(ember::kDefaultTheme.selection.b) / 255.0f};
    bool  invertOrbit = false;                 // read by App once Ash wires it
    bool  emulateNumpad = false;                // let top-row digits drive viewport navigation
    float gizmoSize = 90.0f;                   // handle length in pixels (40..200)
};

Preferences loadPreferences();                  // defaults when the file is absent
bool savePreferences(const Preferences& prefs); // false if the file cannot be written
std::string preferencesPath();

// Draws the Preferences window while `open` is true. Sets `changed` when a value
// was edited this frame (the caller applies the theme and saves).
void drawPreferences(Preferences& prefs, bool& open, bool& changed);

} // namespace ember::editor

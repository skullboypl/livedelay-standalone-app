// The settings window: what the OBS dock opens as a dialog, here as its own
// window. It edits livedelay.ini and the overlay document; the engine itself
// stays in livedelay.dll.

#pragma once

#include <string>
#include <windows.h>

namespace ld {

/// Opens the settings window and returns once it closes. True if anything was saved.
bool ShowSettings(HWND owner);

/// Restarts the engine with the ini as it is on disk. Defined by the app window,
/// which owns the DLL. Returns false and shows the reason on failure.
bool ReloadEngine();

/// Reads hotkey_* from the ini and registers them on the main window.
void ApplyHotkeys();
/// One line per shortcut: działa, nie ustawiony, zajęty, albo nie rozumiem.
std::wstring HotkeyReport();
/// Remember the ini's current write time so a save from this window is not
/// treated as an outside edit.
void TouchIniStamp();

} // namespace ld

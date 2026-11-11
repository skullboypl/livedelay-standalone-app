// The settings window: what the OBS dock opens as a dialog, here as its own
// window. It edits livedelay.ini and the overlay document; the engine itself
// stays in livedelay.dll.

#pragma once

#include <string>
#include <windows.h>

namespace ld {

/// Opens the settings window and returns once it closes. True if anything was saved.
bool ShowSettings(HWND owner);

/// Adds an `[input …]` section to the ini without opening the window: the
/// wizard's one click. When the file has no inputs yet, the ones the engine
/// synthesises today are written down first, so every platform keeps the feed
/// it has. Returns the name the input got — it may differ from the one asked
/// for, when that one is taken — or nothing when the file could not be written.
/// The caller reloads the engine; this only touches the file.
std::wstring AddInput(const std::wstring &name);

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

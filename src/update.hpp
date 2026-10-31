// Update checks for the standalone app. The OBS plugin has its own checker.
// Here there are two versions: this exe, and livedelay.dll (the plugin the app loads).

#pragma once

#include <windows.h>

#include <string>

namespace ld {

constexpr UINT WM_LD_UPDATE = WM_APP + 4;

std::wstring AppVersion();
std::wstring PluginVersion();
void SetPluginVersion(const std::wstring &version);

void UpdateBind(HWND hwnd);
void UpdatePoll();
void UpdateCheckNow();

bool UpdatePending();
bool UpdateBusy();
bool UpdateCanDownload();
std::wstring UpdateBannerText();
std::wstring UpdateActionLabel();
std::wstring UpdateStatus();

void UpdateLater();
void UpdateApply(HWND owner);
void UninstallApp(HWND owner);

} // namespace ld

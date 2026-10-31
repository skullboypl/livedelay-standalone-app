// Preview, overlay and the theme editor, drawn inside the main window.
#pragma once

#include <windows.h>

#include <string>

namespace ld {

void WebShow(HWND parent, const std::wstring &url, RECT bounds);
void WebHide();
bool WebFailed();

} // namespace ld

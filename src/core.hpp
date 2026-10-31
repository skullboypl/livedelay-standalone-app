// Shared by the OpenGL panel and the settings window: palette, DPI, the ini on
// disk and the engine's HTTP API. No drawing and no window code lives here.

#pragma once

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

namespace ld {

// ---------------------------------------------------------------- palette

struct Col {
	float r, g, b, a;
};

constexpr Col Rgb(unsigned hex, float a = 1.0f) {
	return {((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f, (hex & 0xff) / 255.0f, a};
}

constexpr Col Alpha(Col c, float a) {
	return {c.r, c.g, c.b, a};
}

// Mixes towards `over`, for flat GDI surfaces that cannot blend.
constexpr Col Mix(Col under, Col over, float t) {
	return {under.r + (over.r - under.r) * t, under.g + (over.g - under.g) * t, under.b + (over.b - under.b) * t, 1.0f};
}

inline COLORREF Gdi(Col c) {
	return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f));
}

// Straight from the site's stylesheet (web/public/styles.css, :root), so the
// window, the overlay and livedelay.vxh.pl are the same product.
constexpr Col kBg = Rgb(0x070707);    // --ink
constexpr Col kPanel = Rgb(0x0b0b0c); // --panel: sidebar, topbar, status bar
constexpr Col kCard = Rgb(0x111113);  // --raise
constexpr Col kLine = Rgb(0x1e1e21);
constexpr Col kLine2 = Rgb(0x2d2d31);
constexpr Col kFace = Rgb(0x17171a);
constexpr Col kFaceHot = Rgb(0x222226);
constexpr Col kText = Rgb(0xebe8e3); // warm off-white, not a blue white
constexpr Col kMuted = Rgb(0x8a8780);
constexpr Col kDim = Rgb(0x4d4b47);
constexpr Col kRed = Rgb(0xff3b2f);   // --tally
constexpr Col kGreen = Rgb(0x3ddc84); // --live
constexpr Col kAmber = Rgb(0xffbf3c); // --arm
// The site has no blue: one accent, and it is the live green.
constexpr Col kAccent = kGreen;
constexpr Col kGrey = kDim;
constexpr Col kNone = {0, 0, 0, 0};

/// The site's easing, as a frame-rate independent approach to a target.
inline float Ease(float current, float target, float dt, float tau = 0.085f) {
	const float k = 1.0f - std::exp(-dt / tau);
	return current + (target - current) * k;
}

// ---------------------------------------------------------------- DPI

extern float g_scale;

inline float S(float dp) {
	return dp * g_scale;
}

inline int Si(float dp) {
	return (int)std::lround(dp * g_scale);
}

// ---------------------------------------------------------------- text

std::wstring Utf8(const std::string &s);
std::string WideUtf8(const std::wstring &w);

// ---------------------------------------------------------------- files

std::string LoadText(const std::wstring &path);
bool StoreText(const std::wstring &path, const std::string &text);

// ---------------------------------------------------------------- the ini

// %APPDATA%\LiveDelay\livedelay.ini and the three values the window needs from it.
extern std::wstring g_ini;
extern std::wstring g_listen;
extern std::wstring g_api;
extern std::wstring g_ingest;

void SetIniPath(const std::wstring &path);
void ReadIniHints();
std::wstring IniLine(const std::string &text, const char *key);

// ---------------------------------------------------------------- engine API

// A body is sent as application/json; empty means no body.
std::string Http(const wchar_t *method, const std::wstring &path, const std::string &body = {});

std::wstring ApiBase();
std::wstring PreviewUrl();
std::wstring OverlayUrl();
std::wstring ThemeUrl();
std::wstring PanelUrl();
std::wstring RtmpUrl();

void OpenUrl(const std::wstring &url);
void Copy(HWND owner, const std::wstring &text);

// One line from this process, timestamped. The engine's own lines come from GET /logs.
void AppLog(const std::wstring &msg);
size_t AppLogCount();
std::vector<std::wstring> AppLogSnapshot();

// WS_THICKFRAME keeps the shadow, and Windows 11 then draws a light frame
// over the window whenever it loses and regains focus. Paint that frame away.
void SuppressSystemFrame(HWND hwnd);

// ---------------------------------------------------------------- JSON reading

std::string JsonString(const std::string &j, const char *key, size_t from = 0);
double JsonNumber(const std::string &j, const char *key, double def = 0, size_t from = 0);
bool JsonBool(const std::string &j, const char *key, size_t from = 0);

} // namespace ld

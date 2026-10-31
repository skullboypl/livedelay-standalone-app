// LiveDelay window. The delay engine is livedelay.dll from the plugin repo.
// This file is the OpenGL panel, the tray icon and the state it draws.
// Shared bits (palette, ini, HTTP) are in core.cpp; the settings window is in
// settings.cpp.

#include "core.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "webview.hpp"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "opengl32.lib")

using namespace ld;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;

constexpr int ID_SHOW = 1;
constexpr int ID_ARM = 2;
constexpr int ID_ACTIVATE = 3;
constexpr int ID_CUT = 4;
constexpr int ID_CUT_AFTER = 5;
constexpr int ID_DISARM = 6;
constexpr int ID_QUIT = 7;
constexpr int ID_DELAY_DOWN = 100;
constexpr int ID_DELAY_UP = 101;
constexpr int ID_COPY_RTMP = 102;
constexpr int ID_SETTINGS = 103;
constexpr int ID_PREVIEW = 104;
constexpr int ID_OVERLAY = 105;
constexpr int ID_COPY_OVERLAY = 106;
constexpr int ID_THEME = 107;
constexpr int ID_HELP = 108;
constexpr int ID_PANEL = 109;
constexpr int ID_COPY_INGEST = 111;
constexpr int ID_NAV_TOGGLE = 110;
constexpr int ID_MIN = 112;
constexpr int ID_MAX = 113;
constexpr int ID_CLOSE = 114;
constexpr int ID_COPY_LOGS = 115;
constexpr int ID_WEB_FALLBACK = 116;
constexpr int ID_COPY_WIDGET = 117;
constexpr int ID_UPDATE_GO = 130;
constexpr int ID_UPDATE_LATER = 131;
constexpr int ID_UPDATE_CHECK = 132;
constexpr int ID_DANGER_PICK = 133;
constexpr int ID_DANGER_RUN = 134;
// One per sidebar entry that switches the view: ID_NAV + View.
constexpr int ID_NAV = 120;
constexpr int HOTKEY_ARM = 0x4C01;
constexpr int HOTKEY_ACTIVATE = 0x4C02;
constexpr int HOTKEY_CUT = 0x4C03;
constexpr int HOTKEY_CUT_AFTER = 0x4C04;
// One per platform row: ID_DEST + index.
constexpr int ID_DEST = 200;

constexpr const wchar_t *kHelpUrl = L"https://livedelay.vxh.pl/";
// The only resource in app.rc: the site's mark, in every size Windows asks for.
constexpr int IDI_LOGO = 1;

using StartFn = int(__cdecl *)(const char *);
using StopFn = void(__cdecl *)();
using ErrFn = size_t(__cdecl *)(char *, size_t);

// The shell follows SmoothWizard's: a sidebar on its own shade, cards on the
// background, a status bar along the bottom. The colours stay LiveDelay's.
constexpr Col kInk = Rgb(0x0b0e13); // text on a solid accent

// Pill backgrounds, as in the dock.
constexpr Col kPillDelayed = Rgb(0xc62828);
constexpr Col kPillWaiting = Rgb(0x3b4a63);
constexpr Col kPillArmed = Rgb(0xb26a00);
constexpr Col kPillLive = Rgb(0x2e7d32);
constexpr Col kPillOffline = Rgb(0x5a5f6b);

HINSTANCE g_inst;
HWND g_hwnd;
HDC g_dc;
HGLRC g_gl;
HMODULE g_dll;
StartFn g_start;
StopFn g_stop;
ErrFn g_lastErr;
NOTIFYICONDATAW g_tray{};
HICON g_logo = nullptr;
HCURSOR g_arrow;
HCURSOR g_hand;

int g_delay = 15;
bool g_delaySet = false;
bool g_quit = false;
int g_hover = -1;
int g_pressed = -1;
bool g_tracking = false;

// Everything the window draws comes from one /state read.
bool g_connected = false;
bool g_delayed = false;
bool g_input = false;
bool g_armed = false;
bool g_cutPending = false;
double g_armDelay = -1;
double g_delaySec = 0;
double g_bufferS = 0;
double g_bufferMb = 0;
double g_inKbps = 0;

struct Dest {
	std::wstring name;
	std::string state;
	std::wstring error;
	double kbps = 0;
	bool on = false;
};

std::vector<Dest> g_dests;
std::vector<std::wstring> g_destTips;

// One bitrate sample a second per line, same as the dock's sparkline.
constexpr size_t kHistory = 48;
// Keyed by platform name; the incoming feed gets a name no platform can have.
const std::wstring kInputKey = L"\x01";
std::unordered_map<std::wstring, std::deque<double>> g_history;
ULONGLONG g_lastSample = 0;

// Grp is letterspaced small caps for section labels; Num and Mono carry the
// figures, in the role the site gives JetBrains Mono.
enum class Fnt { Body, Bold, Small, Pill, Grp, Title, Nav, Num, Mono };

constexpr int kFonts = 9;
HFONT g_fonts[kFonts]{};
int g_fontTrack[kFonts]{};

struct Glyph {
	GLuint tex = 0;
	int w = 0;
	int h = 0;
};

std::unordered_map<std::wstring, Glyph> g_text;

enum class Icon {
	None, Arm, Activate, Live, LiveAfter, Disarm, Settings, Preview, Overlay, Theme, Copy, Help, Gauge, Globe, Link,
	Logs, Info, Collapse, WinMin, WinMax, WinClose
};

// Normal is the outlined button; Solid is the one action that matters right
// now; Nav is a sidebar entry; Tile is a whole clickable card; Win is one of
// the three glyphs in our own title bar.
enum class BStyle { Normal, Solid, Nav, Tile, Win };

struct Button {
	float x = 0, y = 0, w = 0, h = 0;
	std::wstring label;
	int id = 0;
	Col accent = kNone;
	bool enabled = true;
	const wchar_t *tip = L"";
	Icon icon = Icon::None;
	BStyle style = BStyle::Normal;
	bool on = false;            // the sidebar entry of the view being shown
	std::wstring note;          // second line, for tiles
};

std::vector<Button> g_buttons;

struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
};

enum class View { Board, Addresses, Widgets, Logs, About };
enum class WebPage { Preview, Overlay, Theme };

WebPage g_webPage = WebPage::Overlay;

std::wstring WithEmbed(std::wstring url) {
	url += url.find(L'?') == std::wstring::npos ? L"?inapp=1" : L"&inapp=1";
	return url;
}

std::wstring PublicWebUrl() {
	switch (g_webPage) {
	case WebPage::Overlay:
		return OverlayUrl();
	case WebPage::Theme:
		return ThemeUrl();
	default:
		return PreviewUrl();
	}
}

std::wstring WebUrl() {
	return WithEmbed(PublicWebUrl());
}

View g_view = View::Board;
bool g_navMin = false;

// ---------------------------------------------------------------- motion
//
// Everything that moves eases towards a target with the site's timing. A frame
// timer only runs while something is actually moving, so an idle window costs
// nothing.

double g_now = 0;
float g_dt = 0.016f;
bool g_animating = false;
bool g_frameTimer = false;

std::unordered_map<int, float> g_hoverAmt; // per button, 0..1
float g_navWidth = 0;
float g_railY = 0, g_railH = 0;
float g_bufferShown = 0;
float g_viewSlide = 0;

float Approach(float &v, float target, float tau = 0.085f) {
	v = Ease(v, target, g_dt, tau);
	if (std::fabs(v - target) > 0.0025f)
		g_animating = true;
	else
		v = target;
	return v;
}

// Filled by Layout, read by Paint.
Rect g_topR;
Rect g_navR, g_mainR, g_footR, g_chipR, g_bannerR, g_urlR, g_aboutR, g_dangerR;
bool g_dangerOn = false;
Rect g_stateR, g_ctrlR, g_destsR, g_addrR, g_barR, g_delayBoxR, g_logR, g_webR;
float g_titleY = 0;
float g_grpY = 0;
bool g_showBar = false;

// ---------------------------------------------------------------- numbers

std::wstring Round(double v) {
	wchar_t buf[32];
	swprintf_s(buf, L"%.0f", v);
	return buf;
}

std::wstring Tenth(double v) {
	wchar_t buf[32];
	swprintf_s(buf, L"%.1f", v);
	return buf;
}

// ---------------------------------------------------------------- state

void ParseState(const std::string &j) {
	g_connected = true;
	g_delayed = JsonString(j, "mode") == "delayed";
	g_input = JsonBool(j, "input");
	g_armed = JsonBool(j, "armed");
	g_cutPending = JsonBool(j, "cut_pending");
	g_armDelay = JsonNumber(j, "arm_delay", -1);
	g_delaySec = JsonNumber(j, "delay");
	g_bufferS = JsonNumber(j, "buffer_s");
	g_bufferMb = JsonNumber(j, "buffer_mb");
	g_inKbps = JsonNumber(j, "in_kbps");

	// Take the engine's seconds once, then the window owns the value.
	if (!g_delaySet) {
		g_delaySet = true;
		double d = g_armDelay > 0 ? g_armDelay : JsonNumber(j, "default_delay", 15);
		g_delay = std::clamp((int)std::lround(d), 1, 3600);
	}

	g_dests.clear();
	size_t p = 0;
	// "name" is the first key of every destination object.
	while ((p = j.find("\"name\":\"", p)) != std::string::npos) {
		Dest d;
		d.name = Utf8(JsonString(j, "name", p));
		d.state = JsonString(j, "state", p);
		d.error = Utf8(JsonString(j, "error", p));
		d.kbps = JsonNumber(j, "kbps", 0, p);
		d.on = JsonBool(j, "on", p);
		if (!d.name.empty())
			g_dests.push_back(d);
		p += 8;
		if (g_dests.size() >= 8)
			break;
	}
}

void Offline() {
	g_connected = false;
	g_delayed = false;
	g_input = false;
	g_armed = false;
	g_cutPending = false;
	g_armDelay = -1;
	g_inKbps = 0;
	g_dests.clear();
}

std::string g_engineRaw;
std::vector<std::wstring> g_logView;
int g_logScroll = 0;
bool g_logStick = true;

std::wstring TagEngine(const std::wstring &line) {
	std::wstring s = line;
	size_t p = 0;
	while ((p = s.find(L"live_", p)) != std::wstring::npos) {
		size_t e = p + 5;
		while (e < s.size() && (iswalnum(s[e]) || s[e] == L'_'))
			++e;
		if (e - p > 12) {
			s.replace(p, e - p, L"live_…");
			p += 6;
		} else {
			p = e;
		}
	}
	if (s.size() >= 11 && s[0] == L'[')
		return s.substr(0, 11) + L"silnik: " + s.substr(11);
	return L"silnik: " + s;
}

void RebuildLogView() {
	std::vector<std::wstring> engine;
	std::wstring all = Utf8(g_engineRaw);
	size_t i = 0;
	while (i < all.size()) {
		size_t n = all.find(L'\n', i);
		std::wstring line = all.substr(i, n == std::wstring::npos ? std::wstring::npos : n - i);
		if (!line.empty() && line.back() == L'\r')
			line.pop_back();
		if (!line.empty())
			engine.push_back(TagEngine(line));
		if (n == std::wstring::npos)
			break;
		i = n + 1;
	}
	const std::vector<std::wstring> app = AppLogSnapshot();
	g_logView.clear();
	g_logView.reserve(engine.size() + app.size());
	size_t a = 0, b = 0;
	auto stamp = [](const std::wstring &s) { return s.size() >= 10 ? s.substr(0, 10) : s; };
	while (a < engine.size() || b < app.size()) {
		if (b >= app.size() || (a < engine.size() && stamp(engine[a]) <= stamp(app[b])))
			g_logView.push_back(std::move(engine[a++]));
		else
			g_logView.push_back(app[b++]);
	}
}

void RefreshLogs() {
	std::string body = Http(L"GET", L"/logs");
	const bool engineChanged = !body.empty() && body != g_engineRaw;
	if (engineChanged)
		g_engineRaw = std::move(body);
	static size_t seenApp = (size_t)-1;
	const size_t appCount = AppLogCount();
	if (!engineChanged && appCount == seenApp)
		return;
	seenApp = appCount;
	RebuildLogView();
}

void Refresh() {
	const bool was = g_connected;
	std::string body = Http(L"GET", L"/state");
	if (body.empty() || body.find("\"mode\"") == std::string::npos) {
		if (was)
			AppLog(L"utracono połączenie z silnikiem");
		Offline();
		return;
	}
	ParseState(body);
	if (!was)
		AppLog(L"połączono z silnikiem");
	RefreshLogs();
}

void PostAction(const std::wstring &path) {
	std::wstring full = path;
	if (path == L"/arm" || path == L"/settings")
		full += L"?delay=" + std::to_wstring(g_delay);
	std::string body = Http(L"POST", full);
	if (!body.empty() && body.find("\"mode\"") != std::string::npos)
		ParseState(body);
	else
		Refresh();
}

// ---------------------------------------------------------------- engine

std::wstring ExeDir() {
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s = path;
	size_t slash = s.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

enum class EngineLoad { Ok, Missing, Failed };

EngineLoad LoadEngine() {
	std::wstring dir = ExeDir();
	std::wstring candidates[] = {
		dir + L"\\livedelay.dll",
		dir + L"\\..\\..\\..\\livedelay\\target\\release\\livedelay.dll",
	};
	wchar_t env[MAX_PATH];
	if (GetEnvironmentVariableW(L"LIVEDLAY_DLL", env, MAX_PATH) > 0)
		candidates[0] = env;
	for (const std::wstring &path : candidates) {
		g_dll = LoadLibraryW(path.c_str());
		if (g_dll) {
			AppLog(L"załadowano silnik: " + path);
			break;
		}
	}
	if (!g_dll) {
		AppLog(L"nie ma livedelay.dll obok programu");
		return EngineLoad::Missing;
	}
	using HostFn = void(__cdecl *)(const char *);
	if (HostFn setHost = (HostFn)GetProcAddress(g_dll, "ld_set_host"))
		setHost("app");
	using VerFn = const char *(__cdecl *)();
	if (VerFn ver = (VerFn)GetProcAddress(g_dll, "ld_version"))
		if (const char *s = ver())
			SetPluginVersion(Utf8(s));
	AppLog(L"wtyczka " + PluginVersion());
	g_start = (StartFn)GetProcAddress(g_dll, "ld_start");
	g_stop = (StopFn)GetProcAddress(g_dll, "ld_stop");
	g_lastErr = (ErrFn)GetProcAddress(g_dll, "ld_last_error");
	if (!g_start || !g_stop || !g_lastErr)
		return EngineLoad::Missing;
	std::string ini = WideUtf8(g_ini);
	if (g_start(ini.c_str()) != 0) {
		char msg[512] = {};
		g_lastErr(msg, sizeof msg);
		AppLog(L"silnik nie wystartował");
		MessageBoxW(nullptr, Utf8(msg[0] ? msg : "ld_start failed").c_str(), L"LiveDelay", MB_OK | MB_ICONWARNING);
		return EngineLoad::Failed;
	}
	AppLog(L"silnik wystartował");
	return EngineLoad::Ok;
}

// ---------------------------------------------------------------- drawing

void Use(Col c) {
	glColor4f(c.r, c.g, c.b, c.a);
}

// Points of a rounded rectangle, walked clockwise from the top-left corner.
void RoundPath(float x, float y, float w, float h, float r, std::vector<float> &out) {
	out.clear();
	r = std::min(r, std::min(w, h) * 0.5f);
	const int seg = 5;
	struct Corner {
		float cx, cy, a0;
	};
	const Corner corners[4] = {
		{x + r, y + r, 180.0f},
		{x + w - r, y + r, 270.0f},
		{x + w - r, y + h - r, 0.0f},
		{x + r, y + h - r, 90.0f},
	};
	for (const Corner &c : corners) {
		for (int i = 0; i <= seg; ++i) {
			float a = (c.a0 + 90.0f * i / seg) * 3.14159265f / 180.0f;
			out.push_back(c.cx + std::cos(a) * r);
			out.push_back(c.cy + std::sin(a) * r);
		}
	}
}

void FillRound(float x, float y, float w, float h, float r, Col c) {
	if (c.a <= 0.0f || w <= 0 || h <= 0)
		return;
	static std::vector<float> pts;
	RoundPath(x, y, w, h, r, pts);
	glDisable(GL_TEXTURE_2D);
	Use(c);
	glBegin(GL_TRIANGLE_FAN);
	glVertex2f(x + w * 0.5f, y + h * 0.5f);
	for (size_t i = 0; i < pts.size(); i += 2)
		glVertex2f(pts[i], pts[i + 1]);
	glVertex2f(pts[0], pts[1]);
	glEnd();
}

// The outline is anti-aliased, which also hides the fill's stair-stepped corners.
void StrokeRound(float x, float y, float w, float h, float r, Col c, float thick = 1.0f) {
	if (c.a <= 0.0f || w <= 0 || h <= 0)
		return;
	static std::vector<float> pts;
	RoundPath(x + 0.5f, y + 0.5f, w - 1, h - 1, r, pts);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(thick);
	Use(c);
	glBegin(GL_LINE_LOOP);
	for (size_t i = 0; i < pts.size(); i += 2)
		glVertex2f(pts[i], pts[i + 1]);
	glEnd();
	glLineWidth(1.0f);
}

void Fill(float x, float y, float w, float h, Col c) {
	if (c.a <= 0.0f || w <= 0 || h <= 0)
		return;
	glDisable(GL_TEXTURE_2D);
	Use(c);
	glBegin(GL_QUADS);
	glVertex2f(x, y);
	glVertex2f(x + w, y);
	glVertex2f(x + w, y + h);
	glVertex2f(x, y + h);
	glEnd();
}

void FillDot(float cx, float cy, float r, Col c) {
	glDisable(GL_TEXTURE_2D);
	Use(c);
	glBegin(GL_TRIANGLE_FAN);
	glVertex2f(cx, cy);
	for (int i = 0; i <= 18; ++i) {
		float a = 2 * 3.14159265f * i / 18;
		glVertex2f(cx + std::cos(a) * r, cy + std::sin(a) * r);
	}
	glEnd();
}

void Ring(float cx, float cy, float r, Col c, float thick) {
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(thick);
	Use(c);
	glBegin(GL_LINE_LOOP);
	for (int i = 0; i < 24; ++i) {
		float a = 2 * 3.14159265f * i / 24;
		glVertex2f(cx + std::cos(a) * r, cy + std::sin(a) * r);
	}
	glEnd();
	glLineWidth(1.0f);
}

// Degrees, clockwise from three o'clock, in the window's y-down space.
void Arc(float cx, float cy, float rx, float ry, float a0, float a1, Col c, float thick) {
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(thick);
	Use(c);
	glBegin(GL_LINE_STRIP);
	const int seg = 20;
	for (int i = 0; i <= seg; ++i) {
		const float a = (a0 + (a1 - a0) * i / seg) * 3.14159265f / 180.0f;
		glVertex2f(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
	}
	glEnd();
	glLineWidth(1.0f);
}

void Stroke(float x1, float y1, float x2, float y2, Col c, float thick) {
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(thick);
	Use(c);
	glBegin(GL_LINES);
	glVertex2f(x1, y1);
	glVertex2f(x2, y2);
	glEnd();
	glLineWidth(1.0f);
}

void FillTri(float x1, float y1, float x2, float y2, float x3, float y3, Col c) {
	glDisable(GL_TEXTURE_2D);
	Use(c);
	glBegin(GL_TRIANGLES);
	glVertex2f(x1, y1);
	glVertex2f(x2, y2);
	glVertex2f(x3, y3);
	glEnd();
}

// Icons live on a 24x24 grid, like the dock's.
void DrawIcon(Icon kind, float x, float y, float size, Col c) {
	if (kind == Icon::None)
		return;
	const float u = size / 24.0f;
	auto px = [&](float v) { return x + v * u; };
	auto py = [&](float v) { return y + v * u; };
	const float t = std::max(1.4f, 2.2f * u);
	switch (kind) {
	case Icon::Arm: // ring with a filled centre: recording into the buffer
		Ring(px(12), py(12), 8.5f * u, c, t);
		FillDot(px(12), py(12), 3.6f * u, c);
		break;
	case Icon::Activate: // clock
		Ring(px(12), py(12), 8.5f * u, c, t);
		Stroke(px(12), py(12), px(12), py(7), c, t);
		Stroke(px(12), py(12), px(15.5f), py(14), c, t);
		break;
	case Icon::Live:
	case Icon::LiveAfter: // skip to the live edge
		FillTri(px(5), py(6), px(14), py(12), px(5), py(18), c);
		Stroke(px(18), py(6), px(18), py(18), c, t);
		if (kind == Icon::LiveAfter)
			Ring(px(19), py(19), 4 * u, c, std::max(1.2f, 1.6f * u));
		break;
	case Icon::Disarm: // circle with a cross
		Ring(px(12), py(12), 8.5f * u, c, t);
		Stroke(px(9), py(9), px(15), py(15), c, t);
		Stroke(px(15), py(9), px(9), py(15), c, t);
		break;
	case Icon::Settings: // sliders
		for (int i = 0; i < 3; ++i) {
			float ly = 6.0f + i * 6.0f;
			float knob = i == 1 ? 15.0f : 9.0f;
			Stroke(px(4), py(ly), px(20), py(ly), c, t);
			FillDot(px(knob), py(ly), 2.4f * u, c);
		}
		break;
	case Icon::Preview: // screen with a play mark
		StrokeRound(px(3), py(5), 18 * u, 12 * u, 2 * u, c, t);
		Stroke(px(8), py(20), px(16), py(20), c, t);
		FillTri(px(10.5f), py(8.5f), px(14.5f), py(11), px(10.5f), py(13.5f), c);
		break;
	case Icon::Overlay: // a badge sitting on a frame
		StrokeRound(px(3), py(5), 18 * u, 14 * u, 2 * u, c, t);
		FillRound(px(6), py(13), 9 * u, 3.5f * u, 1.75f * u, c);
		break;
	case Icon::Theme: // palette
		Ring(px(12), py(12), 8.5f * u, c, t);
		FillDot(px(9), py(9.5f), 1.7f * u, c);
		FillDot(px(14.5f), py(9.5f), 1.7f * u, c);
		FillDot(px(11.5f), py(15), 1.7f * u, c);
		break;
	case Icon::Copy: // two sheets
		StrokeRound(px(4), py(4), 11 * u, 13 * u, 2 * u, c, t);
		StrokeRound(px(9), py(8), 11 * u, 13 * u, 2 * u, c, t);
		break;
	case Icon::Help:
		Ring(px(12), py(12), 8.5f * u, c, t);
		Stroke(px(9.6f), py(9.4f), px(14.4f), py(9.4f), c, t);
		Stroke(px(14.4f), py(9.4f), px(12), py(13.5f), c, t);
		FillDot(px(12), py(16.8f), 1.3f * u, c);
		break;
	case Icon::Gauge: // a dial with its needle: the board
		Arc(px(12), py(14), 8.0f * u, 8.0f * u, 180.0f, 360.0f, c, t);
		Stroke(px(12), py(14), px(16), py(9), c, t);
		FillDot(px(12), py(14), 1.6f * u, c);
		break;
	case Icon::Globe: // the browser panel
		Ring(px(12), py(12), 8.5f * u, c, t);
		Stroke(px(3.5f), py(12), px(20.5f), py(12), c, t);
		Arc(px(12), py(12), 4.2f * u, 8.5f * u, 0.0f, 360.0f, c, t); // the meridian
		break;
	case Icon::Logs: // three lines of a log
		for (int i = 0; i < 3; ++i) {
			const float ly = 6.5f + i * 5.5f;
			FillDot(px(5.5f), py(ly), 1.3f * u, c);
			Stroke(px(9), py(ly), px(19), py(ly), c, t);
		}
		break;
	case Icon::Info: // the about mark
		Ring(px(12), py(12), 8.5f * u, c, t);
		FillDot(px(12), py(8), 1.4f * u, c);
		Stroke(px(12), py(11.5f), px(12), py(16.5f), c, t);
		break;
	case Icon::Link: // a plug on a lead: the addresses
		StrokeRound(px(8), py(4), 8 * u, 7 * u, 2 * u, c, t);
		Stroke(px(10), py(4), px(10), py(2), c, t);
		Stroke(px(14), py(4), px(14), py(2), c, t);
		Stroke(px(12), py(11), px(12), py(15), c, t);
		Arc(px(12), py(18), 4.0f * u, 3.5f * u, 180.0f, 360.0f, c, t);
		break;
	case Icon::Collapse:
		Stroke(px(14), py(7), px(9), py(12), c, t);
		Stroke(px(9), py(12), px(14), py(17), c, t);
		Stroke(px(18), py(6), px(18), py(18), c, t);
		break;
	case Icon::WinMin:
		Stroke(px(5), py(12), px(19), py(12), c, t);
		break;
	case Icon::WinMax:
		StrokeRound(px(5.5f), py(5.5f), 13 * u, 13 * u, 1.5f * u, c, t);
		break;
	case Icon::WinClose:
		Stroke(px(6), py(6), px(18), py(18), c, t);
		Stroke(px(18), py(6), px(6), py(18), c, t);
		break;
	default:
		break;
	}
}

// The logo from app.rc, uploaded once as a texture for the sidebar.
GLuint g_logoTex = 0;
int g_logoPx = 0;

GLuint LogoTexture() {
	const int side = std::max(32, Si(26) * 2);
	if (g_logoTex && g_logoPx == side)
		return g_logoTex;
	HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_LOGO), IMAGE_ICON, side, side, LR_DEFAULTCOLOR);
	if (!ic)
		return 0;
	ICONINFO info{};
	if (!GetIconInfo(ic, &info)) {
		DestroyIcon(ic);
		return 0;
	}
	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = side;
	bi.bmiHeader.biHeight = -side;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	std::vector<unsigned char> px((size_t)side * side * 4);
	HDC screen = GetDC(nullptr);
	GetDIBits(screen, info.hbmColor, 0, side, px.data(), &bi, DIB_RGB_COLORS);
	ReleaseDC(nullptr, screen);
	for (size_t i = 0; i + 3 < px.size(); i += 4)
		std::swap(px[i], px[i + 2]); // BGRA from GDI, RGBA for GL
	if (!g_logoTex)
		glGenTextures(1, &g_logoTex);
	glBindTexture(GL_TEXTURE_2D, g_logoTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	g_logoPx = side;
	DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);
	DestroyIcon(ic);
	return g_logoTex;
}

void DrawLogo(float x, float y, float size) {
	GLuint tex = LogoTexture();
	if (!tex)
		return;
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0);
	glVertex2f(x, y);
	glTexCoord2f(1, 0);
	glVertex2f(x + size, y);
	glTexCoord2f(1, 1);
	glVertex2f(x + size, y + size);
	glTexCoord2f(0, 1);
	glVertex2f(x, y + size);
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

Glyph MakeText(const std::wstring &text, Fnt f) {
	Glyph g;
	if (text.empty())
		return g;
	HDC mem = CreateCompatibleDC(nullptr);
	SelectObject(mem, g_fonts[(int)f]);
	SetTextCharacterExtra(mem, g_fontTrack[(int)f]);
	SIZE sz{};
	GetTextExtentPoint32W(mem, text.c_str(), (int)text.size(), &sz);
	int w = std::max(1L, sz.cx + 2 + g_fontTrack[(int)f]);
	int h = std::max(1L, sz.cy + 2);
	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = nullptr;
	HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	SelectObject(mem, bmp);
	SetBkMode(mem, TRANSPARENT);
	SetTextColor(mem, RGB(255, 255, 255));
	TextOutW(mem, 0, 0, text.c_str(), (int)text.size());
	auto *px = static_cast<unsigned char *>(bits);
	for (int i = 0; i < w * h; ++i) {
		unsigned char a = std::max(px[i * 4 + 2], std::max(px[i * 4 + 1], px[i * 4 + 0]));
		px[i * 4 + 0] = 255;
		px[i * 4 + 1] = 255;
		px[i * 4 + 2] = 255;
		px[i * 4 + 3] = a;
	}
	glGenTextures(1, &g.tex);
	glBindTexture(GL_TEXTURE_2D, g.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits);
	g.w = w;
	g.h = h;
	DeleteObject(bmp);
	DeleteDC(mem);
	return g;
}

void ClearTextCache() {
	for (auto &kv : g_text)
		glDeleteTextures(1, &kv.second.tex);
	g_text.clear();
}

Glyph &TextOf(const std::wstring &text, Fnt f) {
	std::wstring key(1, (wchar_t)(L'0' + (int)f));
	key += text;
	auto it = g_text.find(key);
	if (it != g_text.end())
		return it->second;
	if (g_text.size() > 240)
		ClearTextCache();
	return g_text.emplace(key, MakeText(text, f)).first->second;
}

float TextW(const std::wstring &text, Fnt f) {
	return (float)TextOf(text, f).w;
}

float TextH(const std::wstring &text, Fnt f) {
	return (float)TextOf(text, f).h;
}

void Text(float x, float y, const std::wstring &text, Col c, Fnt f = Fnt::Body) {
	Glyph &glyph = TextOf(text, f);
	if (!glyph.tex)
		return;
	glEnable(GL_TEXTURE_2D);
	Use(c);
	glBindTexture(GL_TEXTURE_2D, glyph.tex);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0);
	glVertex2f(x, y);
	glTexCoord2f(1, 0);
	glVertex2f(x + glyph.w, y);
	glTexCoord2f(1, 1);
	glVertex2f(x + glyph.w, y + glyph.h);
	glTexCoord2f(0, 1);
	glVertex2f(x, y + glyph.h);
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

// y is the middle of the line.
void TextMid(float x, float cy, const std::wstring &text, Col c, Fnt f = Fnt::Body) {
	Text(x, cy - TextH(text, f) * 0.5f, text, c, f);
}

void TextRight(float rx, float cy, const std::wstring &text, Col c, Fnt f = Fnt::Body) {
	Text(rx - TextW(text, f), cy - TextH(text, f) * 0.5f, text, c, f);
}

void TextCenter(float cx, float cy, const std::wstring &text, Col c, Fnt f = Fnt::Body) {
	Text(cx - TextW(text, f) * 0.5f, cy - TextH(text, f) * 0.5f, text, c, f);
}

// ---------------------------------------------------------------- state text

std::wstring PillText(Col &bg) {
	if (!g_connected) {
		bg = kPillOffline;
		return L"LiveDelay nie działa";
	}
	if (g_delayed) {
		bg = kPillDelayed;
		return L"OPÓŹNIENIE " + Round(g_armDelay > 0 ? g_armDelay : g_delaySec) + L" s";
	}
	if (!g_input) {
		bg = kPillWaiting;
		return L"CZEKAM NA STREAM";
	}
	if (g_armed) {
		bg = kPillArmed;
		return L"NA ŻYWO · uzbrojone " + Round(g_armDelay) + L" s";
	}
	if (g_armDelay > 0) {
		bg = kPillArmed;
		return L"NA ŻYWO · bufor " + Round(g_bufferS) + L"/" + Round(g_armDelay) + L" s";
	}
	bg = kPillLive;
	return L"NA ŻYWO";
}

std::wstring DestErrorLong(const Dest &d);

std::wstring DetailText() {
	if (!g_connected)
		return L"Silnik nie odpowiada na " + g_api;
	std::wstring d;
	if (!g_input)
		d = g_delayed ? L"Program na tym PC przestał nadawać. Wysyłam resztę buforu."
			      : L"Rozpocznij transmisję na tym PC. Uzbroić możesz już teraz.";
	else if (g_armDelay > 0)
		d = L"Bufor " + Tenth(g_bufferS) + L" s · " + Round(g_bufferMb) + L" MB w RAM";
	for (const Dest &dest : g_dests) {
		if (dest.state == "error") {
			d += (d.empty() ? L"" : L"   ") + dest.name + L": " + DestErrorLong(dest);
			break;
		}
	}
	if (g_cutPending)
		d += (d.empty() ? L"" : L"   ") + std::wstring(L"Powrót na żywo czeka, aż ta chwila dotrze do widzów.");
	return d;
}

std::wstring DestErrorLong(const Dest &d) {
	const std::wstring &e = d.error;
	if (e.find(L"end of file") != std::wstring::npos || e.find(L"connection reset") != std::wstring::npos)
		return L"połączenie zerwane — sprawdź klucz i serwer";
	if (e.find(L"timed out") != std::wstring::npos)
		return L"serwer nie odpowiada";
	if (e.find(L"BadName") != std::wstring::npos || e.find(L"Rejected") != std::wstring::npos)
		return L"serwer odrzucił klucz";
	if (!e.empty())
		return e;
	return L"błąd wysyłki";
}

std::wstring DestErrorShort(const Dest &d) {
	const std::wstring &e = d.error;
	if (e.find(L"end of file") != std::wstring::npos || e.find(L"connection reset") != std::wstring::npos)
		return L"zerwane";
	if (e.find(L"timed out") != std::wstring::npos)
		return L"brak odpowiedzi";
	if (e.find(L"BadName") != std::wstring::npos || e.find(L"Rejected") != std::wstring::npos)
		return L"odrzucony klucz";
	return L"błąd";
}

std::wstring DestState(const Dest &d) {
	if (d.state == "waiting")
		return L"czeka";
	if (d.state == "connecting")
		return L"łączy";
	if (d.state == "live")
		return L"nadaje";
	if (d.state == "error")
		return DestErrorShort(d);
	if (d.state == "stopped")
		return L"zatrzymana";
	return Utf8(d.state);
}

Col DestColor(const Dest &d) {
	if (d.state == "live")
		return kGreen;
	if (d.state == "error")
		return kRed;
	if (d.state == "stopped")
		return kGrey;
	return kAmber;
}

// ---------------------------------------------------------------- layout

constexpr const wchar_t *kTipDelay = L"Opóźnienie przy Uzbrój. Bufor siedzi w RAM: dłuższy delay = więcej pamięci.";

/// Breathing for the states that are doing something: the tally light is alive.
float Pulse() {
	if (!g_connected || !(g_delayed || (g_input && !g_armed && g_armDelay > 0)))
		return 1.0f;
	g_animating = true;
	return 0.58f + 0.42f * (0.5f + 0.5f * std::sin((float)g_now * 3.0f));
}

// Which badge the tray icon wears; also the colour of the state rail and chip.
int StateClass() {
	if (!g_connected)
		return 0;
	if (g_delayed)
		return 1;
	if (!g_input)
		return 2;
	if (g_armed || g_armDelay > 0)
		return 3;
	return 4;
}

Col StateColor(int kind) {
	switch (kind) {
	case 1:
		return kRed; // on delay
	case 2:
		return kMuted; // waiting for a stream: no colour, nothing is happening
	case 3:
		return kAmber; // arming or armed
	case 4:
		return kGreen; // live
	default:
		return kDim; // the engine is not answering
	}
}

void Push(float x, float y, float w, float h, std::wstring label, int id, Col accent, bool enabled, const wchar_t *tip,
	  Icon icon, BStyle style = BStyle::Normal, bool on = false, std::wstring note = {}) {
	Button b;
	b.x = x;
	b.y = y;
	b.w = w;
	b.h = h;
	b.label = std::move(label);
	b.id = id;
	b.accent = accent;
	b.enabled = enabled;
	b.tip = tip;
	b.icon = icon;
	b.style = style;
	b.on = on;
	b.note = std::move(note);
	g_buttons.push_back(b);
}

/// The one action the current state calls for. It is the one drawn solid.
int PrimaryAction() {
	if (!g_connected)
		return 0;
	if (g_delayed)
		return ID_CUT;
	if (g_armed)
		return ID_ACTIVATE;
	return ID_ARM;
}

/// The number worth setting in big type, with its caption. Empty when there is none.
std::wstring BigNumber(std::wstring &caption) {
	if (!g_connected)
		return {};
	if (g_delayed) {
		caption = L"OPÓŹNIENIE";
		return Round(g_armDelay > 0 ? g_armDelay : g_delaySec) + L" s";
	}
	if (g_armed) {
		caption = L"UZBROJONE NA";
		return Round(g_armDelay) + L" s";
	}
	if (g_armDelay > 0) {
		caption = L"BUFOR";
		return Round(g_bufferS) + L"/" + Round(g_armDelay) + L" s";
	}
	return {};
}

std::wstring ViewTitle() {
	switch (g_view) {
	case View::Addresses:
		return L"Adresy";
	case View::Widgets:
		return L"Widgety";
	case View::Logs:
		return L"Logi";
	case View::About:
		return L"O aplikacji";
	default:
		return L"Pulpit";
	}
}

// ---------------------------------------------------------------- layout

struct NavEntry {
	const wchar_t *label;
	Icon icon;
	int id;
	const wchar_t *tip;
};

void LayoutNav(float height) {
	const float pad = S(12);
	const float itemW = g_navR.w - 2 * pad;
	float y = g_navR.y + S(14);

	const NavEntry views[] = {
		{L"Pulpit", Icon::Gauge, ID_NAV + (int)View::Board, L"Stan, opóźnienie i platformy."},
		{L"Adresy", Icon::Link, ID_NAV + (int)View::Addresses, L"Co wkleić w programie na tym PC i w źródle przeglądarki."},
		{L"Widgety", Icon::Preview, ID_NAV + (int)View::Widgets, L"Overlay, podgląd i edytor wyglądu."},
		{L"Logi", Icon::Logs, ID_NAV + (int)View::Logs, L"Wpisy silnika i tej aplikacji."},
		{L"O aplikacji", Icon::Info, ID_NAV + (int)View::About, L"Wersja programu i wtyczki, sprawdzanie aktualizacji."},
	};
	for (const NavEntry &e : views) {
		Push(pad, y, itemW, S(40), e.label, e.id, kNone, true, e.tip, e.icon, BStyle::Nav, e.id - ID_NAV == (int)g_view);
		y += S(41);
	}

	// Settings belong with the window's own views; the group below is only for
	// what opens somewhere else.
	Push(pad, y, itemW, S(40), L"Ustawienia", ID_SETTINGS, kNone, true, L"Opóźnienie, platformy, overlay i skróty.",
	     Icon::Settings, BStyle::Nav);
	y += S(41);

	g_grpY = y + S(13);
	y += S(13) + S(14) + S(6);

	const NavEntry links[] = {
		{L"Webpanel", Icon::Globe, ID_PANEL, L"Wszystkie opcje w przeglądarce. Klucze zostają na tym komputerze."},
		{L"Pomoc", Icon::Help, ID_HELP, L"Instrukcja krok po kroku na livedelay.vxh.pl."},
	};
	for (const NavEntry &e : links) {
		Push(pad, y, itemW, S(40), e.label, e.id, kNone, true, e.tip, e.icon, BStyle::Nav);
		y += S(41);
	}

	Push(pad, height - S(12) - S(40), itemW, S(40), g_navMin ? L"" : L"Zwiń menu", ID_NAV_TOGGLE, kNone, true,
	     g_navMin ? L"Rozwiń menu." : L"Zwęź menu do samych ikon.", Icon::Collapse, BStyle::Nav);
}

void LayoutBoard(float x, float y, float w, float bottom) {
	const float pad = S(18);
	const float bh = S(36);

	g_stateR = {x, y, w, S(116)};
	g_showBar = g_connected && g_input && !g_delayed && g_armDelay > 0 && !g_armed;
	g_barR = {x + S(28), g_stateR.y + g_stateR.h - S(24), w - S(56), S(6)};
	y += g_stateR.h + S(16);

	const float ctrlH = pad + S(22) + S(40) + S(10) + bh + S(8) + bh + pad;
	g_ctrlR = {x, y, w, ctrlH};
	const float cx = x + pad;
	const float cw = w - 2 * pad;
	float cy = g_ctrlR.y + pad + S(22);

	const float step = S(40);
	Push(cx, cy, step, S(40), L"−", ID_DELAY_DOWN, kNone, g_delay > 1, kTipDelay, Icon::None);
	g_delayBoxR = {cx + step + S(6), cy, S(88), S(40)};
	const float plusX = g_delayBoxR.x + g_delayBoxR.w + S(6);
	Push(plusX, cy, step, S(40), L"+", ID_DELAY_UP, kNone, g_delay < 3600, kTipDelay, Icon::None);

	const int primary = PrimaryAction();
	const float armX = plusX + step + S(12);
	Push(armX, cy, cx + cw - armX, S(40), L"Uzbrój", ID_ARM, kAmber, g_connected && !g_delayed,
	     L"Zacznij trzymać bufor w RAM na wybrane opóźnienie.", Icon::Arm,
	     primary == ID_ARM ? BStyle::Solid : BStyle::Normal);
	cy += S(40) + S(10);

	const float half = (cw - S(8)) * 0.5f;
	Push(cx, cy, half, bh, L"Włącz opóźnienie", ID_ACTIVATE, kRed, g_connected && !g_delayed && g_armed,
	     L"Widzowie cofną się do klatki sprzed wybranej liczby sekund.", Icon::Activate,
	     primary == ID_ACTIVATE ? BStyle::Solid : BStyle::Normal);
	Push(cx + half + S(8), cy, half, bh, L"Na żywo", ID_CUT, kGreen, g_connected && g_delayed, L"Wróć na żywo teraz.",
	     Icon::Live, primary == ID_CUT ? BStyle::Solid : BStyle::Normal);
	cy += bh + S(8);
	Push(cx, cy, half, bh, L"Na żywo po emisji", ID_CUT_AFTER, kGreen, g_connected && g_delayed && !g_cutPending,
	     L"Wróć na żywo, gdy obecna chwila dotrze do widzów.", Icon::LiveAfter);
	Push(cx + half + S(8), cy, half, bh, L"Rozbrój", ID_DISARM, kNone, g_connected && !g_delayed && g_armDelay > 0,
	     L"Przestań trzymać bufor i zwolnij pamięć.", Icon::Disarm);
	y += ctrlH + S(16);

	g_destsR = {x, y, w, std::max(S(118), bottom - y)};
	const float rowH = S(32);
	const float toggleW = S(80);
	const float toggleH = S(26);
	g_destTips.clear();
	g_destTips.reserve(g_dests.size());
	for (size_t i = 0; i < g_dests.size(); ++i) {
		const float rowY = g_destsR.y + S(44) + rowH * (i + 1);
		if (rowY + rowH > g_destsR.y + g_destsR.h)
			break;
		const bool on = g_dests[i].on;
		std::wstring tip = on ? L"Zakończ stream tylko na tej platformie." : L"Zacznij wysyłać na tę platformę teraz.";
		if (g_dests[i].state == "error")
			tip = g_dests[i].name + L": " + DestErrorLong(g_dests[i]);
		g_destTips.push_back(std::move(tip));
		Push(g_destsR.x + g_destsR.w - pad - toggleW, rowY + (rowH - toggleH) * 0.5f, toggleW, toggleH,
		     on ? L"Zakończ" : L"Live", ID_DEST + (int)i, on ? kRed : kGreen, g_connected, g_destTips.back().c_str(),
		     Icon::None);
	}
}

constexpr float kAddrRowH = 56;

void LayoutAddresses(float x, float y, float w, float bottom) {
	(void)bottom;
	g_addrR = {x, y, w, S(56) + S(kAddrRowH) * 3 + S(6)};
	const float bw = S(104);
	const int ids[3] = {ID_COPY_RTMP, ID_COPY_INGEST, ID_COPY_OVERLAY};
	const wchar_t *tips[3] = {
		L"Skopiuj adres serwera do ustawień transmisji.",
		L"Skopiuj klucz, którym program na tym PC publikuje do LiveDelay.",
		L"Skopiuj adres overlaya do źródła Przeglądarka.",
	};
	for (int i = 0; i < 3; ++i) {
		const float ry = g_addrR.y + S(56) + S(kAddrRowH) * i;
		Push(g_addrR.x + g_addrR.w - S(18) - bw, ry + S(8), bw, S(32), L"Kopiuj", ids[i], kNone, true, tips[i], Icon::Copy);
	}
}

void LayoutWidgets(float x, float y, float w, float bottom) {
	const float gap = S(8);
	const float bw = (w - 2 * gap) / 3.0f;
	struct Tab {
		const wchar_t *label;
		int id;
		WebPage page;
		const wchar_t *tip;
	};
	const Tab tabs[] = {
		{L"Overlay", ID_OVERLAY, WebPage::Overlay, L"Licznik opóźnienia dla widzów. Adres wklejasz w źródło Przeglądarka."},
		{L"Podgląd", ID_PREVIEW, WebPage::Preview, L"Obraz z tego PC i to, co widzą widzowie."},
		{L"Motyw", ID_THEME, WebPage::Theme, L"Motyw, kolory i czcionka overlaya."},
	};
	for (int i = 0; i < 3; ++i) {
		const bool on = g_webPage == tabs[i].page;
		Push(x + i * (bw + gap), y, bw, S(36), tabs[i].label, tabs[i].id, on ? kAccent : kNone, true, tabs[i].tip, Icon::None,
		     on ? BStyle::Solid : BStyle::Normal);
	}
	g_urlR = {x, y + S(36) + S(8), w, S(36)};
	Push(g_urlR.x + g_urlR.w - S(8) - S(96), g_urlR.y + S(4), S(96), S(28), L"Kopiuj", ID_COPY_WIDGET, kNone, true,
	     L"Skopiuj adres tej strony.", Icon::Copy);
	const float top = g_urlR.y + g_urlR.h + S(8);
	g_webR = {x, top, w, std::max(S(120.0f), bottom - top)};
	if (WebFailed())
		Push(g_webR.x, g_webR.y + S(28), S(240), S(36), L"Otwórz w przeglądarce", ID_WEB_FALLBACK, kNone, true,
		     L"Okno podglądu jest niedostępne.", Icon::Globe);
}

void LayoutAbout(float x, float y, float w, float bottom) {
	(void)bottom;
	g_aboutR = {x, y, w, S(148)};
	Push(x, y + S(148) + S(16), S(230), S(36), L"Sprawdź aktualizacje", ID_UPDATE_CHECK, kNone, !UpdateBusy(),
	     L"Porównaj wersje z livedelay.vxh.pl.", Icon::None);
	const float dy = y + S(148) + S(16) + S(36) + S(18);
	g_dangerR = {x, dy, w, S(118)};
	Push(x + S(18), dy + S(64), S(230), S(36), L"Odinstaluj aplikację", ID_DANGER_PICK, kRed, true,
	     L"Zaznacz, a potem wykonaj. Ustawienia i klucze zostają.", Icon::None,
	     g_dangerOn ? BStyle::Solid : BStyle::Normal, g_dangerOn);
	Push(x + S(18) + S(238), dy + S(64), S(120), S(36), L"Wykonaj", ID_DANGER_RUN, kRed, g_dangerOn,
	     L"Zamyka program i usuwa go z menu Start.", Icon::None);
}

void LayoutLogs(float x, float y, float w, float bottom) {
	g_logR = {x, y, w, std::max(S(160), bottom - y)};
	Push(g_logR.x + g_logR.w - S(18) - S(104), g_logR.y + S(14), S(104), S(32), L"Kopiuj", ID_COPY_LOGS, kNone, true,
	     L"Skopiuj cały log do schowka.", Icon::Copy);
}

void Layout(int width, int height) {
	g_buttons.clear();

	// Our own title bar: logo and name on the left, the three glyphs on the
	// right, everything between them drags the window.
	const float topH = S(38);
	g_topR = {0, 0, (float)width, topH};
	const float wb = S(46);
	Push((float)width - wb * 3, 0, wb, topH, L"", ID_MIN, kNone, true, L"", Icon::WinMin, BStyle::Win);
	Push((float)width - wb * 2, 0, wb, topH, L"", ID_MAX, kNone, true, L"", Icon::WinMax, BStyle::Win);
	Push((float)width - wb, 0, wb, topH, L"", ID_CLOSE, kNone, true, L"Schowaj do zegara. Program działa dalej.",
	     Icon::WinClose, BStyle::Win);

	Approach(g_navWidth, S(g_navMin ? 64.0f : 234.0f), 0.1f);
	const float navW = g_navWidth;
	g_navR = {0, topH, navW, (float)height - topH};
	g_footR = {navW, (float)height - S(31), (float)width - navW, S(31)};
	g_mainR = {navW, topH, (float)width - navW, g_footR.y - topH};

	LayoutNav((float)height);

	const float px = g_mainR.x + S(24);
	const float pw = std::max(S(200), g_mainR.w - S(48));
	g_titleY = g_mainR.y + S(16) + S(20);

	Col ignored = kPillOffline;
	const std::wstring chip = PillText(ignored);
	const float chipW = TextW(chip, Fnt::Bold) + S(46);
	g_chipR = {px + pw - chipW, g_mainR.y + S(16), chipW, S(40)};

	// A new view slides up into place rather than appearing.
	Approach(g_viewSlide, 0.0f, 0.09f);
	float top = g_mainR.y + S(16) + S(40) + S(18) + g_viewSlide * S(16);
	const float bottom = g_footR.y - S(16);
	g_bannerR = {};
	if (UpdatePending()) {
		const float bh = S(52);
		g_bannerR = {px, top, pw, bh};
		const float bw = S(112);
		const float by = top + (bh - S(32)) * 0.5f;
		const bool busy = UpdateBusy();
		Push(px + pw - S(8) - bw * 2 - S(8), by, bw, S(32), UpdateActionLabel(), ID_UPDATE_GO, kAmber, !busy,
		     L"Pobierz i uruchom ponownie. Transmisja z tego PC zostanie przerwana.", Icon::None, BStyle::Solid);
		Push(px + pw - S(8) - bw, by, bw, S(32), L"Później", ID_UPDATE_LATER, kNone, !busy,
		     L"Schowaj ten komunikat do następnej wersji.", Icon::None);
		top += bh + S(12);
	}
	switch (g_view) {
	case View::Addresses:
		LayoutAddresses(px, top, pw, bottom);
		break;
	case View::Widgets:
		LayoutWidgets(px, top, pw, bottom);
		break;
	case View::Logs:
		LayoutLogs(px, top, pw, bottom);
		break;
	case View::About:
		LayoutAbout(px, top, pw, bottom);
		break;
	default:
		LayoutBoard(px, top, pw, bottom);
		break;
	}

	if (g_view == View::Widgets && !WebFailed()) {
		RECT r{(LONG)std::lround(g_webR.x), (LONG)std::lround(g_webR.y), (LONG)std::lround(g_webR.x + g_webR.w),
		       (LONG)std::lround(g_webR.y + g_webR.h)};
		WebShow(g_hwnd, WebUrl(), r);
	} else {
		WebHide();
	}
}

// ---------------------------------------------------------------- painting

void Card(const Rect &r, bool hot = false) {
	FillRound(r.x, r.y, r.w, r.h, S(14), kCard);
	// A hairline of light along the top edge, as on the reference tiles.
	FillRound(r.x + S(14), r.y, r.w - S(28), S(1), 0, Alpha(kText, 0.045f));
	StrokeRound(r.x, r.y, r.w, r.h, S(14), hot ? Mix(kLine, kAccent, 0.35f) : kLine);
}

void DrawButton(const Button &b, float hot, bool down) {
	const float cy = b.y + b.h * 0.5f;
	const float iconSize = S(17);

	if (b.style == BStyle::Win) {
		if (hot > 0.01f)
			Fill(b.x, b.y, b.w, b.h, Alpha(b.id == ID_CLOSE ? kRed : kFaceHot, hot));
		const float s = S(13);
		DrawIcon(b.icon, b.x + (b.w - s) * 0.5f, cy - s * 0.5f, s, Mix(kMuted, kText, hot));
		return;
	}

	if (b.style == BStyle::Nav) {
		// The rail itself is drawn once, sliding, in Paint.
		const float lift = std::max(hot * 0.8f, b.on ? 1.0f : 0.0f);
		if (lift > 0.01f)
			FillRound(b.x, b.y, b.w, b.h, S(9), Alpha(kFaceHot, lift));
		const Col fg = Mix(kMuted, kText, lift);
		const Col ic = b.on ? kAccent : fg;
		if (g_navMin) {
			DrawIcon(b.icon, b.x + (b.w - iconSize) * 0.5f, cy - iconSize * 0.5f, iconSize, ic);
			return;
		}
		DrawIcon(b.icon, b.x + S(12), cy - iconSize * 0.5f, iconSize, ic);
		if (!b.label.empty())
			TextMid(b.x + S(12) + iconSize + S(12), cy, b.label, fg, Fnt::Nav);
		return;
	}

	if (b.style == BStyle::Tile) {
		// The card lifts under the cursor instead of only changing colour.
		const float rise = hot * S(2);
		Card({b.x, b.y - rise, b.w, b.h}, hot > 0.4f);
		DrawIcon(b.icon, b.x + S(20), b.y - rise + S(20), S(22), Mix(kMuted, kAccent, 0.35f + hot * 0.65f));
		TextMid(b.x + S(20) + S(22) + S(12), b.y - rise + S(31), b.label, kText, Fnt::Bold);
		if (!b.note.empty())
			Text(b.x + S(20), b.y - rise + S(62), b.note, kMuted, Fnt::Small);
		return;
	}

	const float r = S(9);
	Col fill, border, label = b.enabled ? kText : kMuted;
	Col iconCol = b.enabled ? (b.accent.a > 0 ? b.accent : kText) : kMuted;
	if (!b.enabled) {
		fill = kNone;
		border = Alpha(kGrey, 0.3f);
	} else if (b.style == BStyle::Solid) {
		// The action the state is waiting for: solid, dark ink, impossible to miss.
		fill = down ? Mix(b.accent, kInk, 0.18f) : Mix(b.accent, kText, hot * 0.14f);
		border = fill;
		label = kInk;
		iconCol = kInk;
	} else if (b.accent.a > 0) {
		fill = Alpha(b.accent, down ? 0.34f : 0.11f + hot * 0.15f);
		border = Alpha(b.accent, 0.35f + hot * 0.25f);
	} else {
		fill = down ? kFaceHot : Mix(kFace, kFaceHot, hot);
		border = Mix(kLine, kLine2, hot);
	}
	FillRound(b.x, b.y, b.w, b.h, r, fill);
	StrokeRound(b.x, b.y, b.w, b.h, r, border);

	const Fnt f = b.style == BStyle::Solid ? Fnt::Bold : Fnt::Bold;
	const float labelW = TextW(b.label, f);
	if (b.icon == Icon::None || labelW + S(16) + S(8) > b.w - S(16)) {
		TextCenter(b.x + b.w * 0.5f, cy, b.label, label, f);
		return;
	}
	const float ix = b.x + (b.w - labelW - S(16) - S(8)) * 0.5f;
	DrawIcon(b.icon, ix, cy - S(8), S(16), iconCol);
	TextMid(ix + S(16) + S(8), cy, b.label, label, f);
}

void DrawSparkline(const std::wstring &key, float x, float cy, float w, float h, Col c) {
	auto it = g_history.find(key);
	if (it == g_history.end() || it->second.size() < 2)
		return;
	const std::deque<double> &v = it->second;
	double peak = 0;
	for (double s : v)
		peak = std::max(peak, s);
	if (peak <= 0) // a flat line of zeroes says nothing
		return;
	const double max = std::max(1000.0, peak);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(std::max(1.0f, S(1.6f)));
	Use(c);
	glBegin(GL_LINE_STRIP);
	for (size_t i = 0; i < v.size(); ++i) {
		float px = x + w * (float)(i + kHistory - v.size()) / (kHistory - 1);
		float py = cy + h * 0.5f - (float)(v[i] / (max * 1.1) * h);
		glVertex2f(px, py);
	}
	glEnd();
	glLineWidth(1.0f);
}

void PaintBoard() {
	// ---- state ----
	Card(g_stateR);
	const Col state = StateColor(StateClass());
	FillRound(g_stateR.x + S(10), g_stateR.y + S(16), S(4), g_stateR.h - S(32), S(2), Alpha(state, Pulse()));
	Col ignored = kPillOffline;
	Text(g_stateR.x + S(30), g_stateR.y + S(24), PillText(ignored), kText, Fnt::Title);
	const std::wstring detail = DetailText();
	if (!detail.empty())
		Text(g_stateR.x + S(30), g_stateR.y + S(56), detail, kMuted, Fnt::Small);

	std::wstring caption;
	const std::wstring big = BigNumber(caption);
	if (!big.empty()) {
		const float rx = g_stateR.x + g_stateR.w - S(30);
		TextRight(rx, g_stateR.y + S(44), big, state, Fnt::Num);
		TextRight(rx, g_stateR.y + S(74), caption, kMuted, Fnt::Grp);
	}
	const float target = g_armDelay > 0 ? (float)std::clamp(g_bufferS / g_armDelay, 0.0, 1.0) : 0.0f;
	Approach(g_bufferShown, target, 0.25f);
	if (g_showBar) {
		FillRound(g_barR.x, g_barR.y, g_barR.w, g_barR.h, S(3), kFace);
		if (g_bufferShown > 0.004f)
			FillRound(g_barR.x, g_barR.y, g_barR.w * g_bufferShown, g_barR.h, S(3), kAmber);
	}

	// ---- controls ----
	Card(g_ctrlR);
	Text(g_ctrlR.x + S(18), g_ctrlR.y + S(16), L"OPÓŹNIENIE", kMuted, Fnt::Grp);
	FillRound(g_delayBoxR.x, g_delayBoxR.y, g_delayBoxR.w, g_delayBoxR.h, S(9), kFace);
	StrokeRound(g_delayBoxR.x, g_delayBoxR.y, g_delayBoxR.w, g_delayBoxR.h, S(9), kLine);
	TextCenter(g_delayBoxR.x + g_delayBoxR.w * 0.5f, g_delayBoxR.y + g_delayBoxR.h * 0.5f,
		   std::to_wstring(g_delay) + L" s", kText, Fnt::Pill);

	// ---- platforms ----
	Card(g_destsR);
	Text(g_destsR.x + S(18), g_destsR.y + S(16), L"PLATFORMY", kMuted, Fnt::Grp);

	const float rowH = S(32);
	const float dotX = g_destsR.x + S(22);
	const float nameX = g_destsR.x + S(38);
	const float right = g_destsR.x + g_destsR.w - S(18);
	const float sparkW = S(64);
	const float stateRight = right - S(80) - S(12);

	float cy = g_destsR.y + S(44) + rowH * 0.5f;
	// The incoming feed is not a state, so it stays colourless.
	FillDot(dotX, cy, S(4), g_input ? kMuted : kDim);
	TextMid(nameX, cy, L"Z twojego PC", kText);
	const std::wstring inRate = g_input ? Round(g_inKbps) + L" kb/s" : L"brak sygnału";
	TextRight(stateRight, cy, inRate, kMuted, g_input ? Fnt::Mono : Fnt::Small);
	DrawSparkline(kInputKey, stateRight - TextW(inRate, g_input ? Fnt::Mono : Fnt::Small) - S(12) - sparkW, cy, sparkW,
		      S(16), kMuted);

	if (g_dests.empty()) {
		TextMid(nameX, cy + rowH, g_connected ? L"Brak platform: dodaj je w Ustawieniach" : L"—", kMuted, Fnt::Small);
		return;
	}
	for (size_t i = 0; i < g_dests.size(); ++i) {
		const Dest &d = g_dests[i];
		cy = g_destsR.y + S(44) + rowH * (i + 1) + rowH * 0.5f;
		if (cy + rowH * 0.5f > g_destsR.y + g_destsR.h)
			break;
		const Col col = DestColor(d);
		FillDot(dotX, cy, S(4), col);
		TextMid(nameX, cy, d.name, kText);
		std::wstring st = DestState(d);
		if (d.kbps > 0)
			st += L" · " + Round(d.kbps) + L" kb/s";
		TextRight(stateRight, cy, st, kMuted, Fnt::Small);
		DrawSparkline(d.name, stateRight - TextW(st, Fnt::Small) - S(12) - sparkW, cy, sparkW, S(16), col);
	}
}

void PaintAddresses() {
	Card(g_addrR);
	Text(g_addrR.x + S(18), g_addrR.y + S(16), L"DLA PROGRAMU NA TYM PC I ŹRÓDŁA PRZEGLĄDARKI", kMuted, Fnt::Grp);
	Text(g_addrR.x + S(18), g_addrR.y + S(34), L"Ustawienia → Transmisja → własny serwer. Klucz: ld0.", kMuted, Fnt::Small);

	struct Row {
		const wchar_t *label;
		std::wstring value;
		Col colour;
		const wchar_t *note;
	};
	const Row rows[3] = {
		{L"Adres serwera", RtmpUrl(), kGreen, L"wpisz w polu Serwer"},
		{L"Klucz wejściowy", g_ingest, kText, L"to nie jest klucz Twitcha"},
		{L"Adres overlaya", OverlayUrl(), kAccent, L"źródło Przeglądarka"},
	};
	for (int i = 0; i < 3; ++i) {
		const float ry = g_addrR.y + S(56) + S(kAddrRowH) * i;
		if (i)
			FillRound(g_addrR.x + S(18), ry - S(4), g_addrR.w - S(36), S(1), 0, kLine);
		Text(g_addrR.x + S(18), ry + S(8), rows[i].label, kMuted, Fnt::Small);
		Text(g_addrR.x + S(18), ry + S(26), rows[i].value, rows[i].colour, Fnt::Body);
		TextRight(g_addrR.x + g_addrR.w - S(18) - S(104) - S(14), ry + S(26) + S(2), rows[i].note, kMuted, Fnt::Small);
	}
}

void PaintLogs(int windowH) {
	Card(g_logR);
	Text(g_logR.x + S(18), g_logR.y + S(16), L"SILNIK I APLIKACJA", kMuted, Fnt::Grp);
	Text(g_logR.x + S(18), g_logR.y + S(34), L"livedelay.dll i to okno. Klucze platform są wycinane.", kMuted, Fnt::Small);

	const Rect clip = {g_logR.x + S(18), g_logR.y + S(62), g_logR.w - S(36), g_logR.h - S(80)};
	const float lh = S(18);
	const int visible = std::max(1, (int)(clip.h / lh));
	const int total = (int)g_logView.size();
	const int maxScroll = std::max(0, total - visible);
	if (g_logStick)
		g_logScroll = maxScroll;
	if (g_logScroll < 0)
		g_logScroll = 0;
	if (g_logScroll >= maxScroll) {
		g_logScroll = maxScroll;
		g_logStick = true;
	}

	int sx = (int)clip.x;
	int sy = windowH - (int)(clip.y + clip.h);
	int sw = (int)clip.w;
	int sh = (int)clip.h;
	if (sx < 0)
		sx = 0;
	if (sy < 0)
		sy = 0;
	if (sw < 0)
		sw = 0;
	if (sh < 0)
		sh = 0;
	glEnable(GL_SCISSOR_TEST);
	glScissor(sx, sy, sw, sh);
	if (total == 0) {
		Text(clip.x, clip.y, L"Brak wpisów w tej sesji.", kMuted, Fnt::Small);
	} else {
		for (int i = 0; i < visible && g_logScroll + i < total; ++i) {
			std::wstring shown = g_logView[g_logScroll + i];
			if (shown.size() > 220)
				shown.resize(220);
			Text(clip.x, clip.y + lh * i, shown, kText, Fnt::Mono);
		}
	}
	glDisable(GL_SCISSOR_TEST);
}

void Paint() {
	RECT rc;
	GetClientRect(g_hwnd, &rc);
	int w = std::max(1L, rc.right);
	int h = std::max(1L, rc.bottom);

	const double now = GetTickCount64() / 1000.0;
	g_dt = (float)std::clamp(now - g_now, 0.0, 0.1);
	g_now = now;
	g_animating = false;

	Layout(w, h);

	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, w, h, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(kBg.r, kBg.g, kBg.b, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	// ---- our own title bar ----
	Fill(g_topR.x, g_topR.y, g_topR.w, g_topR.h, kPanel);
	Fill(0, g_topR.h - 1, (float)w, 1, kLine);
	const float logo = S(20);
	DrawLogo(S(14), (g_topR.h - logo) * 0.5f, logo);
	TextMid(S(14) + logo + S(10), g_topR.h * 0.5f, L"LiveDelay", kText, Fnt::Bold);
	TextMid(S(14) + logo + S(10) + TextW(L"LiveDelay", Fnt::Bold) + S(10), g_topR.h * 0.5f, AppVersion(), kDim, Fnt::Mono);

	// ---- sidebar ----
	Fill(g_navR.x, g_navR.y, g_navR.w, g_navR.h, kPanel);
	Fill(g_navR.w - 1, g_navR.y, 1, g_navR.h, kLine);
	if (!g_navMin)
		Text(S(24), g_grpY, L"W PRZEGLĄDARCE", kMuted, Fnt::Grp);

	// The rail slides to whichever entry is open instead of jumping.
	for (const Button &b : g_buttons) {
		if (b.style != BStyle::Nav || !b.on)
			continue;
		Approach(g_railY, b.y + S(10), 0.075f);
		Approach(g_railH, b.h - S(20), 0.075f);
		FillRound(b.x, g_railY, S(3), g_railH, S(1.5f), kAccent);
		break;
	}

	// ---- title row ----
	Text(g_mainR.x + S(24), g_titleY - S(14), ViewTitle(), kText, Fnt::Title);
	Col ignored = kPillOffline;
	const std::wstring chip = PillText(ignored);
	FillRound(g_chipR.x, g_chipR.y, g_chipR.w, g_chipR.h, S(10), kCard);
	StrokeRound(g_chipR.x, g_chipR.y, g_chipR.w, g_chipR.h, S(10), kLine);
	FillDot(g_chipR.x + S(17), g_chipR.y + g_chipR.h * 0.5f, S(4), Alpha(StateColor(StateClass()), Pulse()));
	TextMid(g_chipR.x + S(30), g_chipR.y + g_chipR.h * 0.5f, chip, kText, Fnt::Bold);

	if (g_bannerR.h > 0) {
		FillRound(g_bannerR.x, g_bannerR.y, g_bannerR.w, g_bannerR.h, S(10), Mix(kCard, kAmber, 0.22f));
		StrokeRound(g_bannerR.x, g_bannerR.y, g_bannerR.w, g_bannerR.h, S(10), Mix(kLine, kAmber, 0.7f));
		std::wstring banner = UpdateBannerText();
		const float maxW = g_bannerR.w - S(24) - S(112) * 2 - S(16);
		while (banner.size() > 8 && TextW(banner, Fnt::Body) > maxW)
			banner.pop_back();
		TextMid(g_bannerR.x + S(16), g_bannerR.y + g_bannerR.h * 0.5f, banner, kText, Fnt::Body);
	}

	switch (g_view) {
	case View::Addresses:
		PaintAddresses();
		break;
	case View::Widgets:
		FillRound(g_urlR.x, g_urlR.y, g_urlR.w, g_urlR.h, S(8), kCard);
		StrokeRound(g_urlR.x, g_urlR.y, g_urlR.w, g_urlR.h, S(8), kLine);
		{
			std::wstring url = PublicWebUrl();
			const float maxW = g_urlR.w - S(120);
			while (url.size() > 12 && TextW(url, Fnt::Small) > maxW) {
				url.pop_back();
				if (url.size() > 1)
					url.back() = L'…';
			}
			TextMid(g_urlR.x + S(12), g_urlR.y + g_urlR.h * 0.5f, url, kText, Fnt::Small);
		}
		if (WebFailed())
			Text(g_webR.x, g_webR.y, L"Podgląd w oknie jest niedostępny.", kMuted, Fnt::Small);
		break;
	case View::Logs:
		PaintLogs(h);
		break;
	case View::About:
		Card(g_aboutR);
		{
			const float x = g_aboutR.x + S(22);
			float y = g_aboutR.y + S(28);
			Text(x, y, L"Aplikacja", kMuted, Fnt::Body);
			Text(x + S(170), y, AppVersion(), kText, Fnt::Bold);
			y += S(36);
			Text(x, y, L"Wtyczka", kMuted, Fnt::Body);
			Text(x + S(170), y, PluginVersion(), kText, Fnt::Bold);
			y += S(40);
			Text(x, y, UpdateStatus(), kMuted, Fnt::Small);
		}
		Card(g_dangerR);
		Text(g_dangerR.x + S(18), g_dangerR.y + S(16), L"Strefa niebezpieczna", kText, Fnt::Bold);
		Text(g_dangerR.x + S(18), g_dangerR.y + S(40), L"Usuwa program. Ustawienia i klucze zostają.", kMuted, Fnt::Small);
		break;
	default:
		PaintBoard();
		break;
	}

	for (size_t i = 0; i < g_buttons.size(); ++i) {
		float &amt = g_hoverAmt[g_buttons[i].id];
		Approach(amt, (int)i == g_hover && g_buttons[i].enabled ? 1.0f : 0.0f, 0.055f);
		DrawButton(g_buttons[i], amt, (int)i == g_pressed);
	}

	// ---- status bar ----
	Fill(g_footR.x, g_footR.y, g_footR.w, g_footR.h, kPanel);
	Fill(g_footR.x, g_footR.y, g_footR.w, 1, kLine);
	const float fy = g_footR.y + g_footR.h * 0.5f;
	std::wstring left = L"Silnik " + g_api;
	if (g_hover >= 0 && g_hover < (int)g_buttons.size() && *g_buttons[g_hover].tip)
		left = g_buttons[g_hover].tip;
	TextMid(g_footR.x + S(24), fy, left, kMuted, Fnt::Small);

	std::wstring rightText;
	if (!g_connected)
		rightText = L"brak połączenia";
	else if (g_armDelay > 0)
		rightText = L"bufor " + Tenth(g_bufferS) + L" s · " + Round(g_bufferMb) + L" MB";
	else if (g_input)
		rightText = Round(g_inKbps) + L" kb/s";
	TextRight(g_footR.x + g_footR.w - S(24), fy, rightText, kMuted, Fnt::Small);

	SwapBuffers(g_dc);
}

// ---------------------------------------------------------------- input

int Hit(int x, int y) {
	for (size_t i = 0; i < g_buttons.size(); ++i) {
		const Button &b = g_buttons[i];
		if (x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h)
			return (int)i;
	}
	return -1;
}

void Sample() {
	ULONGLONG now = GetTickCount64();
	if (now - g_lastSample < 900)
		return;
	g_lastSample = now;
	auto push = [](const std::wstring &key, double v) {
		std::deque<double> &h = g_history[key];
		h.push_back(v);
		if (h.size() > kHistory)
			h.pop_front();
	};
	push(kInputKey, g_input ? g_inKbps : 0);
	for (const Dest &d : g_dests)
		push(d.name, d.state == "live" ? d.kbps : 0);
}

void OpenSettings() {
	if (ShowSettings(g_hwnd)) {
		g_delaySet = false; // the ini may carry a new default
		Refresh();
	}
	InvalidateRect(g_hwnd, nullptr, FALSE);
}

void OpenInApp(WebPage page) {
	if (g_view != View::Widgets)
		g_viewSlide = 1.0f;
	g_view = View::Widgets;
	g_webPage = page;
	ShowWindow(g_hwnd, SW_SHOW);
	SetForegroundWindow(g_hwnd);
	InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DoButton(int id) {
	if (id >= ID_NAV && id < ID_NAV + (int)View::About + 1) {
		if (g_view != (View)(id - ID_NAV))
			g_viewSlide = 1.0f;
		g_view = (View)(id - ID_NAV);
		ShowWindow(g_hwnd, SW_SHOW);
		InvalidateRect(g_hwnd, nullptr, FALSE);
		return;
	}
	switch (id) {
	case ID_MIN:
		ShowWindow(g_hwnd, SW_MINIMIZE);
		return;
	case ID_MAX:
		ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
		return;
	case ID_CLOSE:
		ShowWindow(g_hwnd, SW_HIDE);
		return;
	default:
		break;
	}
	if (id == ID_NAV_TOGGLE) {
		g_navMin = !g_navMin;
		InvalidateRect(g_hwnd, nullptr, FALSE);
		return;
	}
	if (id >= ID_DEST) {
		size_t i = (size_t)(id - ID_DEST);
		if (i < g_dests.size()) {
			std::wstring path = g_dests[i].on ? L"/dest/stop?i=" : L"/dest/start?i=";
			PostAction(path + std::to_wstring(i));
		}
		InvalidateRect(g_hwnd, nullptr, FALSE);
		return;
	}
	switch (id) {
	case ID_ARM:
		PostAction(L"/arm");
		break;
	case ID_ACTIVATE:
		PostAction(L"/activate");
		break;
	case ID_CUT:
		PostAction(L"/cut");
		break;
	case ID_CUT_AFTER:
		PostAction(L"/cut-after");
		break;
	case ID_DISARM:
		PostAction(L"/disarm");
		break;
	case ID_DELAY_DOWN:
		g_delay = std::max(1, g_delay - 1);
		PostAction(L"/settings");
		break;
	case ID_DELAY_UP:
		g_delay = std::min(3600, g_delay + 1);
		PostAction(L"/settings");
		break;
	case ID_COPY_RTMP:
		Copy(g_hwnd, RtmpUrl());
		break;
	case ID_COPY_OVERLAY:
	case ID_COPY_WIDGET:
		Copy(g_hwnd, g_view == View::Widgets && id == ID_COPY_WIDGET ? PublicWebUrl() : OverlayUrl());
		break;
	case ID_COPY_INGEST:
		Copy(g_hwnd, g_ingest);
		break;
	case ID_COPY_LOGS: {
		std::wstring all;
		for (const std::wstring &line : g_logView) {
			all += line;
			all += L"\r\n";
		}
		Copy(g_hwnd, all.empty() ? L"" : all);
		break;
	}
	case ID_PREVIEW:
		OpenInApp(WebPage::Preview);
		break;
	case ID_OVERLAY:
		OpenInApp(WebPage::Overlay);
		break;
	case ID_THEME:
		OpenInApp(WebPage::Theme);
		break;
	case ID_WEB_FALLBACK:
		OpenUrl(WebUrl());
		break;
	case ID_HELP:
		OpenUrl(kHelpUrl);
		break;
	case ID_PANEL:
		OpenUrl(PanelUrl());
		break;
	case ID_SETTINGS:
		OpenSettings();
		break;
	case ID_UPDATE_GO:
		UpdateApply(g_hwnd);
		break;
	case ID_UPDATE_LATER:
		UpdateLater();
		break;
	case ID_UPDATE_CHECK:
		if (g_view != View::About)
			g_viewSlide = 1.0f;
		g_view = View::About;
		UpdateCheckNow();
		break;
	case ID_DANGER_PICK:
		g_dangerOn = !g_dangerOn;
		break;
	case ID_DANGER_RUN:
		if (g_dangerOn)
			UninstallApp(g_hwnd);
		break;
	default:
		break;
	}
	InvalidateRect(g_hwnd, nullptr, FALSE);
}

// The tray menu is drawn by hand, so it matches the window instead of the
// system's light popup. Kinds: 0 item, 1 info line, 2 separator, 3 submenu.
struct MenuItem {
	std::wstring text;
	int kind = 0;
};

std::vector<MenuItem *> g_menuItems;
HFONT g_menuFont;
HFONT g_menuBold;
HBRUSH g_menuBrush;

UINT_PTR MenuData(const std::wstring &text, int kind) {
	g_menuItems.push_back(new MenuItem{text, kind});
	return (UINT_PTR)g_menuItems.back();
}

void MenuAdd(HMENU m, int id, const std::wstring &text, bool enabled, int kind = 0) {
	UINT flags = MF_OWNERDRAW | (enabled ? MF_ENABLED : (MF_GRAYED | MF_DISABLED));
	AppendMenuW(m, flags, id, (LPCWSTR)MenuData(text, kind));
}

void MenuSep(HMENU m) {
	AppendMenuW(m, MF_OWNERDRAW | MF_GRAYED | MF_DISABLED, 0, (LPCWSTR)MenuData(L"", 2));
}

void MenuFonts() {
	if (g_menuFont)
		return;
	NONCLIENTMETRICSW ncm{};
	ncm.cbSize = sizeof ncm;
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
	g_menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
	ncm.lfMenuFont.lfWeight = FW_SEMIBOLD;
	g_menuBold = CreateFontIndirectW(&ncm.lfMenuFont);
	g_menuBrush = CreateSolidBrush(Gdi(kCard));
}

void MeasureMenuItem(MEASUREITEMSTRUCT *mi) {
	auto *item = (MenuItem *)mi->itemData;
	if (!item)
		return;
	if (item->kind == 2) {
		mi->itemWidth = 0;
		mi->itemHeight = Si(9);
		return;
	}
	HDC dc = GetDC(g_hwnd);
	HGDIOBJ old = SelectObject(dc, g_menuFont);
	SIZE sz{};
	GetTextExtentPoint32W(dc, item->text.c_str(), (int)item->text.size(), &sz);
	SelectObject(dc, old);
	ReleaseDC(g_hwnd, dc);
	mi->itemWidth = sz.cx + Si(46);
	mi->itemHeight = std::max<UINT>(Si(27), sz.cy + Si(9));
}

void DrawMenuItem(DRAWITEMSTRUCT *d) {
	auto *item = (MenuItem *)d->itemData;
	if (!item)
		return;
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	FillRect(dc, &r, g_menuBrush);

	if (item->kind == 2) {
		RECT line{r.left + Si(14), (r.top + r.bottom) / 2, r.right - Si(14), (r.top + r.bottom) / 2 + 1};
		HBRUSH b = CreateSolidBrush(Gdi(kLine));
		FillRect(dc, &line, b);
		DeleteObject(b);
		return;
	}

	const bool grey = (d->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
	if ((d->itemState & ODS_SELECTED) && !grey) {
		RECT hot{r.left + Si(4), r.top, r.right - Si(4), r.bottom};
		HBRUSH b = CreateSolidBrush(Gdi(kFaceHot));
		FillRect(dc, &hot, b);
		DeleteObject(b);
	}

	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, (d->itemState & ODS_DEFAULT) ? g_menuBold : g_menuFont);
	SetTextColor(dc, Gdi(item->kind == 1 ? kMuted : grey ? Mix(kCard, kMuted, 0.75f) : kText));
	RECT t{r.left + Si(18), r.top, r.right - Si(18), r.bottom};
	DrawTextW(dc, item->text.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	if (item->kind == 3) { // owner-drawn items get no submenu arrow of their own
		const int cy = (r.top + r.bottom) / 2;
		const int x = r.right - Si(14);
		POINT arrow[3] = {{x - Si(4), cy - Si(4)}, {x, cy}, {x - Si(4), cy + Si(4)}};
		HBRUSH b = CreateSolidBrush(Gdi(kMuted));
		HPEN pen = CreatePen(PS_SOLID, 1, Gdi(kMuted));
		HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
		Polygon(dc, arrow, 3);
		SelectObject(dc, ob);
		SelectObject(dc, op);
		DeleteObject(b);
		DeleteObject(pen);
	}
}

void TrayMenu() {
	MenuFonts();
	Col ignored = kPillOffline;
	HMENU menu = CreatePopupMenu();
	MENUINFO mi{};
	mi.cbSize = sizeof mi;
	mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
	mi.hbrBack = g_menuBrush;
	SetMenuInfo(menu, &mi);
	MenuAdd(menu, 0, PillText(ignored), false, 1);

	std::wstring detail;
	if (!g_connected)
		detail = L"Silnik nie odpowiada na " + g_api;
	else if (!g_input)
		detail = L"Brak sygnału z tego PC";
	else
		detail = L"Wejście " + Round(g_inKbps) + L" kb/s";
	if (g_connected && g_armDelay > 0)
		detail += L"   ·   bufor " + Tenth(g_bufferS) + L"/" + Round(g_armDelay) + L" s · " + Round(g_bufferMb) + L" MB";
	if (g_cutPending)
		detail += L"   ·   powrót na żywo czeka na widzów";
	MenuAdd(menu, 0, detail, false, 1);
	MenuSep(menu);

	MenuAdd(menu, ID_ARM, L"Uzbrój (" + std::to_wstring(g_delay) + L" s)", g_connected && !g_delayed);
	MenuAdd(menu, ID_ACTIVATE, L"Włącz opóźnienie", g_connected && !g_delayed && g_armed);
	MenuAdd(menu, ID_CUT, L"Na żywo", g_connected && g_delayed);
	MenuAdd(menu, ID_CUT_AFTER, L"Na żywo po emisji", g_connected && g_delayed && !g_cutPending);
	MenuAdd(menu, ID_DISARM, L"Rozbrój", g_connected && !g_delayed && g_armDelay > 0);

	if (!g_dests.empty()) {
		HMENU sub = CreatePopupMenu();
		SetMenuInfo(sub, &mi);
		for (size_t i = 0; i < g_dests.size(); ++i) {
			const Dest &d = g_dests[i];
			std::wstring line = d.name + L"  —  " + DestState(d);
			if (d.kbps > 0)
				line += L" · " + Round(d.kbps) + L" kb/s";
			line += d.on ? L"   (zakończ)" : L"   (start)";
			MenuAdd(sub, ID_DEST + (int)i, line, g_connected);
		}
		AppendMenuW(menu, MF_OWNERDRAW | MF_POPUP, (UINT_PTR)sub, (LPCWSTR)MenuData(L"Platformy", 3));
	}

	MenuSep(menu);
	MenuAdd(menu, ID_OVERLAY, L"Overlay", true);
	MenuAdd(menu, ID_COPY_OVERLAY, L"Kopiuj adres overlaya", true);
	MenuAdd(menu, ID_PREVIEW, L"Podgląd", true);
	MenuAdd(menu, ID_THEME, L"Edytor motywu", true);
	MenuAdd(menu, ID_PANEL, L"Webpanel", true);
	MenuSep(menu);
	MenuAdd(menu, ID_SHOW, L"Pokaż okno", true);
	MenuAdd(menu, ID_NAV + (int)View::About, L"O aplikacji", true);
	MenuAdd(menu, ID_SETTINGS, L"Ustawienia…", true);
	MenuAdd(menu, ID_QUIT, L"Zakończ", true);
	SetMenuDefaultItem(menu, ID_SHOW, FALSE);
	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(g_hwnd);
	int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
	DestroyMenu(menu); // takes the submenu with it
	for (MenuItem *item : g_menuItems)
		delete item;
	g_menuItems.clear();
	if (cmd == ID_SHOW) {
		ShowWindow(g_hwnd, SW_SHOW);
		SetForegroundWindow(g_hwnd);
	} else if (cmd == ID_QUIT) {
		g_quit = true;
		DestroyWindow(g_hwnd);
	} else if (cmd)
		DoButton(cmd);
}

void AddTray() {
	g_logo = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_LOGO), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
	g_tray.cbSize = sizeof g_tray;
	g_tray.hWnd = g_hwnd;
	g_tray.uID = 1;
	g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	g_tray.uCallbackMessage = WM_TRAY;
	g_tray.hIcon = g_logo ? g_logo : LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_LOGO));
	wcscpy_s(g_tray.szTip, L"LiveDelay");
	Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void TrayTip() {
	Col ignored = kPillOffline;
	std::wstring tip = L"LiveDelay: " + PillText(ignored);
	g_tray.uFlags = NIF_TIP;
	wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
	Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void MakeFonts() {
	for (HFONT &f : g_fonts) {
		if (f)
			DeleteObject(f);
		f = nullptr;
	}
	auto face = [](float dp, int weight, const wchar_t *name, int pitch) {
		return CreateFontW(-Si(dp), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				   CLEARTYPE_QUALITY, pitch, name);
	};
	auto font = [&](float dp, int weight) { return face(dp, weight, L"Segoe UI", DEFAULT_PITCH | FF_DONTCARE); };
	// The site sets its figures in JetBrains Mono; Consolas is the one Windows ships.
	auto mono = [&](float dp, int weight) { return face(dp, weight, L"Consolas", FIXED_PITCH | FF_MODERN); };
	g_fonts[(int)Fnt::Body] = font(14.5f, FW_NORMAL);
	g_fonts[(int)Fnt::Bold] = font(14.5f, FW_SEMIBOLD);
	g_fonts[(int)Fnt::Small] = font(12.5f, FW_NORMAL);
	g_fonts[(int)Fnt::Pill] = font(17.0f, FW_SEMIBOLD);
	g_fonts[(int)Fnt::Grp] = font(10.5f, FW_BOLD);
	g_fonts[(int)Fnt::Title] = font(18.0f, FW_SEMIBOLD);
	g_fonts[(int)Fnt::Nav] = font(13.5f, FW_NORMAL);
	g_fonts[(int)Fnt::Num] = mono(30.0f, FW_SEMIBOLD);
	g_fonts[(int)Fnt::Mono] = mono(12.5f, FW_NORMAL);
	// Section labels are letterspaced, like the ones in the reference shell.
	g_fontTrack[(int)Fnt::Grp] = std::max(1, Si(1.6f));
}

FILETIME g_iniStamp{};
bool g_iniWatch = false;
std::wstring g_hotReport;

std::wstring TrimPart(std::wstring s) {
	size_t a = s.find_first_not_of(L" \t");
	if (a == std::wstring::npos)
		return {};
	size_t b = s.find_last_not_of(L" \t");
	s = s.substr(a, b - a + 1);
	for (wchar_t &c : s)
		c = (wchar_t)towlower(c);
	return s;
}

bool ParseHotkey(const std::wstring &spec, UINT &mods, UINT &vk) {
	mods = MOD_NOREPEAT;
	bool have = false;
	size_t i = 0;
	while (i <= spec.size()) {
		size_t plus = spec.find(L'+', i);
		std::wstring part = TrimPart(spec.substr(i, plus == std::wstring::npos ? std::wstring::npos : plus - i));
		if (part.empty())
			return false;
		if (part == L"ctrl" || part == L"control")
			mods |= MOD_CONTROL;
		else if (part == L"alt")
			mods |= MOD_ALT;
		else if (part == L"shift")
			mods |= MOD_SHIFT;
		else if (part == L"win")
			mods |= MOD_WIN;
		else {
			if (have)
				return false;
			have = true;
			if (part.rfind(L"num", 0) == 0) {
				std::wstring n = part.substr(3);
				if (n == L"+" || n == L"add" || n == L"plus")
					vk = VK_ADD;
				else if (n == L"-" || n == L"sub" || n == L"minus")
					vk = VK_SUBTRACT;
				else if (n == L"*" || n == L"mul")
					vk = VK_MULTIPLY;
				else if (n == L"/" || n == L"div")
					vk = VK_DIVIDE;
				else if (n == L"." || n == L"dot" || n == L"del")
					vk = VK_DECIMAL;
				else if (n.size() == 1 && n[0] >= L'0' && n[0] <= L'9')
					vk = VK_NUMPAD0 + (n[0] - L'0');
				else
					return false;
			} else if (part.size() >= 2 && part[0] == L'f' && part[1] >= L'0' && part[1] <= L'9') {
				int n = _wtoi(part.c_str() + 1);
				if (n < 1 || n > 24)
					return false;
				vk = VK_F1 + (n - 1);
			} else if (part.size() == 1 && iswalnum(part[0]))
				vk = (UINT)towupper(part[0]);
			else if (part == L"pause")
				vk = VK_PAUSE;
			else if (part == L"scrolllock")
				vk = VK_SCROLL;
			else if (part == L"insert")
				vk = VK_INSERT;
			else if (part == L"home")
				vk = VK_HOME;
			else if (part == L"end")
				vk = VK_END;
			else if (part == L"pageup")
				vk = VK_PRIOR;
			else if (part == L"pagedown")
				vk = VK_NEXT;
			else
				return false;
		}
		if (plus == std::wstring::npos)
			break;
		i = plus + 1;
	}
	return have;
}

void RegisterIniHotkeys() {
	UnregisterHotKey(g_hwnd, HOTKEY_ARM);
	UnregisterHotKey(g_hwnd, HOTKEY_ACTIVATE);
	UnregisterHotKey(g_hwnd, HOTKEY_CUT);
	UnregisterHotKey(g_hwnd, HOTKEY_CUT_AFTER);
	const char *keys[] = {"hotkey_arm", "hotkey_activate", "hotkey_cut", "hotkey_cut_after"};
	const int ids[] = {HOTKEY_ARM, HOTKEY_ACTIVATE, HOTKEY_CUT, HOTKEY_CUT_AFTER};
	const wchar_t *names[] = {L"Uzbrój", L"Włącz opóźnienie", L"Na żywo", L"Na żywo po emisji"};
	std::string text = LoadText(g_ini);
	g_hotReport.clear();
	for (int i = 0; i < 4; ++i) {
		std::wstring combo = IniLine(text, keys[i]);
		if (combo.empty()) {
			g_hotReport += std::wstring(names[i]) + L": nie ustawiony\r\n";
			continue;
		}
		UINT mods = 0, vk = 0;
		if (!ParseHotkey(combo, mods, vk)) {
			g_hotReport += std::wstring(names[i]) + L": nie rozumiem „" + combo + L"”\r\n";
			continue;
		}
		if (!RegisterHotKey(g_hwnd, ids[i], mods, vk))
			g_hotReport += std::wstring(names[i]) + L": " + combo + L" — zajęty przez inny program\r\n";
		else
			g_hotReport += std::wstring(names[i]) + L": " + combo + L" — działa\r\n";
	}
}

std::string g_shape;

std::string ShapeOfIni() {
	std::string text = LoadText(g_ini), shape;
	for (size_t i = 0; i < text.size();) {
		size_t end = text.find('\n', i);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(i, end - i);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const char *keys[] = {"listen ", "api ", "[dest", "encode_mode", "enhanced_broadcasting", "audio_track", "vod_track"};
		for (const char *k : keys) {
			if (line.rfind(k, 0) == 0) {
				shape += line;
				shape.push_back('\n');
				break;
			}
		}
		i = end + 1;
	}
	return shape;
}

void TouchIniStampFile() {
	g_shape = ShapeOfIni();
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &info))
		g_iniStamp = info.ftLastWriteTime;
}

void WatchIni() {
	if (!g_iniWatch)
		return;
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (!GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &info))
		return;
	if (CompareFileTime(&info.ftLastWriteTime, &g_iniStamp) == 0)
		return;
	std::string now = ShapeOfIni();
	bool structural = now != g_shape;
	std::wstring listen = g_listen, api = g_api;
	ReadIniHints();
	TouchIniStampFile();
	if (structural || listen != g_listen || api != g_api)
		ld::ReloadEngine();
	ld::ApplyHotkeys();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_TRAY:
		if (lp == WM_LBUTTONUP) {
			ShowWindow(hwnd, SW_SHOW);
			SetForegroundWindow(hwnd);
		} else if (lp == WM_RBUTTONUP)
			TrayMenu();
		return 0;
	case WM_HOTKEY:
		if (wp == HOTKEY_ARM)
			DoButton(ID_ARM);
		else if (wp == HOTKEY_ACTIVATE)
			DoButton(ID_ACTIVATE);
		else if (wp == HOTKEY_CUT)
			DoButton(ID_CUT);
		else if (wp == HOTKEY_CUT_AFTER)
			DoButton(ID_CUT_AFTER);
		return 0;
	case WM_LD_UPDATE:
		if (wp == 1)
			DestroyWindow(hwnd);
		else
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_TIMER:
		if (wp == 2) { // the frame clock: redraw, nothing else
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		UpdatePoll();
		WatchIni();
		Refresh();
		Sample();
		TrayTip();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDOWN: {
		int i = Hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		g_pressed = (i >= 0 && g_buttons[i].enabled) ? i : -1;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_LBUTTONUP: {
		int i = Hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		int was = g_pressed;
		g_pressed = -1;
		if (i >= 0 && i == was && g_buttons[i].enabled)
			DoButton(g_buttons[i].id);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_MOUSEWHEEL:
		if (g_view == View::Logs) {
			g_logStick = false;
			g_logScroll -= GET_WHEEL_DELTA_WPARAM(wp) / 40;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_MOUSEMOVE: {
		if (!g_tracking) {
			TRACKMOUSEEVENT tme{};
			tme.cbSize = sizeof tme;
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = hwnd;
			TrackMouseEvent(&tme);
			g_tracking = true;
		}
		int i = Hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		if (i != g_hover) {
			g_hover = i;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		g_tracking = false;
		g_hover = -1;
		g_pressed = -1;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) {
			bool hot = g_hover >= 0 && g_hover < (int)g_buttons.size() && g_buttons[g_hover].enabled;
			SetCursor(hot ? g_hand : g_arrow);
			return TRUE;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_GETMINMAXINFO: {
		auto *mm = (MINMAXINFO *)lp;
		mm->ptMinTrackSize.x = Si(760);
		mm->ptMinTrackSize.y = Si(540);
		return 0;
	}
	case WM_DPICHANGED: {
		g_scale = std::max(1.0f, LOWORD(wp) / 96.0f);
		MakeFonts();
		ClearTextCache();
		const RECT *r = (const RECT *)lp;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_MEASUREITEM:
		if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
			MeasureMenuItem((MEASUREITEMSTRUCT *)lp);
			return TRUE;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_DRAWITEM:
		if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
			DrawMenuItem((DRAWITEMSTRUCT *)lp);
			return TRUE;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_NCACTIVATE:
		// lParam -1 keeps DefWindowProc from repainting the system frame.
		SuppressSystemFrame(hwnd);
		return DefWindowProcW(hwnd, WM_NCACTIVATE, wp, -1);
	case WM_NCPAINT:
		return 0;
	case WM_ACTIVATE:
		SuppressSystemFrame(hwnd);
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_NCCALCSIZE:
		// The client area covers the whole window, caption included: the title
		// bar above is ours. The resize frame survives through WM_NCHITTEST.
		if (wp) {
			auto *p = (NCCALCSIZE_PARAMS *)lp;
			if (IsZoomed(hwnd)) {
				const int frame = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
				p->rgrc[0].left += frame;
				p->rgrc[0].right -= frame;
				p->rgrc[0].top += frame;
				p->rgrc[0].bottom -= frame;
			}
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_NCHITTEST: {
		POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		RECT r;
		GetWindowRect(hwnd, &r);
		const int b = Si(6);
		if (!IsZoomed(hwnd)) {
			const bool left = pt.x < r.left + b, right = pt.x >= r.right - b;
			const bool top = pt.y < r.top + b, low = pt.y >= r.bottom - b;
			if (top && left)
				return HTTOPLEFT;
			if (top && right)
				return HTTOPRIGHT;
			if (low && left)
				return HTBOTTOMLEFT;
			if (low && right)
				return HTBOTTOMRIGHT;
			if (top)
				return HTTOP;
			if (low)
				return HTBOTTOM;
			if (left)
				return HTLEFT;
			if (right)
				return HTRIGHT;
		}
		ScreenToClient(hwnd, &pt);
		// Anything in the title bar that is not one of our buttons drags the window.
		if (pt.y < g_topR.h && Hit(pt.x, pt.y) < 0)
			return HTCAPTION;
		return HTCLIENT;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		Paint();
		EndPaint(hwnd, &ps);
		// The frame timer only runs while something is still moving.
		if (g_animating != g_frameTimer) {
			if (g_animating)
				SetTimer(hwnd, 2, 16, nullptr);
			else
				KillTimer(hwnd, 2);
			g_frameTimer = g_animating;
		}
		return 0;
	}
	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		UnregisterHotKey(hwnd, HOTKEY_ARM);
		UnregisterHotKey(hwnd, HOTKEY_ACTIVATE);
		UnregisterHotKey(hwnd, HOTKEY_CUT);
		UnregisterHotKey(hwnd, HOTKEY_CUT_AFTER);
		Shell_NotifyIconW(NIM_DELETE, &g_tray);
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

} // namespace

namespace ld {

// The settings window calls this after a change the running engine cannot take
// over HTTP: a platform added or removed, a new ingest key, a new address.
bool ReloadEngine() {
	if (!g_start || !g_stop)
		return false;
	g_stop();
	std::string ini = WideUtf8(g_ini);
	if (g_start(ini.c_str()) != 0) {
		char msg[512] = {};
		if (g_lastErr)
			g_lastErr(msg, sizeof msg);
		AppLog(L"nie udało się przeładować silnika");
		MessageBoxW(g_hwnd, Utf8(msg[0] ? msg : "Nie udało się uruchomić silnika z nowymi ustawieniami.").c_str(),
			    L"LiveDelay", MB_OK | MB_ICONWARNING);
		return false;
	}
	AppLog(L"przeładowano silnik");
	g_delaySet = false;
	Refresh();
	InvalidateRect(g_hwnd, nullptr, FALSE);
	TouchIniStampFile();
	return true;
}

void ApplyHotkeys() {
	RegisterIniHotkeys();
}

std::wstring HotkeyReport() {
	return g_hotReport;
}

void TouchIniStamp() {
	TouchIniStampFile();
}

} // namespace ld

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
	g_inst = inst;
	SetProcessDPIAware();
	wchar_t appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
		return 1;
	std::wstring dir = std::wstring(appdata) + L"\\LiveDelay";
	CreateDirectoryW(dir.c_str(), nullptr);
	SetIniPath(dir + L"\\livedelay.ini");
	AppLog(L"aplikacja uruchomiona");

	EngineLoad engine = LoadEngine();
	if (engine == EngineLoad::Missing) {
		MessageBoxW(nullptr,
			    L"Nie ma livedelay.dll obok programu.\n\n"
			    L"Zbuduj silnik w repozytorium wtyczki: cargo build --release\n"
			    L"Potem uruchom build.bat tutaj — skopiuje DLL obok exe.",
			    L"LiveDelay", MB_OK | MB_ICONWARNING);
		return 1;
	}
	if (engine != EngineLoad::Ok)
		return 1;
	ReadIniHints();

	g_arrow = LoadCursorW(nullptr, IDC_ARROW);
	g_hand = LoadCursorW(nullptr, IDC_HAND);

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"LiveDelayApp";
	wc.hCursor = g_arrow;
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LOGO));
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassW(&wc);

	HDC screen = GetDC(nullptr);
	g_scale = std::max(1.0f, GetDeviceCaps(screen, LOGPIXELSX) / 96.0f);
	ReleaseDC(nullptr, screen);

	// No system caption: WM_NCCALCSIZE hands the whole window to us, while
	// WS_THICKFRAME keeps resizing, snapping and the drop shadow.
	const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
	// WS_POPUP alone would drop the window off the taskbar.
	g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"LiveDelay", style, CW_USEDEFAULT, CW_USEDEFAULT,
				 Si(920), Si(620), nullptr, nullptr, inst, nullptr);
	SuppressSystemFrame(g_hwnd);

	g_dc = GetDC(g_hwnd);
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	int format = ChoosePixelFormat(g_dc, &pfd);
	SetPixelFormat(g_dc, format, &pfd);
	g_gl = wglCreateContext(g_dc);
	wglMakeCurrent(g_dc, g_gl);
	MakeFonts();

	AddTray();
	UpdateBind(g_hwnd);
	ShowWindow(g_hwnd, SW_SHOW);
	Refresh();
	Sample();
	SetTimer(g_hwnd, 1, 500, nullptr);
	ApplyHotkeys();
	TouchIniStamp();
	g_iniWatch = true;

	MSG msg;
	while (!g_quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (g_stop)
		g_stop();
	ClearTextCache();
	if (g_gl) {
		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(g_gl);
	}
	for (HFONT f : g_fonts)
		if (f)
			DeleteObject(f);
	if (g_dll)
		FreeLibrary(g_dll);
	return 0;
}

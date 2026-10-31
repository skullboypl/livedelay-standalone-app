#include "settings.hpp"

#include "core.hpp"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace ld {
namespace {

// ---------------------------------------------------------------- ids

enum {
	IDC_TAB = 100, // +0..3
	IDC_DELAY = 110,
	IDC_BUFFER,
	IDC_LISTEN,
	IDC_API,
	IDC_LIST = 120,
	IDC_ADD,
	IDC_DEL,
	IDC_NAME = 130,
	IDC_URL,
	IDC_KEY,
	IDC_SHOWKEY,
	IDC_INGEST,
	IDC_AUTOSTART,
	IDC_SERVICE,
	IDC_SERVER,
	IDC_OVURL = 140,
	IDC_OVCOPY,
	IDC_OVEDITOR,
	IDC_OVARMED,
	IDC_OVTEXT = 150, // +0..3
	IDC_OVSHOW = 160, // +0..3
	IDC_HOTKEY = 170, // +0..3
	IDC_HOTCAP = 180, // +0..3
	IDC_SAVE = 190,
	IDC_CANCEL,
	IDC_OPENINI,
	IDC_CLOSEWIN,
};

// Stored in each button's GWLP_USERDATA so WM_DRAWITEM knows what to paint.
enum class Kind { Push = 1, Accent, Check, Tab, Select, Nav, Close };

// The shell: our own title bar across the top, the pages down the left.
constexpr int kWinW = 820;
constexpr int kWinH = 560;
constexpr int kTopH = 38;
constexpr int kNavW = 180;
constexpr int kPadX = 204; // where the content column starts
constexpr int kRight = 796;

// ---------------------------------------------------------------- platforms
//
// Typical ingests, so nobody has to hunt for an address. The platform's own
// dashboard always wins: that is what "Własny adres" is for.

struct Ingest {
	const wchar_t *label;
	const wchar_t *url;
};

const Ingest kTwitchIn[] = {
	{L"Automatyczny", L"rtmp://live.twitch.tv/app"},
	{L"Warszawa", L"rtmp://waw.contribute.live-video.net/app"},
	{L"Praga", L"rtmp://prg.contribute.live-video.net/app"},
	{L"Frankfurt", L"rtmp://fra.contribute.live-video.net/app"},
	{L"Amsterdam", L"rtmp://ams.contribute.live-video.net/app"},
	{L"Londyn", L"rtmp://lhr.contribute.live-video.net/app"},
	{L"Nowy Jork", L"rtmp://iad.contribute.live-video.net/app"},
	{L"Chicago", L"rtmp://ord.contribute.live-video.net/app"},
	{L"San Francisco", L"rtmp://sfo.contribute.live-video.net/app"},
};
const Ingest kYouTubeIn[] = {
	{L"Główny", L"rtmp://a.rtmp.youtube.com/live2"},
	{L"Zapasowy", L"rtmp://b.rtmp.youtube.com/live2?backup=1"},
	{L"Główny przez RTMPS", L"rtmps://a.rtmps.youtube.com/live2"},
};
const Ingest kKickIn[] = {
	{L"Główny", L"rtmps://fa723fc1b171.global-contribute.live-video.net/app"},
};
const Ingest kFacebookIn[] = {
	{L"Główny", L"rtmps://live-api-s.facebook.com:443/rtmp/"},
};
const Ingest kTrovoIn[] = {
	{L"Główny", L"rtmp://livepush.trovo.live/live/"},
};

struct Service {
	const wchar_t *name;
	const Ingest *ingests;
	int count;
	/// Recognises addresses this list does not carry — Twitch alone has dozens
	/// of numbered ingests, and they come and go.
	const wchar_t *domain;
};

const Service kServices[] = {
	{L"Twitch", kTwitchIn, (int)std::size(kTwitchIn), L"contribute.live-video.net"},
	{L"YouTube", kYouTubeIn, (int)std::size(kYouTubeIn), L"youtube.com"},
	{L"Kick", kKickIn, (int)std::size(kKickIn), L"global-contribute.live-video.net"},
	{L"Facebook", kFacebookIn, (int)std::size(kFacebookIn), L"facebook.com"},
	{L"Trovo", kTrovoIn, (int)std::size(kTrovoIn), L"trovo.live"},
};
constexpr int kServiceCount = (int)std::size(kServices);

/// The host part of rtmp://host:port/app.
std::wstring HostOf(const std::wstring &url) {
	size_t start = url.find(L"://");
	if (start == std::wstring::npos)
		return {};
	start += 3;
	size_t end = url.find_first_of(L":/", start);
	return url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

constexpr int kPages = 4;
const char *const kHotkeyKey[4] = {"hotkey_arm", "hotkey_activate", "hotkey_cut", "hotkey_cut_after"};
const wchar_t *const kHotkeyLabel[4] = {L"Uzbrój", L"Włącz opóźnienie", L"Na żywo", L"Na żywo po emisji"};
constexpr int kStates = 4;
const wchar_t *const kStateLabel[kStates] = {L"Na żywo", L"Uzbrajanie", L"Uzbrojone", L"Opóźnienie"};
const char *const kStateKey[kStates] = {"live", "arming", "armed", "delayed"};

// ---------------------------------------------------------------- state

struct DestRow {
	std::wstring name, url, key, ingest;
	bool autostart = true;
	/// Index of this platform's block in the ini as it was read; -1 when new.
	int origin = -1;
	/// The [dest NAME] it was read under; empty for a platform added here.
	std::wstring fileName;
};

struct Snapshot {
	int delay = 15;
	int buffer = 512;
	std::wstring listen, api;
	std::vector<DestRow> rows;
	std::string overlay;
};

HWND g_wnd;
HWND g_hint; // the "fits ~N seconds" line under the buffer limit
HWND g_hotStatus = nullptr;
WNDPROC g_hotEditProc = nullptr;
std::wstring g_hotPrev[4];

bool IsModifier(UINT vk) {
	switch (vk) {
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
	case VK_MENU: case VK_LMENU: case VK_RMENU:
	case VK_LWIN: case VK_RWIN:
		return true;
	default:
		return false;
	}
}

std::wstring TokenOf(UINT vk) {
	if (vk >= VK_F1 && vk <= VK_F24)
		return L"F" + std::to_wstring(vk - VK_F1 + 1);
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
		return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
	switch (vk) {
	case VK_ADD: return L"Num+";
	case VK_SUBTRACT: return L"Num-";
	case VK_MULTIPLY: return L"Num*";
	case VK_DIVIDE: return L"Num/";
	case VK_DECIMAL: return L"Num.";
	case VK_PAUSE: return L"Pause";
	case VK_SCROLL: return L"ScrollLock";
	case VK_INSERT: return L"Insert";
	case VK_HOME: return L"Home";
	case VK_END: return L"End";
	case VK_PRIOR: return L"PageUp";
	case VK_NEXT: return L"PageDown";
	default: break;
	}
	if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
		return std::wstring(1, (wchar_t)vk);
	return L"";
}

LRESULT CALLBACK HotEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	int id = GetDlgCtrlID(hwnd);
	int idx = (id >= IDC_HOTKEY && id < IDC_HOTKEY + 4) ? id - IDC_HOTKEY : -1;
	if (idx < 0 || !g_hotEditProc)
		return DefWindowProcW(hwnd, msg, wp, lp);
	if (msg == WM_SETFOCUS) {
		wchar_t buf[160] = {};
		GetWindowTextW(hwnd, buf, 160);
		if (wcscmp(buf, L"Naciśnij skrót…") != 0)
			g_hotPrev[idx] = buf;
		SetWindowTextW(hwnd, L"Naciśnij skrót…");
	} else if (msg == WM_KILLFOCUS) {
		wchar_t buf[160] = {};
		GetWindowTextW(hwnd, buf, 160);
		if (wcscmp(buf, L"Naciśnij skrót…") == 0)
			SetWindowTextW(hwnd, g_hotPrev[idx].c_str());
	} else if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
		UINT vk = (UINT)wp;
		if (IsModifier(vk))
			return 0;
		if (vk == VK_ESCAPE) {
			SetWindowTextW(hwnd, g_hotPrev[idx].c_str());
			return 0;
		}
		if (vk == VK_BACK || vk == VK_DELETE) {
			g_hotPrev[idx].clear();
			SetWindowTextW(hwnd, L"");
			return 0;
		}
		std::wstring token = TokenOf(vk);
		if (!token.empty()) {
			std::wstring spec;
			auto add = [&](int vkey, const wchar_t *name) {
				if (GetKeyState(vkey) & 0x8000) {
					if (!spec.empty())
						spec += L"+";
					spec += name;
				}
			};
			add(VK_CONTROL, L"Ctrl");
			add(VK_MENU, L"Alt");
			add(VK_SHIFT, L"Shift");
			if (GetKeyState(VK_LWIN) & 0x8000)
				add(VK_LWIN, L"Win");
			else
				add(VK_RWIN, L"Win");
			if (!spec.empty())
				spec += L"+";
			spec += token;
			g_hotPrev[idx] = spec;
			SetWindowTextW(hwnd, spec.c_str());
		}
		return 0;
	} else if (msg == WM_CHAR || msg == WM_SYSCHAR)
		return 0;
	return CallWindowProcW(g_hotEditProc, hwnd, msg, wp, lp);
}

HFONT g_font;
HFONT g_bold;
HFONT g_title;
HBRUSH g_brBg;
HBRUSH g_brFace;
int g_page = 0;
int g_sel = -1;
bool g_saved = false;
bool g_keyShown = false;
double g_inKbps = 0;

std::string g_iniText;
FILETIME g_iniWhen{};   // the file's write time when these fields were filled
bool g_dirty = false;   // the user has touched something in this window
bool g_outsideNoticed = false;
bool g_filling = false; // fields are being set from the file, not by the user
HWND g_sync;            // the line that reports an outside change
std::string g_overlay;
std::vector<DestRow> g_rows;
Snapshot g_orig;

struct Ctl {
	HWND h;
	int page; // -1: visible on every page
};

std::vector<Ctl> g_ctls;
std::vector<HWND> g_framed; // controls that get a drawn outline
std::unordered_map<int, bool> g_checked;
HWND g_hot;

HWND Of(int id) {
	return GetDlgItem(g_wnd, id);
}

std::wstring Get(int id) {
	HWND h = Of(id);
	int n = GetWindowTextLengthW(h);
	std::wstring s(n + 1, 0);
	GetWindowTextW(h, s.data(), n + 1);
	s.resize(n);
	return s;
}

void Set(int id, const std::wstring &text) {
	SetWindowTextW(Of(id), text.c_str());
}

bool Checked(int id) {
	auto it = g_checked.find(id);
	return it != g_checked.end() && it->second;
}

void SetChecked(int id, bool on) {
	g_checked[id] = on;
	if (HWND h = Of(id))
		InvalidateRect(h, nullptr, TRUE);
}

std::wstring Trim(const std::wstring &s) {
	size_t a = s.find_first_not_of(L" \t");
	if (a == std::wstring::npos)
		return {};
	size_t b = s.find_last_not_of(L" \t");
	return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------- ini as blocks

struct Block {
	std::string header; // "[dest Twitch]"; empty for the lines before the first platform
	std::vector<std::string> lines;
};

std::string TrimAscii(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

/// "url = rtmp://…" → key "url", value "rtmp://…". False for comments and junk.
bool SplitKey(const std::string &line, std::string &key, std::string &value) {
	std::string t = TrimAscii(line);
	if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[')
		return false;
	size_t eq = t.find('=');
	if (eq == std::string::npos)
		return false;
	key = TrimAscii(t.substr(0, eq));
	value = TrimAscii(t.substr(eq + 1));
	return !key.empty();
}

std::vector<Block> SplitIni(const std::string &text) {
	std::vector<Block> blocks(1);
	size_t p = 0;
	while (p <= text.size()) {
		size_t nl = text.find('\n', p);
		std::string line = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		std::string t = TrimAscii(line);
		if (t.rfind("[dest", 0) == 0 && t.back() == ']')
			blocks.push_back({line, {}});
		else if (!(nl == std::string::npos && t.empty()))
			blocks.back().lines.push_back(line);
		if (nl == std::string::npos)
			break;
		p = nl + 1;
	}
	return blocks;
}

std::string JoinIni(const std::vector<Block> &blocks, const char *eol) {
	std::string out;
	for (const Block &b : blocks) {
		if (!b.header.empty()) {
			out += b.header;
			out += eol;
		}
		for (const std::string &l : b.lines) {
			out += l;
			out += eol;
		}
	}
	return out;
}

/// Replaces the first `key = …` line, keeping its indentation; appends when missing.
void SetLineKey(std::vector<std::string> &lines, const char *key, const std::string &value) {
	std::string k, v;
	for (std::string &line : lines) {
		if (SplitKey(line, k, v) && k == key) {
			line = std::string(key) + " = " + value;
			return;
		}
	}
	// Keep the trailing blank line of a block below the new key.
	size_t at = lines.size();
	while (at > 0 && TrimAscii(lines[at - 1]).empty())
		--at;
	lines.insert(lines.begin() + at, std::string(key) + " = " + value);
}

std::string BlockValue(const Block &b, const char *key) {
	std::string k, v;
	for (const std::string &line : b.lines)
		if (SplitKey(line, k, v) && k == key)
			return v;
	return {};
}

bool Yes(const std::string &v) {
	std::string t = v;
	std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return t == "yes" || t == "tak" || t == "true" || t == "1" || t == "on";
}

std::string DestName(const std::string &header) {
	std::string t = TrimAscii(header);
	if (t.size() < 7)
		return {};
	return TrimAscii(t.substr(5, t.size() - 6)); // between "[dest" and "]"
}

// ---------------------------------------------------------------- overlay document

std::string JsonEsc(const std::string &s) {
	std::string out;
	for (char c : s) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out.push_back(c);
		}
	}
	return out;
}

/// Span of the object that `key` holds, as [begin, end) just inside its braces.
bool ObjectSpan(const std::string &j, const char *key, size_t &begin, size_t &end) {
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat);
	if (p == std::string::npos)
		return false;
	p = j.find('{', p + pat.size());
	if (p == std::string::npos)
		return false;
	int depth = 0;
	bool inStr = false;
	for (size_t i = p; i < j.size(); ++i) {
		char c = j[i];
		if (inStr) {
			if (c == '\\')
				++i;
			else if (c == '"')
				inStr = false;
			continue;
		}
		if (c == '"')
			inStr = true;
		else if (c == '{')
			++depth;
		else if (c == '}' && --depth == 0) {
			begin = p + 1;
			end = i;
			return true;
		}
	}
	return false;
}

/// End of the value that starts at `p` (a string, number, bool or null).
size_t ValueEnd(const std::string &j, size_t p) {
	if (p < j.size() && j[p] == '"') {
		for (size_t i = p + 1; i < j.size(); ++i) {
			if (j[i] == '\\')
				++i;
			else if (j[i] == '"')
				return i + 1;
		}
		return j.size();
	}
	size_t i = p;
	while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != ']')
		++i;
	return i;
}

/// Sets `key` inside the object `obj` (nullptr = the document itself) to an
/// already-encoded JSON value, leaving everything else in the document alone.
std::string SetJson(std::string j, const char *obj, const char *key, const std::string &value) {
	size_t begin = 0, end = j.size();
	if (obj) {
		if (!ObjectSpan(j, obj, begin, end)) {
			size_t open = j.find('{');
			if (open == std::string::npos)
				return j;
			j.insert(open + 1, std::string("\"") + obj + "\":{\"" + key + "\":" + value + "},");
			return j;
		}
	} else {
		size_t open = j.find('{');
		if (open == std::string::npos)
			return j;
		begin = open + 1;
		end = j.size();
	}
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat, begin);
	if (p == std::string::npos || p >= end) {
		j.insert(begin, pat + value + ",");
		return j;
	}
	size_t vs = p + pat.size();
	j.replace(vs, ValueEnd(j, vs) - vs, value);
	return j;
}

std::string JsonIn(const std::string &j, const char *obj, const char *key) {
	size_t begin = 0, end = 0;
	if (!ObjectSpan(j, obj, begin, end))
		return {};
	std::string inner = j.substr(begin, end - begin);
	return JsonString(inner, key);
}

bool JsonBoolIn(const std::string &j, const char *obj, const char *key, bool def) {
	size_t begin = 0, end = 0;
	if (!ObjectSpan(j, obj, begin, end))
		return def;
	std::string inner = j.substr(begin, end - begin);
	std::string pat = std::string("\"") + key + "\":";
	size_t p = inner.find(pat);
	if (p == std::string::npos)
		return def;
	return inner.compare(p + pat.size(), 4, "true") == 0;
}

// ---------------------------------------------------------------- controls

HWND Make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, int page) {
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, Si((float)x), Si((float)y), Si((float)w), Si((float)h), g_wnd,
				 (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrW(g_wnd, GWLP_HINSTANCE), nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
	g_ctls.push_back({c, page});
	return c;
}

HWND MakeLabel(const wchar_t *text, int x, int y, int w, int page, bool muted = false, bool bold = false) {
	HWND c = Make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, 20, -1, page);
	SetWindowLongPtrW(c, GWLP_USERDATA, muted ? 1 : 0);
	if (bold)
		SendMessageW(c, WM_SETFONT, (WPARAM)g_bold, TRUE);
	return c;
}

HWND MakeWrapLabel(const wchar_t *text, int x, int y, int w, int h, int page) {
	HWND c = Make(L"STATIC", text, SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, x, y, w, h, -1, page);
	SetWindowLongPtrW(c, GWLP_USERDATA, 1);
	return c;
}

HWND MakeEdit(int x, int y, int w, int id, int page, DWORD extra = 0) {
	HWND c = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | extra, x, y, w, 28, id, page);
	SendMessageW(c, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Si(8), Si(8)));
	g_framed.push_back(c);
	return c;
}

HWND MakeButton(const wchar_t *text, int x, int y, int w, int id, int page, Kind kind, int h = 30) {
	HWND c = Make(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id, page);
	SetWindowLongPtrW(c, GWLP_USERDATA, (LONG_PTR)kind);
	return c;
}

// A dark stand-in for a combo box: a button that opens an owner-drawn menu.
struct PickItem {
	std::wstring text;
};

std::vector<PickItem *> g_pickItems;

/// Opens a menu under `anchor` and returns the chosen index, or -1.
int Pick(HWND anchor, const std::vector<std::wstring> &items, int current) {
	HMENU menu = CreatePopupMenu();
	MENUINFO mi{};
	mi.cbSize = sizeof mi;
	mi.fMask = MIM_BACKGROUND;
	mi.hbrBack = g_brFace;
	SetMenuInfo(menu, &mi);
	for (size_t i = 0; i < items.size(); ++i) {
		g_pickItems.push_back(new PickItem{items[i]});
		AppendMenuW(menu, MF_OWNERDRAW, i + 1, (LPCWSTR)g_pickItems.back());
	}
	if (current >= 0 && current < (int)items.size())
		CheckMenuRadioItem(menu, 0, (UINT)items.size() - 1, current, MF_BYPOSITION);
	RECT r;
	GetWindowRect(anchor, &r);
	SetForegroundWindow(g_wnd);
	const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, r.left, r.bottom + Si(2), 0, g_wnd, nullptr);
	DestroyMenu(menu);
	for (PickItem *p : g_pickItems)
		delete p;
	g_pickItems.clear();
	return cmd > 0 ? cmd - 1 : -1;
}

HWND MakeCheck(const wchar_t *text, int x, int y, int w, int id, int page, bool on) {
	HWND c = MakeButton(text, x, y, w, id, page, Kind::Check, 24);
	g_checked[id] = on;
	return c;
}

Kind KindOf(HWND h) {
	return (Kind)GetWindowLongPtrW(h, GWLP_USERDATA);
}

void ShowPage(int page) {
	g_page = page;
	for (const Ctl &c : g_ctls)
		ShowWindow(c.h, c.page < 0 || c.page == page ? SW_SHOW : SW_HIDE);
	for (int i = 0; i < kPages; ++i)
		InvalidateRect(Of(IDC_TAB + i), nullptr, TRUE);
	InvalidateRect(g_wnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- drawing

void RoundFrame(HDC dc, RECT r, int radius, Col fill, Col border) {
	HBRUSH br = fill.a > 0 ? CreateSolidBrush(Gdi(fill)) : (HBRUSH)GetStockObject(NULL_BRUSH);
	HPEN pen = CreatePen(PS_SOLID, 1, Gdi(border));
	HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
	RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	if (fill.a > 0)
		DeleteObject(br);
	DeleteObject(pen);
}

void DrawCheckGlyph(HDC dc, RECT box, bool on) {
	RoundFrame(dc, box, Si(5), on ? Mix(kBg, kAccent, 0.75f) : kFace, on ? kAccent : kLine);
	if (!on)
		return;
	HPEN pen = CreatePen(PS_SOLID, std::max(2, Si(2)), Gdi(kBg));
	HGDIOBJ op = SelectObject(dc, pen);
	const int w = box.right - box.left, h = box.bottom - box.top;
	POINT pts[3] = {{box.left + w / 4, box.top + h / 2},
			{box.left + w / 2 - Si(1), box.bottom - h / 3},
			{box.right - w / 4, box.top + h / 3}};
	Polyline(dc, pts, 3);
	SelectObject(dc, op);
	DeleteObject(pen);
}

void DrawButtonItem(DRAWITEMSTRUCT *d) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	const bool down = (d->itemState & ODS_SELECTED) != 0;
	const bool hot = d->hwndItem == g_hot;
	const bool off = (d->itemState & ODS_DISABLED) != 0;
	const Kind kind = KindOf(d->hwndItem);
	wchar_t text[256] = {};
	GetWindowTextW(d->hwndItem, text, 255);

	FillRect(dc, &r, g_brBg);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, kind == Kind::Tab ? g_bold : g_font);

	if (kind == Kind::Tab) {
		const int index = (int)(GetDlgCtrlID(d->hwndItem) - IDC_TAB);
		const bool on = index == g_page;
		SetTextColor(dc, Gdi(on ? kText : hot ? kText : kMuted));
		DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		if (on) {
			RECT line{r.left + Si(10), r.bottom - Si(3), r.right - Si(10), r.bottom - Si(1)};
			HBRUSH br = CreateSolidBrush(Gdi(kAccent));
			FillRect(dc, &line, br);
			DeleteObject(br);
		}
		return;
	}

	if (kind == Kind::Check) {
		const int side = Si(19);
		RECT box{r.left, r.top + (r.bottom - r.top - side) / 2, r.left + side, r.top + (r.bottom - r.top + side) / 2};
		DrawCheckGlyph(dc, box, Checked((int)GetDlgCtrlID(d->hwndItem)));
		RECT t{r.left + side + Si(10), r.top, r.right, r.bottom};
		SetTextColor(dc, Gdi(off ? kMuted : kText));
		DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (d->itemState & ODS_FOCUS) {
			RECT f{t.left - Si(3), r.top, r.right, r.bottom};
			DrawFocusRect(dc, &f);
		}
		return;
	}

	if (kind == Kind::Close) {
		// Owner-drawn buttons paint their whole rect or show leftovers.
		HBRUSH b = CreateSolidBrush(Gdi(hot ? kRed : kPanel));
		FillRect(dc, &r, b);
		DeleteObject(b);
		HPEN pen = CreatePen(PS_SOLID, std::max(1, Si(1.4f)), Gdi(hot ? kText : kMuted));
		HGDIOBJ op = SelectObject(dc, pen);
		const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = Si(5);
		MoveToEx(dc, cx - s, cy - s, nullptr);
		LineTo(dc, cx + s + 1, cy + s + 1);
		MoveToEx(dc, cx + s, cy - s, nullptr);
		LineTo(dc, cx - s - 1, cy + s + 1);
		SelectObject(dc, op);
		DeleteObject(pen);
		return;
	}

	if (kind == Kind::Nav) {
		const bool on = (int)(GetDlgCtrlID(d->hwndItem) - IDC_TAB) == g_page;
		HBRUSH b = CreateSolidBrush(Gdi(on || hot ? kFaceHot : kPanel));
		FillRect(dc, &r, b);
		DeleteObject(b);
		if (on) { // the rail that marks the page you are on
			HBRUSH b = CreateSolidBrush(Gdi(kGreen));
			RECT rail{r.left, r.top + Si(10), r.left + Si(3), r.bottom - Si(10)};
			FillRect(dc, &rail, b);
			DeleteObject(b);
		}
		SetBkMode(dc, TRANSPARENT);
		SelectObject(dc, on ? g_bold : g_font);
		SetTextColor(dc, Gdi(on || hot ? kText : kMuted));
		RECT t{r.left + Si(18), r.top, r.right - Si(10), r.bottom};
		DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	if (kind == Kind::Select) {
		RoundFrame(dc, r, Si(7), off ? kBg : (down || hot ? kFaceHot : kFace), kLine);
		RECT t{r.left + Si(12), r.top, r.right - Si(28), r.bottom};
		SetTextColor(dc, Gdi(off ? kMuted : kText));
		DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		const int mid = (r.top + r.bottom) / 2, cx = r.right - Si(17);
		HPEN pen = CreatePen(PS_SOLID, std::max(1, Si(1.6f)), Gdi(kMuted));
		HGDIOBJ op = SelectObject(dc, pen);
		POINT chev[3] = {{cx - Si(4), mid - Si(2)}, {cx, mid + Si(2)}, {cx + Si(4), mid - Si(2)}};
		Polyline(dc, chev, 3);
		SelectObject(dc, op);
		DeleteObject(pen);
		if (d->itemState & ODS_FOCUS) {
			RECT f = r;
			InflateRect(&f, -Si(4), -Si(4));
			DrawFocusRect(dc, &f);
		}
		return;
	}

	const Col accent = kind == Kind::Accent ? kGreen : kNone;
	Col fill = kFace, border = kLine;
	if (off) {
		fill = kBg;
		border = Mix(kBg, kGrey, 0.35f);
	} else if (accent.a > 0) {
		fill = Mix(kBg, accent, down ? 0.36f : hot ? 0.26f : 0.16f);
		border = Mix(kBg, accent, 0.5f);
	} else if (down || hot) {
		fill = kFaceHot;
	}
	RoundFrame(dc, r, Si(7), fill, border);
	SetTextColor(dc, Gdi(off ? kMuted : kText));
	DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	if (d->itemState & ODS_FOCUS) {
		RECT f = r;
		InflateRect(&f, -Si(4), -Si(4));
		DrawFocusRect(dc, &f);
	}
}

void DrawPickItem(DRAWITEMSTRUCT *d) {
	auto *item = (PickItem *)d->itemData;
	if (!item)
		return;
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	const bool on = (d->itemState & ODS_SELECTED) != 0;
	HBRUSH bg = CreateSolidBrush(Gdi(on ? kFaceHot : kFace));
	FillRect(dc, &r, bg);
	DeleteObject(bg);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font);
	SetTextColor(dc, Gdi(kText));
	RECT t{r.left + Si(14), r.top, r.right - Si(12), r.bottom};
	DrawTextW(dc, item->text.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	if (d->itemState & ODS_CHECKED) { // the one in use
		HBRUSH dot = CreateSolidBrush(Gdi(kGreen));
		RECT mark{r.left + Si(5), (r.top + r.bottom) / 2 - Si(2), r.left + Si(9), (r.top + r.bottom) / 2 + Si(2)};
		FillRect(dc, &mark, dot);
		DeleteObject(dot);
	}
}

void DrawListItem(DRAWITEMSTRUCT *d) {
	if ((int)d->itemID < 0)
		return;
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	const bool on = (d->itemState & ODS_SELECTED) != 0;
	HBRUSH br = CreateSolidBrush(Gdi(on ? Mix(kFace, kAccent, 0.28f) : kFace));
	FillRect(dc, &r, br);
	DeleteObject(br);
	wchar_t text[256] = {};
	SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font);
	SetTextColor(dc, Gdi(kText));
	RECT t{r.left + Si(10), r.top, r.right - Si(6), r.bottom};
	DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// Title bar, sidebar and the name of the page you are on.
void PaintShell(HDC dc) {
	HBRUSH panel = CreateSolidBrush(Gdi(kPanel));
	RECT top{0, 0, Si(kWinW), Si(kTopH)};
	FillRect(dc, &top, panel);
	RECT side{0, Si(kTopH), Si(kNavW), Si(kWinH)};
	FillRect(dc, &side, panel);
	DeleteObject(panel);

	HBRUSH line = CreateSolidBrush(Gdi(kLine));
	RECT under{0, Si(kTopH) - 1, Si(kWinW), Si(kTopH)};
	FillRect(dc, &under, line);
	RECT edge{Si(kNavW) - 1, Si(kTopH), Si(kNavW), Si(kWinH)};
	FillRect(dc, &edge, line);
	DeleteObject(line);

	// Resource 1 in app.rc is the site's mark.
	const int logo = Si(18);
	if (HICON ic = (HICON)LoadImageW((HINSTANCE)GetWindowLongPtrW(g_wnd, GWLP_HINSTANCE), MAKEINTRESOURCEW(1), IMAGE_ICON, logo,
					 logo, LR_DEFAULTCOLOR)) {
		DrawIconEx(dc, Si(14), (Si(kTopH) - logo) / 2, ic, logo, logo, 0, nullptr, DI_NORMAL);
		DestroyIcon(ic);
	}
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_bold);
	SetTextColor(dc, Gdi(kText));
	RECT title{Si(14) + logo + Si(10), 0, Si(400), Si(kTopH)};
	DrawTextW(dc, L"LiveDelay — ustawienia", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	const wchar_t *pages[kPages] = {L"Ogólne", L"Platformy", L"Overlay", L"Skróty"};
	SelectObject(dc, g_title);
	RECT heading{Si(kPadX), Si(kTopH) + Si(14), Si(kRight), Si(kTopH) + Si(48)};
	DrawTextW(dc, pages[g_page], -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// Edits and the list have no border of their own, so the window draws one.
void PaintFrames(HDC dc) {
	for (HWND h : g_framed) {
		if (!IsWindowVisible(h))
			continue;
		RECT r;
		GetWindowRect(h, &r);
		MapWindowPoints(nullptr, g_wnd, (POINT *)&r, 2);
		InflateRect(&r, Si(2), Si(2));
		RoundFrame(dc, r, Si(7), kNone, GetFocus() == h ? Mix(kLine, kAccent, 0.6f) : kLine);
	}
}

// ---------------------------------------------------------------- platform list

void FillList() {
	HWND list = Of(IDC_LIST);
	SendMessageW(list, LB_RESETCONTENT, 0, 0);
	for (const DestRow &d : g_rows)
		SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)(d.name.empty() ? L"(bez nazwy)" : d.name.c_str()));
	if (g_sel >= 0)
		SendMessageW(list, LB_SETCURSEL, g_sel, 0);
}

/// Which service and ingest the address belongs to, or -1 for an address of
/// one's own. The address in the field is always the truth; these only label it.
void MatchUrl(const std::wstring &url, int &service, int &server) {
	service = -1;
	server = -1;
	for (int s = 0; s < kServiceCount; ++s)
		for (int n = 0; n < kServices[s].count; ++n)
			if (url == kServices[s].ingests[n].url) {
				service = s;
				server = n;
				return;
			}
	// An ingest we do not carry, like Twitch's waw02: name the service from the
	// host. Longest domain wins, so Kick beats Twitch on the tail they share.
	const std::wstring host = HostOf(url);
	size_t best = 0;
	for (int s = 0; s < kServiceCount; ++s) {
		const std::wstring d = kServices[s].domain;
		if (host.size() >= d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0 && d.size() > best) {
			best = d.size();
			service = s;
		}
	}
}

void ShowServiceLabels() {
	int service = -1, server = -1;
	MatchUrl(Trim(Get(IDC_URL)), service, server);
	SetWindowTextW(Of(IDC_SERVICE), service < 0 ? L"Własny adres" : kServices[service].name);
	std::wstring label = L"—";
	if (service >= 0)
		label = server >= 0 ? kServices[service].ingests[server].label : HostOf(Trim(Get(IDC_URL)));
	SetWindowTextW(Of(IDC_SERVER), label.c_str());
	EnableWindow(Of(IDC_SERVER), service >= 0);
	InvalidateRect(Of(IDC_SERVICE), nullptr, TRUE);
	InvalidateRect(Of(IDC_SERVER), nullptr, TRUE);
}

void ShowRow(int i) {
	const bool wasFilling = g_filling;
	g_filling = true;
	struct Restore {
		bool prev;
		~Restore() { g_filling = prev; }
	} restore{wasFilling};
	const bool any = i >= 0 && i < (int)g_rows.size();
	for (int id : {IDC_NAME, IDC_URL, IDC_KEY, IDC_INGEST, IDC_AUTOSTART, IDC_SHOWKEY})
		EnableWindow(Of(id), any);
	if (!any) {
		for (int id : {IDC_NAME, IDC_URL, IDC_KEY, IDC_INGEST})
			Set(id, L"");
		return;
	}
	const DestRow &d = g_rows[i];
	Set(IDC_NAME, d.name);
	Set(IDC_URL, d.url);
	Set(IDC_KEY, d.key);
	Set(IDC_INGEST, d.ingest);
	SetChecked(IDC_AUTOSTART, d.autostart);
	ShowServiceLabels();
}

/// Pulls what is in the fields back into the row they belong to.
void StoreRow(int i) {
	if (i < 0 || i >= (int)g_rows.size())
		return;
	DestRow &d = g_rows[i];
	d.name = Trim(Get(IDC_NAME));
	d.url = Trim(Get(IDC_URL));
	d.key = Trim(Get(IDC_KEY));
	d.ingest = Trim(Get(IDC_INGEST));
	d.autostart = Checked(IDC_AUTOSTART);
}

void SelectRow(int i) {
	StoreRow(g_sel);
	g_sel = i;
	FillList();
	ShowRow(i);
}

// ---------------------------------------------------------------- reading

int ToInt(const std::wstring &s, int def) {
	wchar_t *end = nullptr;
	long v = wcstol(s.c_str(), &end, 10);
	return end && end != s.c_str() ? (int)v : def;
}

FILETIME IniWriteTime() {
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &info))
		return info.ftLastWriteTime;
	return FILETIME{};
}

void ReadEverything() {
	g_iniText = LoadText(g_ini);
	g_iniWhen = IniWriteTime();
	g_dirty = false;
	g_outsideNoticed = false;
	std::vector<Block> blocks = SplitIni(g_iniText);

	g_orig.delay = ToInt(IniLine(g_iniText, "delay"), 15);
	g_orig.buffer = ToInt(IniLine(g_iniText, "max_buffer_mb"), 512);
	g_orig.listen = g_listen;
	g_orig.api = g_api;

	g_rows.clear();
	for (size_t i = 1; i < blocks.size(); ++i) {
		DestRow d;
		d.name = Utf8(DestName(blocks[i].header));
		d.fileName = d.name;
		d.url = Utf8(BlockValue(blocks[i], "url"));
		d.key = Utf8(BlockValue(blocks[i], "key"));
		d.ingest = Utf8(BlockValue(blocks[i], "ingest_key"));
		std::string autostart = BlockValue(blocks[i], "autostart");
		d.autostart = autostart.empty() || Yes(autostart);
		d.origin = (int)i;
		g_rows.push_back(d);
	}
	g_orig.rows = g_rows;
	g_sel = g_rows.empty() ? -1 : 0;

	g_overlay = Http(L"GET", L"/overlay/config");
	if (g_overlay.find('{') == std::string::npos)
		g_overlay.clear();
	g_orig.overlay = g_overlay;

	std::string state = Http(L"GET", L"/state");
	g_inKbps = JsonNumber(state, "in_kbps");
}

std::wstring BufferHint() {
	const int mb = ToInt(Get(IDC_BUFFER), g_orig.buffer);
	const double kbps = g_inKbps > 100 ? g_inKbps : 6000;
	const int seconds = (int)(mb * 8000.0 / kbps);
	wchar_t buf[200];
	swprintf_s(buf, L"Starczy na około %d s przy %.0f kb/s%s.", seconds, kbps, g_inKbps > 100 ? L"" : L" (szacunek)");
	return buf;
}

// ---------------------------------------------------------------- saving

std::string BuildIni() {
	// Start from the file as it is right now, not from the copy read when this
	// window opened: everything the panel writes and this window does not show
	// — language, arm slots, encoder settings — then survives the save.
	const std::string text = LoadText(g_ini);
	std::vector<Block> blocks = SplitIni(text);
	const char *eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

	std::vector<Block> out(1);
	out[0] = blocks[0];
	SetLineKey(out[0].lines, "delay", std::to_string(ToInt(Get(IDC_DELAY), g_orig.delay)));
	SetLineKey(out[0].lines, "max_buffer_mb", std::to_string(ToInt(Get(IDC_BUFFER), g_orig.buffer)));
	SetLineKey(out[0].lines, "listen", WideUtf8(Trim(Get(IDC_LISTEN))));
	SetLineKey(out[0].lines, "api", WideUtf8(Trim(Get(IDC_API))));
	for (int i = 0; i < 4; ++i)
		SetLineKey(out[0].lines, kHotkeyKey[i], WideUtf8(Trim(Get(IDC_HOTKEY + i))));

	for (const DestRow &d : g_rows) {
		Block b;
		// Match by the section name this row was read under, so a rename here or
		// a reorder there still lands on the right block.
		if (!d.fileName.empty())
			for (size_t i = 1; i < blocks.size(); ++i)
				if (Utf8(DestName(blocks[i].header)) == d.fileName) {
					b = blocks[i];
					break;
				}
		b.header = "[dest " + WideUtf8(d.name) + "]";
		SetLineKey(b.lines, "url", WideUtf8(d.url));
		SetLineKey(b.lines, "key", WideUtf8(d.key));
		SetLineKey(b.lines, "ingest_key", WideUtf8(d.ingest));
		SetLineKey(b.lines, "autostart", d.autostart ? "yes" : "no");
		out.push_back(b);
	}
	return JoinIni(out, eol);
}

/// True when the change cannot be handed to a running engine over HTTP.
bool NeedsReload() {
	if (Trim(Get(IDC_LISTEN)) != g_orig.listen || Trim(Get(IDC_API)) != g_orig.api)
		return true;
	if (g_rows.size() != g_orig.rows.size())
		return true;
	for (size_t i = 0; i < g_rows.size(); ++i) {
		const DestRow &a = g_rows[i], &b = g_orig.rows[i];
		if (a.origin != b.origin || a.name != b.name || a.ingest != b.ingest)
			return true;
	}
	return false;
}

std::string PatchedOverlay() {
	std::string j = g_overlay;
	for (int i = 0; i < kStates; ++i) {
		j = SetJson(j, "texts", kStateKey[i], "\"" + JsonEsc(WideUtf8(Get(IDC_OVTEXT + i))) + "\"");
		j = SetJson(j, "show", kStateKey[i], Checked(IDC_OVSHOW + i) ? "true" : "false");
	}
	return SetJson(j, nullptr, "armedAsLive", Checked(IDC_OVARMED) ? "true" : "false");
}

void FillFields();

/// Someone else — the webpanel, a text editor — wrote the ini since this window
/// read it. Untouched fields just catch up; edited ones wait for the user.
void CheckOutsideChange() {
	const FILETIME now = IniWriteTime();
	if (CompareFileTime(&now, &g_iniWhen) == 0)
		return;
	if (g_dirty && g_outsideNoticed)
		return; // already said so; the save will ask
	if (!g_dirty) {
		const int page = g_page;
		ReadEverything();
		FillFields();
		ShowPage(page);
		SetWindowTextW(g_sync, L"Ustawienia zmieniły się poza tym oknem — pola odświeżone.");
		return;
	}
	g_outsideNoticed = true; // the stamp stays put, so the save has to ask
	SetWindowTextW(g_sync, L"Ktoś zmienił ustawienia poza tym oknem. Zapis zapyta, co zrobić.");
}

void Warn(const wchar_t *text) {
	MessageBoxW(g_wnd, text, L"LiveDelay – ustawienia", MB_OK | MB_ICONWARNING);
}

bool Save() {
	StoreRow(g_sel);
	for (const DestRow &d : g_rows) {
		if (d.name.empty()) {
			Warn(L"Każda platforma musi mieć nazwę.");
			return false;
		}
		if (d.name.find(L']') != std::wstring::npos) {
			Warn(L"Nazwa platformy nie może zawierać znaku ].");
			return false;
		}
	}

	// Last chance to notice that the panel wrote the file while this window was open.
	const FILETIME onDisk = IniWriteTime();
	if (CompareFileTime(&onDisk, &g_iniWhen) != 0) {
		const int answer = MessageBoxW(g_wnd,
					       L"Ustawienia zmieniły się poza tym oknem, odkąd je otworzyłeś —\n"
					       L"prawdopodobnie w panelu w przeglądarce.\n\n"
					       L"Tak — zapisz to, co widzisz tutaj (tamte zmiany w tych samych polach przepadną).\n"
					       L"Nie — porzuć moje zmiany i wczytaj ustawienia na nowo.\n"
					       L"Anuluj — wróć do okna.",
					       L"LiveDelay – ustawienia", MB_YESNOCANCEL | MB_ICONWARNING);
		if (answer == IDCANCEL)
			return false;
		if (answer == IDNO) {
			const int page = g_page;
			ReadEverything();
			FillFields();
			ShowPage(page);
			SetWindowTextW(g_sync, L"Wczytane na nowo z pliku.");
			return false;
		}
	}

	const bool reload = NeedsReload();
	if (reload && MessageBoxW(g_wnd,
				  L"Te zmiany wymagają przeładowania silnika.\n\n"
				  L"Transmisja na chwilę się urwie, a bufor opóźnienia przepadnie.\n"
				  L"Zapisać mimo to?",
				  L"LiveDelay – ustawienia", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
		return false;

	if (!g_iniText.empty() && !StoreText(g_ini + L".bak", g_iniText))
		Warn(L"Nie udało się zapisać kopii livedelay.ini.bak. Zapisuję dalej.");
	if (!StoreText(g_ini, BuildIni())) {
		Warn(L"Nie udało się zapisać livedelay.ini.");
		return false;
	}

	if (!g_overlay.empty()) {
		std::string patched = PatchedOverlay();
		if (patched != g_orig.overlay && Http(L"POST", L"/overlay/config", patched).find("\"ok\"") == std::string::npos)
			Warn(L"Overlay zapisany w pliku, ale silnik go nie przyjął.");
	}

	if (reload) {
		ReadIniHints();
		if (!ReloadEngine())
			return true; // ReloadEngine already said what went wrong
	} else {
		const int delay = ToInt(Get(IDC_DELAY), g_orig.delay);
		const int buffer = ToInt(Get(IDC_BUFFER), g_orig.buffer);
		if (delay != g_orig.delay || buffer != g_orig.buffer)
			Http(L"POST", L"/settings?delay=" + std::to_wstring(delay) + L"&max_buffer_mb=" + std::to_wstring(buffer));
		for (size_t i = 0; i < g_rows.size(); ++i) {
			const DestRow &a = g_rows[i], &b = g_orig.rows[i];
			if (a.url == b.url && a.key == b.key)
				continue;
			std::string body = "{\"url\":\"" + JsonEsc(WideUtf8(a.url)) + "\",\"key\":\"" + JsonEsc(WideUtf8(a.key)) + "\"}";
			std::string answer = Http(L"POST", L"/dest/target?i=" + std::to_wstring(i), body);
			// The engine answers a refusal with {"error":"…"} and a success with the
			// whole state, which carries an empty "error" per platform — so the state
			// document is what tells the two apart, not the word "error".
			if (answer.find("\"mode\"") == std::string::npos)
				Warn((L"Silnik odrzucił adres platformy " + a.name + L". Zapisany w pliku, wejdzie po restarcie.").c_str());
		}
		ReadIniHints();
	}
	ApplyHotkeys();
	AppLog(L"zapisano ustawienia");
	TouchIniStamp();
	g_iniWhen = IniWriteTime();
	g_dirty = false;
	std::wstring report = HotkeyReport();
	if (report.find(L"zajęty") != std::wstring::npos || report.find(L"nie rozumiem") != std::wstring::npos)
		Warn(report.c_str());
	return true;
}

// ---------------------------------------------------------------- building the window

void Build() {
	// ---- the shell: title bar, then the pages down the left ----
	MakeButton(L"", kWinW - 46, 0, 46, IDC_CLOSEWIN, -1, Kind::Close, kTopH);
	const wchar_t *pages[kPages] = {L"Ogólne", L"Platformy", L"Overlay", L"Skróty"};
	for (int i = 0; i < kPages; ++i)
		MakeButton(pages[i], 12, kTopH + 14 + i * 41, kNavW - 24, IDC_TAB + i, -1, Kind::Nav, 40);
	MakeButton(L"Otwórz plik ini", 12, kWinH - 52, kNavW - 24, IDC_OPENINI, -1, Kind::Push, 34);
	g_sync = MakeWrapLabel(L"", kPadX, kWinH - 58, kRight - kPadX - 250, 42, -1);
	MakeButton(L"Anuluj", kRight - 238, kWinH - 54, 110, IDC_CANCEL, -1, Kind::Push, 34);
	MakeButton(L"Zapisz", kRight - 120, kWinH - 54, 120, IDC_SAVE, -1, Kind::Accent, 34);

	struct Row {
		const wchar_t *label;
		int id;
	};

	// ---- Ogólne ----
	// The buffer hint needs a line of its own, so the rows are placed by hand.
	const int generalY[4] = {96, 140, 212, 256};
	const Row general[] = {
		{L"Domyślne opóźnienie", IDC_DELAY},
		{L"Limit pamięci bufora", IDC_BUFFER},
		{L"Nasłuch RTMP", IDC_LISTEN},
		{L"Adres API", IDC_API},
	};
	for (int i = 0; i < 4; ++i) {
		MakeLabel(general[i].label, kPadX, generalY[i] + 4, 180, 0);
		MakeEdit(kPadX + 188, generalY[i], i < 2 ? 120 : 240, general[i].id, 0);
	}
	MakeLabel(L"s", kPadX + 318, generalY[0] + 4, 40, 0, true);
	MakeLabel(L"MB", kPadX + 318, generalY[1] + 4, 40, 0, true);
	g_hint = MakeWrapLabel(L"", kPadX + 188, generalY[1] + 36, kRight - kPadX - 188, 36, 0);
	MakeWrapLabel(L"Nasłuch RTMP to adres, który wpisujesz w programie nadającym z tego PC. Adres API obsługuje panel, "
		      L"overlay i podgląd. Zmiana każdego z nich przeładowuje silnik.",
		      kPadX, 304, kRight - kPadX, 44, 0);

	// ---- Platformy ----
	MakeWrapLabel(L"LiveDelay odbiera obraz z tego PC i wysyła go dalej na te platformy.", kPadX, 92, kRight - kPadX, 22, 1);
	HWND list = Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, kPadX, 120, 160,
			 240, IDC_LIST, 1);
	SendMessageW(list, LB_SETITEMHEIGHT, 0, Si(28));
	g_framed.push_back(list);
	MakeButton(L"Dodaj", kPadX, 370, 77, IDC_ADD, 1, Kind::Push);
	MakeButton(L"Usuń", kPadX + 83, 370, 77, IDC_DEL, 1, Kind::Push);

	const Row dest[] = {
		{L"Nazwa", IDC_NAME},
		{L"Usługa", IDC_SERVICE},
		{L"Serwer", IDC_SERVER},
		{L"Adres", IDC_URL},
		{L"Klucz streamu", IDC_KEY},
		{L"Klucz wejściowy", IDC_INGEST},
	};
	const int fieldX = kPadX + 180;
	const int ctlX = fieldX + 120;
	for (int i = 0; i < 6; ++i) {
		const int y = 120 + i * 40;
		MakeLabel(dest[i].label, fieldX, y + 4, 120, 1);
		const int id = dest[i].id;
		if (id == IDC_SERVICE || id == IDC_SERVER)
			MakeButton(L"", ctlX, y, kRight - ctlX, id, 1, Kind::Select, 28);
		else
			MakeEdit(ctlX, y, id == IDC_KEY ? kRight - ctlX - 90 : kRight - ctlX, id, 1,
				 id == IDC_KEY ? ES_PASSWORD : 0);
	}
	MakeButton(L"Pokaż", kRight - 82, 120 + 4 * 40, 82, IDC_SHOWKEY, 1, Kind::Push, 28);
	MakeCheck(L"Startuj razem ze streamem", ctlX, 364, kRight - ctlX, IDC_AUTOSTART, 1, true);
	MakeWrapLabel(L"Serwer wybierz z listy albo zostaw Własny adres i wpisz swój. Klucz wejściowy to ten, którym "
		      L"program na tym PC publikuje do LiveDelay — puste pole silnik wypełnia sam.",
		      fieldX, 398, kRight - fieldX, 60, 1);

	// ---- Overlay ----
	MakeLabel(L"Adres", kPadX, 100, 60, 2);
	MakeEdit(kPadX + 66, 96, 290, IDC_OVURL, 2, ES_READONLY);
	MakeButton(L"Kopiuj", kPadX + 366, 96, 90, IDC_OVCOPY, 2, Kind::Push, 28);
	MakeButton(L"Edytor wyglądu", kPadX + 464, 96, 128, IDC_OVEDITOR, 2, Kind::Push, 28);
	for (int i = 0; i < kStates; ++i) {
		const int y = 142 + i * 38;
		MakeCheck(L"", kPadX, y + 2, 28, IDC_OVSHOW + i, 2, true);
		MakeLabel(kStateLabel[i], kPadX + 36, y + 4, 130, 2);
		MakeEdit(kPadX + 174, y, kRight - kPadX - 174, IDC_OVTEXT + i, 2);
	}
	MakeCheck(L"Uzbrajanie i Uzbrojone jak Na żywo", kPadX, 302, 400, IDC_OVARMED, 2, false);
	MakeWrapLabel(L"Ptaszek z lewej decyduje, czy widzowie w ogóle widzą ten stan. {s} to sekundy opóźnienia, {buf} to ile "
		      L"już jest w buforze. Motywy, kolory i czcionka: w edytorze wyglądu.",
		      kPadX, 338, kRight - kPadX, 44, 2);

	// ---- Skróty ----
	MakeWrapLabel(L"Kliknij Ustaw i naciśnij skrót. Escape zostawia poprzedni, Backspace czyści. "
		      L"Działa, gdy to okno jest schowane. Ten sam zapis jest w panelu przeglądarki.",
		      kPadX, 92, kRight - kPadX, 40, 3);
	for (int i = 0; i < 4; ++i) {
		const int y = 146 + i * 44;
		MakeLabel(kHotkeyLabel[i], kPadX, y, 180, 3);
		HWND edit = MakeEdit(kPadX + 190, y - 4, 200, IDC_HOTKEY + i, 3);
		WNDPROC prev = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)HotEditProc);
		if (!g_hotEditProc)
			g_hotEditProc = prev;
		MakeButton(L"Ustaw", kPadX + 398, y - 5, 84, IDC_HOTCAP + i, 3, Kind::Push, 28);
	}
	g_hotStatus = MakeWrapLabel(L"", kPadX, 330, kRight - kPadX, 90, 3);
}

void FillFields() {
	g_filling = true;
	struct Done {
		~Done() { g_filling = false; }
	} done;
	Set(IDC_DELAY, std::to_wstring(g_orig.delay));
	Set(IDC_BUFFER, std::to_wstring(g_orig.buffer));
	Set(IDC_LISTEN, g_orig.listen);
	Set(IDC_API, g_orig.api);
	for (int i = 0; i < 4; ++i)
		Set(IDC_HOTKEY + i, IniLine(g_iniText, kHotkeyKey[i]));
	if (g_hotStatus)
		SetWindowTextW(g_hotStatus, HotkeyReport().c_str());
	FillList();
	ShowRow(g_sel);

	Set(IDC_OVURL, OverlayUrl());
	const bool haveOverlay = !g_overlay.empty();
	for (int i = 0; i < kStates; ++i) {
		Set(IDC_OVTEXT + i, Utf8(JsonIn(g_overlay, "texts", kStateKey[i])));
		SetChecked(IDC_OVSHOW + i, JsonBoolIn(g_overlay, "show", kStateKey[i], true));
		EnableWindow(Of(IDC_OVTEXT + i), haveOverlay);
		EnableWindow(Of(IDC_OVSHOW + i), haveOverlay);
	}
	SetChecked(IDC_OVARMED, JsonBool(g_overlay, "armedAsLive"));
	EnableWindow(Of(IDC_OVARMED), haveOverlay);
	if (!haveOverlay)
		Set(IDC_OVURL, L"silnik nie odpowiada — overlay niedostępny");
}

void RefreshHint() {
	if (g_hint)
		SetWindowTextW(g_hint, BufferHint().c_str());
}

// ---------------------------------------------------------------- window

void Close(bool saved) {
	g_saved = saved;
	DestroyWindow(g_wnd);
}

void OnCommand(int id, int code) {
	if (id >= IDC_HOTCAP && id < IDC_HOTCAP + 4) {
		SetFocus(Of(IDC_HOTKEY + (id - IDC_HOTCAP)));
		return;
	}
	if (!g_filling && (code == EN_CHANGE || code == BN_CLICKED || code == LBN_SELCHANGE) && id != IDC_SAVE &&
	    id != IDC_CANCEL && id != IDC_CLOSEWIN && id != IDC_OPENINI && id != IDC_SHOWKEY && id != IDC_OVCOPY &&
	    id != IDC_OVEDITOR && !(id >= IDC_TAB && id < IDC_TAB + kPages))
		g_dirty = true;
	if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
		// Select the key when it gets focus, so typing replaces it instead of
		// appending to whatever is already in there. Posted, so it survives the click.
		if (id == IDC_KEY && code == EN_SETFOCUS)
			PostMessageW(Of(IDC_KEY), EM_SETSEL, 0, -1);
		InvalidateRect(g_wnd, nullptr, FALSE); // the focused field gets a brighter outline
		return;
	}
	if (id >= IDC_TAB && id < IDC_TAB + kPages) {
		ShowPage(id - IDC_TAB);
		return;
	}
	if (id >= IDC_OVSHOW && id < IDC_OVSHOW + kStates) {
		SetChecked(id, !Checked(id));
		return;
	}
	switch (id) {
	case IDC_AUTOSTART:
	case IDC_OVARMED:
		SetChecked(id, !Checked(id));
		break;
	case IDC_LIST:
		if (code == LBN_SELCHANGE) {
			int i = (int)SendMessageW(Of(IDC_LIST), LB_GETCURSEL, 0, 0);
			if (i != g_sel)
				SelectRow(i);
		}
		break;
	case IDC_ADD: {
		StoreRow(g_sel);
		DestRow d;
		d.name = L"Nowa platforma";
		g_rows.push_back(d);
		g_sel = (int)g_rows.size() - 1;
		FillList();
		ShowRow(g_sel);
		SetFocus(Of(IDC_NAME));
		SendMessageW(Of(IDC_NAME), EM_SETSEL, 0, -1);
		break;
	}
	case IDC_DEL:
		if (g_sel >= 0 && g_sel < (int)g_rows.size()) {
			g_rows.erase(g_rows.begin() + g_sel);
			g_sel = g_rows.empty() ? -1 : std::min(g_sel, (int)g_rows.size() - 1);
			FillList();
			ShowRow(g_sel);
		}
		break;
	case IDC_SERVICE: {
		std::vector<std::wstring> names;
		for (const Service &s : kServices)
			names.push_back(s.name);
		names.push_back(L"Własny adres");
		int service = -1, server = -1;
		MatchUrl(Trim(Get(IDC_URL)), service, server);
		const int pickIndex = Pick(Of(IDC_SERVICE), names, service < 0 ? kServiceCount : service);
		if (pickIndex < 0)
			break;
		if (pickIndex < kServiceCount)
			Set(IDC_URL, kServices[pickIndex].ingests[0].url); // first ingest of that service
		ShowServiceLabels();
		break;
	}
	case IDC_SERVER: {
		int service = -1, server = -1;
		MatchUrl(Trim(Get(IDC_URL)), service, server);
		if (service < 0)
			break;
		std::vector<std::wstring> names;
		for (int n = 0; n < kServices[service].count; ++n)
			names.push_back(kServices[service].ingests[n].label);
		const int pickIndex = Pick(Of(IDC_SERVER), names, server);
		if (pickIndex >= 0)
			Set(IDC_URL, kServices[service].ingests[pickIndex].url);
		ShowServiceLabels();
		break;
	}
	case IDC_URL:
		if (code == EN_CHANGE)
			ShowServiceLabels();
		break;
	case IDC_KEY:
		// Select what is there, so typing replaces the key instead of growing it.
		if (code == EN_SETFOCUS)
			PostMessageW(Of(IDC_KEY), EM_SETSEL, 0, -1);
		break;
	case IDC_SHOWKEY:
		g_keyShown = !g_keyShown;
		SendMessageW(Of(IDC_KEY), EM_SETPASSWORDCHAR, g_keyShown ? 0 : L'\x25cf', 0);
		SetWindowTextW(Of(IDC_SHOWKEY), g_keyShown ? L"Ukryj" : L"Pokaż");
		InvalidateRect(Of(IDC_KEY), nullptr, TRUE);
		break;
	case IDC_OVCOPY:
		Copy(g_wnd, OverlayUrl());
		break;
	case IDC_OVEDITOR:
		OpenUrl(ThemeUrl());
		break;
	case IDC_OPENINI:
		ShellExecuteW(g_wnd, L"open", L"notepad.exe", g_ini.c_str(), nullptr, SW_SHOWNORMAL);
		break;
	case IDC_BUFFER:
		if (code == EN_CHANGE)
			RefreshHint();
		break;
	case IDC_SAVE:
		if (Save())
			Close(true);
		break;
	case IDC_CANCEL:
	case IDC_CLOSEWIN:
		Close(false);
		break;
	default:
		break;
	}
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_COMMAND:
		OnCommand(LOWORD(wp), HIWORD(wp));
		return 0;
	case WM_DRAWITEM: {
		auto *d = (DRAWITEMSTRUCT *)lp;
		if (d->CtlType == ODT_BUTTON)
			DrawButtonItem(d);
		else if (d->CtlType == ODT_LISTBOX)
			DrawListItem(d);
		else if (d->CtlType == ODT_MENU)
			DrawPickItem(d);
		return TRUE;
	}
	case WM_MEASUREITEM: {
		auto *mi = (MEASUREITEMSTRUCT *)lp;
		if (mi->CtlType != ODT_MENU)
			return DefWindowProcW(hwnd, msg, wp, lp);
		auto *item = (PickItem *)mi->itemData;
		HDC dc = GetDC(hwnd);
		HGDIOBJ old = SelectObject(dc, g_font);
		SIZE sz{};
		if (item)
			GetTextExtentPoint32W(dc, item->text.c_str(), (int)item->text.size(), &sz);
		SelectObject(dc, old);
		ReleaseDC(hwnd, dc);
		mi->itemWidth = sz.cx + Si(40);
		mi->itemHeight = std::max<UINT>(Si(28), sz.cy + Si(10));
		return TRUE;
	}
	case WM_CTLCOLORSTATIC: {
		HDC dc = (HDC)wp;
		SetBkColor(dc, Gdi(kBg));
		SetTextColor(dc, Gdi(GetWindowLongPtrW((HWND)lp, GWLP_USERDATA) == 1 ? kMuted : kText));
		return (LRESULT)g_brBg;
	}
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX: {
		HDC dc = (HDC)wp;
		SetBkColor(dc, Gdi(kFace));
		SetTextColor(dc, Gdi(IsWindowEnabled((HWND)lp) ? kText : kMuted));
		return (LRESULT)g_brFace;
	}
	case WM_NCACTIVATE:
		SuppressSystemFrame(hwnd);
		return DefWindowProcW(hwnd, WM_NCACTIVATE, wp, -1);
	case WM_NCPAINT:
		return 0;
	case WM_ACTIVATE:
		SuppressSystemFrame(hwnd);
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_NCCALCSIZE:
		if (wp)
			return 0; // the caption is ours; the client covers the window
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_NCHITTEST: {
		POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		ScreenToClient(hwnd, &pt);
		// The title bar drags the window; its close button does not.
		if (pt.y < Si(kTopH) && pt.x < Si(kWinW - 46))
			return HTCAPTION;
		return HTCLIENT;
	}
	case WM_GETMINMAXINFO: {
		// The pages are laid out at a fixed size, so the window keeps it.
		auto *mm = (MINMAXINFO *)lp;
		mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = Si(kWinW);
		mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = Si(kWinH);
		return 0;
	}
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(hwnd, &r);
		FillRect((HDC)wp, &r, g_brBg);
		return 1;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		PaintShell(dc);
		PaintFrames(dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_TIMER: {
		static int ticks = 0;
		if (++ticks >= 11) { // about once a second, on the 90 ms hover clock
			ticks = 0;
			CheckOutsideChange();
		}
		// Cheap hover tracking: owner-drawn buttons do not report it themselves.
		POINT pt;
		GetCursorPos(&pt);
		HWND under = WindowFromPoint(pt);
		if (under != g_hot) {
			HWND was = g_hot;
			g_hot = under;
			for (HWND h : {was, under})
				if (h && IsChild(hwnd, h))
					InvalidateRect(h, nullptr, TRUE);
		}
		return 0;
	}
	case WM_CLOSE:
		Close(false);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, 1);
		g_wnd = nullptr;
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

} // namespace

bool ShowSettings(HWND owner) {
	static bool registered = false;
	HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
	if (!registered) {
		WNDCLASSW wc{};
		wc.lpfnWndProc = Proc;
		wc.hInstance = inst;
		wc.lpszClassName = L"LiveDelaySettings";
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassW(&wc);
		registered = true;
	}

	g_saved = false;
	g_page = 0;
	g_hot = nullptr;
	g_ctls.clear();
	g_framed.clear();
	g_checked.clear();

	g_font = CreateFontW(-Si(14.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	g_title = CreateFontW(-Si(18.0f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	g_bold = CreateFontW(-Si(14.5f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	g_brBg = CreateSolidBrush(Gdi(kBg));
	g_brFace = CreateSolidBrush(Gdi(kFace));

	RECT want{0, 0, Si(kWinW), Si(kWinH)};
	AdjustWindowRect(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
	// Our own title bar, like the main window's.
	g_wnd = CreateWindowExW(0, L"LiveDelaySettings", L"LiveDelay – ustawienia", WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
				CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top, owner, nullptr, inst,
				nullptr);
	if (!g_wnd)
		return false;
	SuppressSystemFrame(g_wnd);

	ReadEverything();
	Build();
	FillFields();
	RefreshHint();
	ShowPage(0);
	SetTimer(g_wnd, 1, 90, nullptr);

	EnableWindow(owner, FALSE);
	ShowWindow(g_wnd, SW_SHOW);
	SetFocus(Of(IDC_DELAY));

	MSG msg{};
	while (g_wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && IsChild(g_wnd, msg.hwnd)) {
			Close(false);
			continue;
		}
		if (!g_wnd || !IsDialogMessageW(g_wnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	if (msg.message == WM_QUIT)
		PostQuitMessage((int)msg.wParam);

	EnableWindow(owner, TRUE);
	SetForegroundWindow(owner);
	DeleteObject(g_font);
	DeleteObject(g_bold);
	DeleteObject(g_title);
	DeleteObject(g_brBg);
	DeleteObject(g_brFace);
	return g_saved;
}

} // namespace ld

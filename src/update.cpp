#include "update.hpp"
#include "core.hpp"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ld {
namespace {

#ifndef LD_APP_VERSION
#define LD_APP_VERSION "0.1.0"
#endif

constexpr const wchar_t *kMirror = L"https://livedelay.vxh.pl/version.json";

HWND g_hwnd = nullptr;
std::mutex g_mu;
std::wstring g_plugin = L"nieznana";
bool g_checking = false;
bool g_busy = false;
bool g_pluginNewer = false;
bool g_appNewer = false;
std::wstring g_pluginRemote, g_appRemote;
std::wstring g_pluginFile, g_appFile;
std::wstring g_pluginPage, g_appPage;
std::wstring g_status = L"Sprawdzanie włącza się samo po chwili.";
std::wstring g_skipPlugin, g_skipApp;
int g_ticks = 0;

std::wstring ExePath() {
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	return path;
}

std::wstring DirOf(const std::wstring &path) {
	size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring SkipPath() {
	std::wstring dir = DirOf(g_ini);
	return dir + L"\\update-skip.txt";
}

void LoadSkip() {
	std::string text = LoadText(SkipPath());
	g_skipPlugin = IniLine(text, "plugin");
	g_skipApp = IniLine(text, "app");
}

void StoreSkip() {
	std::string text = "plugin=" + WideUtf8(g_skipPlugin) + "\r\napp=" + WideUtf8(g_skipApp) + "\r\n";
	StoreText(SkipPath(), text);
}

std::wstring StripV(std::wstring v) {
	if (!v.empty() && (v[0] == L'v' || v[0] == L'V'))
		v.erase(v.begin());
	while (!v.empty() && iswspace(v.front()))
		v.erase(v.begin());
	while (!v.empty() && iswspace(v.back()))
		v.pop_back();
	return v;
}

bool HasDigit(const std::wstring &v) {
	return std::any_of(v.begin(), v.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

std::vector<int> Parts(std::wstring v) {
	v = StripV(std::move(v));
	std::vector<int> out;
	int cur = 0;
	bool any = false;
	for (wchar_t c : v) {
		if (c >= L'0' && c <= L'9') {
			cur = cur * 10 + (c - L'0');
			any = true;
		} else if (any) {
			out.push_back(cur);
			cur = 0;
			any = false;
		}
	}
	if (any)
		out.push_back(cur);
	while (out.size() < 3)
		out.push_back(0);
	return out;
}

bool NewerThan(const std::wstring &remote, const std::wstring &local) {
	if (!HasDigit(remote) || !HasDigit(local))
		return false;
	auto a = Parts(remote), b = Parts(local);
	size_t n = std::max(a.size(), b.size());
	a.resize(n, 0);
	b.resize(n, 0);
	for (size_t i = 0; i < n; ++i) {
		if (a[i] > b[i])
			return true;
		if (a[i] < b[i])
			return false;
	}
	return false;
}

size_t SkipWs(const std::string &j, size_t p, size_t end) {
	while (p < end && std::isspace((unsigned char)j[p]))
		++p;
	return p;
}

// Value start after "key", or npos. `end` limits the search to one object.
size_t FindValue(const std::string &j, const char *key, size_t from, size_t end) {
	std::string pat = std::string("\"") + key + "\"";
	size_t p = from;
	while (p < end) {
		p = j.find(pat, p);
		if (p == std::string::npos || p >= end)
			return std::string::npos;
		size_t q = SkipWs(j, p + pat.size(), end);
		if (q < end && j[q] == ':')
			return SkipWs(j, q + 1, end);
		p += pat.size();
	}
	return std::string::npos;
}

std::string JsonAt(const std::string &j, size_t p, size_t end) {
	if (p >= end || j[p] != '"')
		return {};
	++p;
	std::string out;
	while (p < end && j[p] != '"') {
		if (j[p] == '\\' && p + 1 < end) {
			out.push_back(j[p + 1]);
			p += 2;
			continue;
		}
		out.push_back(j[p++]);
	}
	return out;
}

std::string JsonIn(const std::string &j, const char *key, size_t from, size_t end) {
	size_t p = FindValue(j, key, from, end);
	if (p == std::string::npos)
		return {};
	return JsonAt(j, p, end);
}

// Byte range of the object that is the value of `key`.
bool ObjectRange(const std::string &j, const char *key, size_t *begin, size_t *end) {
	size_t p = FindValue(j, key, 0, j.size());
	if (p == std::string::npos || j[p] != '{')
		return false;
	int depth = 0;
	for (size_t i = p; i < j.size(); ++i) {
		if (j[i] == '{')
			++depth;
		else if (j[i] == '}' && --depth == 0) {
			*begin = p;
			*end = i + 1;
			return true;
		}
	}
	return false;
}

bool HttpGet(const std::wstring &urlStr, std::string *out, std::wstring *err) {
	out->clear();
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof parts;
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	wchar_t extra[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 2048;
	parts.lpszExtraInfo = extra;
	parts.dwExtraInfoLength = 2048;
	if (!WinHttpCrackUrl(urlStr.c_str(), 0, 0, &parts)) {
		if (err)
			*err = L"zły adres";
		return false;
	}
	if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
		if (err)
			*err = L"adres nie jest https";
		return false;
	}
	std::wstring ua = L"livedelay-app/" + AppVersion();
	HINTERNET ses = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) {
		if (err)
			*err = L"brak sieci";
		return false;
	}
	WinHttpSetTimeouts(ses, 4000, 4000, 20000, 60000);
	HINTERNET con = WinHttpConnect(ses, host, parts.nPort, 0);
	DWORD flags = WINHTTP_FLAG_SECURE;
	std::wstring object = path;
	object += extra;
	HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
	if (req) {
		DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
	}
	bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
	DWORD status = 0;
	if (ok) {
		DWORD size = sizeof status;
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
				    WINHTTP_NO_HEADER_INDEX);
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
				break;
			if (out->size() + avail > 80 * 1024 * 1024) {
				ok = false;
				if (err)
					*err = L"plik jest za duży";
				break;
			}
			std::string chunk(avail, 0);
			DWORD read = 0;
			if (!WinHttpReadData(req, chunk.data(), avail, &read)) {
				ok = false;
				break;
			}
			chunk.resize(read);
			*out += chunk;
		}
	}
	if (req)
		WinHttpCloseHandle(req);
	if (con)
		WinHttpCloseHandle(con);
	WinHttpCloseHandle(ses);
	if (!ok || status < 200 || status >= 300) {
		if (err && err->empty())
			*err = status ? L"HTTP " + std::to_wstring(status) : L"pobieranie przerwane";
		return false;
	}
	return true;
}

void ApplyParsed(const std::string &body, bool force) {
	size_t appB = 0, appE = 0, engB = 0, engE = 0;
	bool hasApp = ObjectRange(body, "app", &appB, &appE);
	bool hasEng = ObjectRange(body, "engine", &engB, &engE);
	std::wstring pluginRemote = Utf8(JsonIn(body, "version", 0, body.size()));
	std::wstring pluginPage = Utf8(JsonIn(body, "url", 0, body.size()));
	std::wstring pluginFile;
	if (hasEng) {
		std::string dll = JsonIn(body, "dll", engB, engE);
		if (!dll.empty())
			pluginFile = Utf8(dll);
	}
	std::wstring appRemote, appPage, appFile;
	if (hasApp) {
		appRemote = Utf8(JsonIn(body, "version", appB, appE));
		appPage = Utf8(JsonIn(body, "url", appB, appE));
		appFile = Utf8(JsonIn(body, "zip", appB, appE));
	}
	pluginRemote = StripV(pluginRemote);
	appRemote = StripV(appRemote);

	std::lock_guard<std::mutex> lock(g_mu);
	g_pluginRemote = pluginRemote;
	g_pluginPage = pluginPage.empty() ? L"https://github.com/skullboypl/livedelay-obs/releases/latest" : pluginPage;
	g_pluginFile = pluginFile;
	g_appRemote = appRemote;
	g_appPage = appPage.empty() ? L"https://github.com/skullboypl/livedelay-standalone-app/releases/latest" : appPage;
	g_appFile = appFile;
	g_pluginNewer = NewerThan(pluginRemote, g_plugin) && (force || pluginRemote != g_skipPlugin);
	g_appNewer = NewerThan(appRemote, AppVersion()) && (force || appRemote != g_skipApp);
	if (g_pluginNewer || g_appNewer)
		g_status = L"Jest nowsza wersja.";
	else if (!HasDigit(pluginRemote) && !HasDigit(appRemote))
		g_status = L"Serwer nie podał wersji.";
	else
		g_status = L"Aplikacja i wtyczka są aktualne.";
	g_checking = false;
}

void CheckThread(bool force) {
	std::string body;
	std::wstring err;
	bool ok = HttpGet(kMirror, &body, &err);
	if (!ok) {
		std::lock_guard<std::mutex> lock(g_mu);
		g_checking = false;
		g_status = L"Nie udało się sprawdzić: " + (err.empty() ? L"sieć" : err);
	} else {
		ApplyParsed(body, force);
	}
	if (g_hwnd)
		PostMessageW(g_hwnd, WM_LD_UPDATE, 0, 0);
}

std::wstring Quote(std::wstring s) {
	std::wstring out;
	for (wchar_t c : s) {
		if (c == L'\'')
			out += L"''";
		else
			out.push_back(c);
	}
	return out;
}

bool WriteBom(const std::wstring &path, const std::wstring &text) {
	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
		return false;
	std::string utf8 = WideUtf8(text);
	unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
	fwrite(bom, 1, 3, f);
	fwrite(utf8.data(), 1, utf8.size(), f);
	fclose(f);
	return true;
}

bool WriteFileBytes(const std::wstring &path, const std::string &bytes) {
	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
		return false;
	fwrite(bytes.data(), 1, bytes.size(), f);
	fclose(f);
	return true;
}

bool RunWait(const std::wstring &exe, const std::wstring &args) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	STARTUPINFOW si{};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;
	WaitForSingleObject(pi.hProcess, 60000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code == 0;
}

std::wstring FindNamed(const std::wstring &dir, const wchar_t *name, int depth) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return {};
	std::wstring found;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
			continue;
		std::wstring path = dir + L"\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (depth > 0)
				found = FindNamed(path, name, depth - 1);
		} else if (_wcsicmp(fd.cFileName, name) == 0) {
			found = path;
		}
		if (!found.empty())
			break;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return found;
}

// Downloads on this thread, then asks the window to quit. `wParam` 1 means quit.
void DownloadThread(std::wstring pluginFile, std::wstring appFile) {
	auto fail = [](const std::wstring &why) {
		{
			std::lock_guard<std::mutex> lock(g_mu);
			g_busy = false;
			g_status = why;
		}
		if (g_hwnd)
			PostMessageW(g_hwnd, WM_LD_UPDATE, 0, 0);
		MessageBoxW(g_hwnd, why.c_str(), L"LiveDelay", MB_OK | MB_ICONWARNING);
	};

	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	std::wstring work = std::wstring(temp) + L"LiveDelay-update";
	CreateDirectoryW(work.c_str(), nullptr);
	std::wstring dllSrc, exeSrc;

	if (!appFile.empty()) {
		std::string zip;
		std::wstring err;
		if (!HttpGet(appFile, &zip, &err) || zip.size() < 1000)
			return fail(L"Nie udało się pobrać aplikacji. " + err);
		std::wstring zipPath = work + L"\\app.zip";
		std::wstring extract = work + L"\\app";
		CreateDirectoryW(extract.c_str(), nullptr);
		if (!WriteFileBytes(zipPath, zip))
			return fail(L"Nie udało się zapisać paczki.");
		if (!RunWait(L"tar.exe", L"-xf \"" + zipPath + L"\" -C \"" + extract + L"\""))
			return fail(L"Nie udało się rozpakować aplikacji.");
		exeSrc = FindNamed(extract, L"livedelay.exe", 3);
		dllSrc = FindNamed(extract, L"livedelay.dll", 3);
		if (exeSrc.empty())
			return fail(L"W paczce nie ma livedelay.exe.");
	}
	if (!pluginFile.empty() && dllSrc.empty()) {
		std::string dll;
		std::wstring err;
		if (!HttpGet(pluginFile, &dll, &err) || dll.size() < 1000)
			return fail(L"Nie udało się pobrać wtyczki. " + err);
		dllSrc = work + L"\\livedelay.dll";
		if (!WriteFileBytes(dllSrc, dll))
			return fail(L"Nie udało się zapisać wtyczki.");
	}

	std::wstring exe = ExePath();
	std::wstring destDir = DirOf(exe);
	DWORD pid = GetCurrentProcessId();
	std::wstring script = work + L"\\apply.ps1";
	std::wstring ps =
		L"$ErrorActionPreference = 'Stop'\r\n"
		L"$target = " +
		std::to_wstring(pid) +
		L"\r\n"
		L"$deadline = (Get-Date).AddSeconds(90)\r\n"
		L"while (Get-Process -Id $target -ErrorAction SilentlyContinue) {\r\n"
		L"  if ((Get-Date) -gt $deadline) { exit 1 }\r\n"
		L"  Start-Sleep -Milliseconds 200\r\n"
		L"}\r\n"
		L"Start-Sleep -Milliseconds 400\r\n"
		L"$dest = '" +
		Quote(destDir) + L"'\r\n" + L"$exe = '" + Quote(exe) + L"'\r\n";
	if (!dllSrc.empty())
		ps += L"Copy-Item -LiteralPath '" + Quote(dllSrc) + L"' -Destination (Join-Path $dest 'livedelay.dll') -Force\r\n";
	if (!exeSrc.empty())
		ps += L"Copy-Item -LiteralPath '" + Quote(exeSrc) + L"' -Destination $exe -Force\r\n";
	ps += L"Start-Process -FilePath $exe\r\n";
	if (!WriteBom(script, ps))
		return fail(L"Nie udało się przygotować podmiany.");

	std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\"";
	HINSTANCE launched = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
	if ((INT_PTR)launched <= 32)
		return fail(L"Nie udało się uruchomić podmiany.");
	AppLog(L"aktualizacja: zamykam program, pliki podmieni skrypt");
	if (g_hwnd)
		PostMessageW(g_hwnd, WM_LD_UPDATE, 1, 0);
}

} // namespace

std::wstring AppVersion() {
	return Utf8(LD_APP_VERSION);
}

std::wstring PluginVersion() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_plugin;
}

void SetPluginVersion(const std::wstring &version) {
	std::lock_guard<std::mutex> lock(g_mu);
	if (!version.empty())
		g_plugin = version;
}

void UpdateBind(HWND hwnd) {
	g_hwnd = hwnd;
	LoadSkip();
}

void BeginCheck(bool force) {
	{
		std::lock_guard<std::mutex> lock(g_mu);
		if (g_checking || g_busy)
			return;
		g_checking = true;
		g_status = L"Sprawdzam…";
	}
	std::thread(CheckThread, force).detach();
}

void UpdatePoll() {
	// The window timer is 500 ms. First look after 4 s, then every 12 hours.
	if (++g_ticks == 8 || (g_ticks > 8 && g_ticks % (12 * 60 * 60 * 2) == 0))
		BeginCheck(false);
}

void UpdateCheckNow() {
	BeginCheck(true);
}

bool UpdatePending() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_pluginNewer || g_appNewer;
}

bool UpdateBusy() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_busy || g_checking;
}

bool UpdateCanDownload() {
	std::lock_guard<std::mutex> lock(g_mu);
	if (g_appNewer && !g_appFile.empty())
		return true;
	if (g_pluginNewer && !g_pluginFile.empty())
		return true;
	return false;
}

std::wstring UpdateBannerText() {
	std::lock_guard<std::mutex> lock(g_mu);
	if (g_busy)
		return L"Pobieranie aktualizacji…";
	std::wstring t;
	if (g_pluginNewer)
		t = L"Wtyczka " + g_pluginRemote;
	if (g_appNewer) {
		if (!t.empty())
			t += L" · ";
		t += L"Aplikacja " + g_appRemote;
	}
	bool can = (g_appNewer && !g_appFile.empty()) || (g_pluginNewer && !g_pluginFile.empty());
	if (!t.empty())
		t += can ? L" jest gotowa" : L" — otwórz stronę wydania";
	return t;
}

std::wstring UpdateActionLabel() {
	if (UpdateBusy())
		return L"Czekaj";
	return UpdateCanDownload() ? L"Aktualizuj" : L"Otwórz";
}

std::wstring UpdateStatus() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_status;
}

void UpdateLater() {
	std::lock_guard<std::mutex> lock(g_mu);
	if (g_pluginNewer)
		g_skipPlugin = g_pluginRemote;
	if (g_appNewer)
		g_skipApp = g_appRemote;
	g_pluginNewer = false;
	g_appNewer = false;
	g_status = L"Pominięte do następnej wersji.";
	StoreSkip();
}

void UpdateApply(HWND owner) {
	std::wstring pluginFile, appFile, page;
	bool download = false;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		if (g_busy)
			return;
		if (g_appNewer && !g_appFile.empty()) {
			appFile = g_appFile;
			download = true;
		} else if (g_pluginNewer && !g_pluginFile.empty()) {
			pluginFile = g_pluginFile;
			download = true;
		} else if (g_appNewer) {
			page = g_appPage;
		} else {
			page = g_pluginPage;
		}
	}
	if (!download) {
		if (!page.empty())
			OpenUrl(page);
		return;
	}
	int answer = MessageBoxW(owner,
				  L"LiveDelay pobierze aktualizację i uruchomi się ponownie.\n"
				  L"Trwająca transmisja z tego PC zostanie przerwana.",
				  L"LiveDelay", MB_OKCANCEL | MB_ICONINFORMATION);
	if (answer != IDOK)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		g_busy = true;
		g_status = L"Pobieranie…";
	}
	std::thread(DownloadThread, pluginFile, appFile).detach();
}

void UninstallApp(HWND owner) {
	int answer = MessageBoxW(owner,
				  L"Odinstalować LiveDelay?\n\n"
				  L"Program zniknie z menu Start i z folderu instalacji.\n"
				  L"Ustawienia i klucze zostaną na tym komputerze.",
				  L"LiveDelay", MB_OKCANCEL | MB_ICONWARNING);
	if (answer != IDOK)
		return;

	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	std::wstring script = std::wstring(temp) + L"LiveDelay-uninstall.ps1";
	DWORD pid = GetCurrentProcessId();
	std::wstring ps =
		L"$ErrorActionPreference = 'Stop'\r\n"
		L"$target = " +
		std::to_wstring(pid) +
		L"\r\n"
		L"$deadline = (Get-Date).AddSeconds(90)\r\n"
		L"while (Get-Process -Id $target -ErrorAction SilentlyContinue) {\r\n"
		L"  if ((Get-Date) -gt $deadline) { exit 1 }\r\n"
		L"  Start-Sleep -Milliseconds 200\r\n"
		L"}\r\n"
		L"Start-Sleep -Milliseconds 400\r\n"
		L"$dest = Join-Path $env:LOCALAPPDATA 'LiveDelay\\App'\r\n"
		L"if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }\r\n"
		L"$shortcut = Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs\\LiveDelay.lnk'\r\n"
		L"if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }\r\n";
	bool wrote = false;
	{
		FILE *f = nullptr;
		if (_wfopen_s(&f, script.c_str(), L"wb") == 0 && f) {
			std::string utf8 = WideUtf8(ps);
			unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
			fwrite(bom, 1, 3, f);
			fwrite(utf8.data(), 1, utf8.size(), f);
			fclose(f);
			wrote = true;
		}
	}
	if (!wrote) {
		MessageBoxW(owner, L"Nie udało się przygotować odinstalowania.", L"LiveDelay", MB_OK | MB_ICONWARNING);
		return;
	}
	std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\"";
	HINSTANCE launched = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
	if ((INT_PTR)launched <= 32) {
		MessageBoxW(owner, L"Nie udało się uruchomić odinstalowania.", L"LiveDelay", MB_OK | MB_ICONWARNING);
		return;
	}
	AppLog(L"odinstalowanie: zamykam program");
	if (g_hwnd)
		PostMessageW(g_hwnd, WM_LD_UPDATE, 1, 0);
}

} // namespace ld

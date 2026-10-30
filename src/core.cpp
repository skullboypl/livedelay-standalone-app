#include "core.hpp"

#include <shellapi.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace ld {

float g_scale = 1.0f;

std::wstring g_ini;
std::wstring g_listen = L"127.0.0.1:1935";
std::wstring g_api = L"127.0.0.1:7799";
std::wstring g_ingest = L"ld0";

// ---------------------------------------------------------------- text

std::wstring Utf8(const std::string &s) {
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string WideUtf8(const std::wstring &w) {
	if (w.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// ---------------------------------------------------------------- files

std::string LoadText(const std::wstring &path) {
	FILE *f = nullptr;
	_wfopen_s(&f, path.c_str(), L"rb");
	if (!f)
		return {};
	std::string text;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		text.append(buf, n);
	fclose(f);
	return text;
}

bool StoreText(const std::wstring &path, const std::string &text) {
	// Write beside the target and rename, so a crash cannot leave half a file.
	std::wstring tmp = path + L".tmp";
	FILE *f = nullptr;
	_wfopen_s(&f, tmp.c_str(), L"wb");
	if (!f)
		return false;
	bool ok = text.empty() || fwrite(text.data(), 1, text.size(), f) == text.size();
	fclose(f);
	if (!ok) {
		_wremove(tmp.c_str());
		return false;
	}
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---------------------------------------------------------------- the ini

void SetIniPath(const std::wstring &path) {
	g_ini = path;
}

std::wstring IniLine(const std::string &text, const char *key) {
	// Same leniency as the engine's parser: trim, split on the first '='.
	auto trim = [](std::string s) {
		size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos)
			return std::string();
		return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
	};
	size_t p = 0;
	while (p < text.size()) {
		size_t nl = text.find('\n', p);
		std::string line = trim(text.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
		size_t eq = line.find('=');
		if (!line.empty() && line[0] != '#' && line[0] != ';' && line[0] != '[' && eq != std::string::npos &&
		    trim(line.substr(0, eq)) == key)
			return Utf8(trim(line.substr(eq + 1)));
		if (nl == std::string::npos)
			break;
		p = nl + 1;
	}
	return {};
}

void ReadIniHints() {
	std::string text = LoadText(g_ini);
	if (text.empty())
		return;
	std::wstring listen = IniLine(text, "listen");
	std::wstring api = IniLine(text, "api");
	std::wstring ingest = IniLine(text, "ingest_key");
	if (!listen.empty())
		g_listen = listen;
	if (!api.empty())
		g_api = api;
	if (!ingest.empty())
		g_ingest = ingest;
}

// ---------------------------------------------------------------- engine API

namespace {

void SplitHost(const std::wstring &api, std::wstring &host, INTERNET_PORT &port) {
	host = api;
	port = 7799;
	size_t colon = api.rfind(L':');
	if (colon != std::wstring::npos && api.find(L':') == colon) {
		host = api.substr(0, colon);
		port = (INTERNET_PORT)_wtoi(api.c_str() + colon + 1);
	}
}

} // namespace

std::string Http(const wchar_t *method, const std::wstring &path, const std::string &body) {
	std::wstring host;
	INTERNET_PORT port = 7799;
	SplitHost(g_api, host, port);
	HINTERNET ses = WinHttpOpen(L"livedelay-app/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses)
		return {};
	WinHttpSetTimeouts(ses, 400, 400, 1500, 1500);
	HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
	HINTERNET req = con ? WinHttpOpenRequest(con, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
	std::string out;
	const wchar_t *headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
	DWORD headerLen = body.empty() ? 0 : (DWORD)-1L;
	if (req &&
	    WinHttpSendRequest(req, headers, headerLen, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
			       (DWORD)body.size(), 0) &&
	    WinHttpReceiveResponse(req, nullptr)) {
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
				break;
			std::string chunk(avail, 0);
			DWORD read = 0;
			if (!WinHttpReadData(req, chunk.data(), avail, &read))
				break;
			chunk.resize(read);
			out += chunk;
		}
	}
	if (req)
		WinHttpCloseHandle(req);
	if (con)
		WinHttpCloseHandle(con);
	WinHttpCloseHandle(ses);
	return out;
}

std::wstring ApiBase() {
	return L"http://" + g_api;
}

std::wstring PreviewUrl() {
	return ApiBase() + L"/live-preview?lang=pl";
}

std::wstring OverlayUrl() {
	return ApiBase() + L"/overlay";
}

std::wstring ThemeUrl() {
	return ApiBase() + L"/edit-theme";
}

std::wstring PanelUrl() {
	return ApiBase() + L"/panel";
}

std::wstring RtmpUrl() {
	return L"rtmp://" + g_listen + L"/live";
}

void OpenUrl(const std::wstring &url) {
	ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Copy(HWND owner, const std::wstring &text) {
	if (!OpenClipboard(owner))
		return;
	EmptyClipboard();
	size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem) {
		void *dst = GlobalLock(mem);
		if (dst) {
			memcpy(dst, text.c_str(), bytes);
			GlobalUnlock(mem);
			SetClipboardData(CF_UNICODETEXT, mem);
		}
	}
	CloseClipboard();
}

// ---------------------------------------------------------------- JSON reading

std::string JsonString(const std::string &j, const char *key, size_t from) {
	std::string pat = std::string("\"") + key + "\":\"";
	size_t p = j.find(pat, from);
	if (p == std::string::npos)
		return {};
	p += pat.size();
	std::string out;
	while (p < j.size() && j[p] != '"') {
		if (j[p] == '\\' && p + 1 < j.size()) {
			out.push_back(j[p + 1]);
			p += 2;
			continue;
		}
		out.push_back(j[p++]);
	}
	return out;
}

double JsonNumber(const std::string &j, const char *key, double def, size_t from) {
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat, from);
	if (p == std::string::npos)
		return def;
	p += pat.size();
	if (p >= j.size() || (!std::isdigit((unsigned char)j[p]) && j[p] != '-'))
		return def; // null, or something else we do not read
	size_t e = p;
	while (e < j.size() && (std::isdigit((unsigned char)j[e]) || j[e] == '.' || j[e] == '-'))
		++e;
	return std::atof(j.substr(p, e - p).c_str());
}

bool JsonBool(const std::string &j, const char *key, size_t from) {
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat, from);
	return p != std::string::npos && j.compare(p + pat.size(), 4, "true") == 0;
}

} // namespace ld

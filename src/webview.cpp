#include "webview.hpp"

#include "core.hpp"

#include <shlobj.h>

#include <cstring>

#ifdef LIVEDLAY_WEBVIEW2

#include <WebView2.h>
#include <wrl.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace ld {
namespace {

HWND g_parent = nullptr;
ComPtr<ICoreWebView2Controller> g_ctl;
ComPtr<ICoreWebView2> g_web;
std::wstring g_url;
std::wstring g_shown;
RECT g_bounds{};
bool g_started = false;
bool g_failed = false;
bool g_visible = false;

void Apply() {
	if (!g_ctl || !g_web)
		return;
	if (!g_visible) {
		g_ctl->put_IsVisible(TRUE);
		g_visible = true;
	}
	RECT cur{};
	g_ctl->get_Bounds(&cur);
	if (std::memcmp(&cur, &g_bounds, sizeof(RECT)) != 0)
		g_ctl->put_Bounds(g_bounds);
	if (g_shown != g_url) {
		g_shown = g_url;
		g_web->Navigate(g_url.c_str());
	}
}

void Fail(const wchar_t *why) {
	if (!g_failed)
		AppLog(why);
	g_failed = true;
}

std::wstring UserData() {
	wchar_t appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata)))
		return L".";
	std::wstring dir = std::wstring(appdata) + L"\\LiveDelay";
	CreateDirectoryW(dir.c_str(), nullptr);
	dir += L"\\webview2";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

void Ensure(HWND parent) {
	if (g_started || g_failed)
		return;
	g_started = true;
	g_parent = parent;
	const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
		Fail(L"podgląd w oknie: COM niedostępny");
		return;
	}
	const std::wstring data = UserData();
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		nullptr, data.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
				if (FAILED(result) || !env || !g_parent) {
					Fail(L"podgląd w oknie: brak środowiska WebView2");
					return S_OK;
				}
				env->CreateCoreWebView2Controller(
					g_parent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							  [](HRESULT result, ICoreWebView2Controller *ctl) -> HRESULT {
								  if (FAILED(result) || !ctl) {
									  Fail(L"podgląd w oknie: nie udało się otworzyć widoku");
									  return S_OK;
								  }
								  g_ctl = ctl;
								  g_ctl->get_CoreWebView2(&g_web);
								  if (!g_web) {
									  Fail(L"podgląd w oknie: pusty widok");
									  return S_OK;
								  }
								  ComPtr<ICoreWebView2Settings> settings;
								  if (SUCCEEDED(g_web->get_Settings(&settings)) && settings) {
									  settings->put_AreDevToolsEnabled(FALSE);
									  settings->put_IsStatusBarEnabled(FALSE);
									  settings->put_AreDefaultContextMenusEnabled(TRUE);
								  }
								  ComPtr<ICoreWebView2Controller2> ctl2;
								  if (SUCCEEDED(g_ctl->QueryInterface(IID_PPV_ARGS(&ctl2))) && ctl2) {
									  const COREWEBVIEW2_COLOR ink{255, 7, 7, 7};
									  ctl2->put_DefaultBackgroundColor(ink);
								  }
								  g_shown.clear();
								  g_visible = false;
								  Apply();
								  return S_OK;
							  })
						.Get());
				return S_OK;
			})
			.Get());
	if (FAILED(hr))
		Fail(L"podgląd w oknie: WebView2 nie wystartował");
}

} // namespace

void WebShow(HWND parent, const std::wstring &url, RECT bounds) {
	g_url = url;
	g_bounds = bounds;
	Ensure(parent);
	Apply();
}

void WebHide() {
	if (g_ctl && g_visible) {
		g_ctl->put_IsVisible(FALSE);
		g_visible = false;
	}
}

bool WebFailed() {
	return g_failed;
}

} // namespace ld

#else

namespace ld {

void WebShow(HWND, const std::wstring &, RECT) {}

void WebHide() {}

bool WebFailed() {
	return true;
}

} // namespace ld

#endif

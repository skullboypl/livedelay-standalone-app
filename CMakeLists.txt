cmake_minimum_required(VERSION 3.24)
project(livedelay-app VERSION 0.1.0 LANGUAGES CXX)

add_executable(livedelay-app WIN32 src/app.cpp src/core.cpp src/settings.cpp src/webview.cpp src/update.cpp src/app.rc)
set_target_properties(livedelay-app PROPERTIES OUTPUT_NAME livedelay CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(livedelay-app PRIVATE /utf-8 /W3 /permissive-)
target_compile_definitions(livedelay-app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE LD_APP_VERSION="${PROJECT_VERSION}")
configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
target_sources(livedelay-app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_link_libraries(livedelay-app PRIVATE opengl32 winhttp user32 gdi32 shell32 dwmapi ole32)

# Preview, overlay and the theme editor stay inside the window when the SDK is present.
if(DEFINED ENV{USERPROFILE})
	set(WEBVIEW2_ROOT "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/1.0.3719.77/build/native")
endif()
if(EXISTS "${WEBVIEW2_ROOT}/include/WebView2.h")
	target_include_directories(livedelay-app PRIVATE "${WEBVIEW2_ROOT}/include")
	target_compile_definitions(livedelay-app PRIVATE LIVEDLAY_WEBVIEW2=1)
	target_link_libraries(livedelay-app PRIVATE "${WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib" version)
endif()

# The delay engine is not built here. build.bat copies livedelay.dll from the plugin repo.

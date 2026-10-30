cmake_minimum_required(VERSION 3.24)
project(livedelay-app VERSION 0.1.0 LANGUAGES CXX)

add_executable(livedelay-app WIN32 src/app.cpp src/core.cpp src/settings.cpp src/app.rc)
set_target_properties(livedelay-app PROPERTIES OUTPUT_NAME livedelay CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(livedelay-app PRIVATE /utf-8 /W3 /permissive-)
target_compile_definitions(livedelay-app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(livedelay-app PRIVATE opengl32 winhttp user32 gdi32 shell32)

# The delay engine is not built here. build.bat copies livedelay.dll from the plugin repo.

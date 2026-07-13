cmake_minimum_required(VERSION 3.20)
project(PlayerEditor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- GLFW ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

# ---- Dear ImGui (docking branch) ----
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking)
FetchContent_MakeAvailable(imgui)

# ---- miniaudio (header only) ----
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG master)
FetchContent_MakeAvailable(miniaudio)

# ---- portable-file-dialogs (header only) ----
FetchContent_Declare(pfd
    GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
    GIT_TAG main)
FetchContent_MakeAvailable(pfd)

# ImGui as a static lib (imgui core + GLFW/OpenGL3 backends)
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)
if (MSVC)
    target_compile_options(imgui_lib PRIVATE /utf-8)
endif()

add_executable(PlayerEditor
    src/main.cpp
    src/miniaudio_impl.cpp
    src/audio_clip.cpp
    src/player.cpp
    src/wav_io.cpp
    src/ffmpeg.cpp
    src/video_reader.cpp)
target_include_directories(PlayerEditor PRIVATE
    ${miniaudio_SOURCE_DIR}
    ${pfd_SOURCE_DIR})
target_link_libraries(PlayerEditor PRIVATE imgui_lib glfw opengl32)
if (MSVC)
    target_compile_options(PlayerEditor PRIVATE /utf-8)
endif()
if (WIN32)
    target_link_libraries(PlayerEditor PRIVATE ole32 winmm shell32)
    target_compile_definitions(PlayerEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # 関連付けアプリ用: コンソール窓を出さない(GUIサブシステム)。エントリは main() のまま。
    set_target_properties(PlayerEditor PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(PlayerEditor PRIVATE /ENTRY:mainCRTStartup)
endif()

# ---- ヘッドレス検証用 ----
add_executable(PlayerEditorTests
    tests/test_main.cpp
    src/miniaudio_impl.cpp
    src/audio_clip.cpp
    src/player.cpp
    src/wav_io.cpp
    src/ffmpeg.cpp
    src/video_reader.cpp)
target_include_directories(PlayerEditorTests PRIVATE ${miniaudio_SOURCE_DIR} src)
if (MSVC)
    target_compile_options(PlayerEditorTests PRIVATE /utf-8)
endif()
if (WIN32)
    target_link_libraries(PlayerEditorTests PRIVATE ole32 winmm)
    target_compile_definitions(PlayerEditorTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

cmake_minimum_required(VERSION 3.20)
set (CMAKE_TOOLCHAIN_FILE "C:/Program Files/vcpkg/scripts/buildsystems/vcpkg.cmake")
project(MusicVisualizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dear ImGui ──────────────────────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# ── SDL2 (install via: vcpkg install sdl2) ──────────────────────────────────
find_package(SDL2 REQUIRED)

# ── OpenGL ───────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(MusicVisualizer
    src/main.cpp
    src/audio_devices.cpp
    src/audio_capture.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(MusicVisualizer PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(MusicVisualizer PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
)

# On Windows, SDL2 provides WinMain — keep the standard main signature.
# ole32 + propsys are needed for COM and WASAPI device enumeration.
if(WIN32)
    target_link_libraries(MusicVisualizer PRIVATE SDL2::SDL2main ole32 propsys)
endif()

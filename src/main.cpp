#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "audio_devices.h"
#include "audio_capture.h"

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif
#include <GL/gl.h>

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

// ── Tab page forward declarations ─────────────────────────────────────────
static void DrawVisualizerTab(AudioCapture& capture);
static void DrawBeatVisualizerTab(AudioCapture& capture);
static void DrawSettingsTab(std::vector<AudioDevice>& devices, int& selectedDevice,
                            AudioCapture& capture);

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // COM must be initialised before any WASAPI calls
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Enumerate loopback devices and start capturing the default device immediately
    std::vector<AudioDevice> audioDevices = EnumerateLoopbackDevices();
    int selectedAudioDevice = 0;

    AudioCapture audioCapture;
    if (!audioDevices.empty())
        audioCapture.Start(audioDevices[0].id);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Request OpenGL 3.0 core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "Music Visualizer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // vsync

    // ── ImGui setup ──────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.WindowBorderSize  = 0.0f;
    style.TabRounding       = 4.0f;
    style.FrameRounding     = 3.0f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    // ── Main loop ─────────────────────────────────────────────────────────────
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ── Full-screen host window ──────────────────────────────────────────
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGui::Begin("##host", nullptr,
            ImGuiWindowFlags_NoTitleBar  |
            ImGuiWindowFlags_NoResize    |
            ImGuiWindowFlags_NoMove      |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::PopStyleVar();

        // ── Tab bar ─────────────────────────────────────────────────────────
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("  Visualizer  ")) {
                DrawVisualizerTab(audioCapture);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Beat Visualizer  ")) {
                DrawBeatVisualizerTab(audioCapture);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Settings  ")) {
                DrawSettingsTab(audioDevices, selectedAudioDevice, audioCapture);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::PopStyleVar();
        ImGui::End();

        // ── Render ───────────────────────────────────────────────────────────
        ImGui::Render();
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    audioCapture.Stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    CoUninitialize();
    return 0;
}

// ── Tab page implementations ──────────────────────────────────────────────

static void DrawVisualizerTab(AudioCapture& capture)
{
    ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 100.0f) size.x = 100.0f;
    if (size.y < 100.0f) size.y = 100.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, IM_COL32(12, 12, 18, 255));

    constexpr int   NUM_BARS = AudioCapture::NUM_BANDS;  // 32
    constexpr float MIN_FREQ = 40.0f;
    constexpr float MAX_FREQ = 20000.0f;

    const float label_area_h = 20.0f;
    const float bar_gap      = 3.0f;
    const float bars_w       = size.x;
    const float bar_w        = (bars_w - bar_gap * (NUM_BARS - 1)) / NUM_BARS;
    const float top_pad      = 8.0f;
    const float bars_h       = size.y - label_area_h - top_pad;
    const float bars_bot     = pos.y + top_pad + bars_h;

    // Fetch the latest band data from the capture thread
    float bands[NUM_BARS];
    capture.GetBands(bands);

    for (int i = 0; i < NUM_BARS; ++i) {
        const float norm   = static_cast<float>(i) / (NUM_BARS - 1); // 0..1
        const float freq   = MIN_FREQ * std::pow(MAX_FREQ / MIN_FREQ, norm);
        const float h_frac = std::clamp(bands[i], 0.0f, 1.0f);

        // ── Rainbow color: hue 0.0 (red) → 0.75 (violet) ─────────────────
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB((1.0f - norm) * 0.85f, 0.90f, 1.0f, r, g, b);
        ImU32 col_top = IM_COL32(
            static_cast<int>(r * 255),
            static_cast<int>(g * 255),
            static_cast<int>(b * 255), 255);
        ImU32 col_bot = IM_COL32(0, 0, 0, 255);

        // ── Draw bar ───────────────────────────────────────────────────────
        float x0 = pos.x + static_cast<float>(i) * (bar_w + bar_gap);
        float x1 = x0 + bar_w;
        float y0 = bars_bot - h_frac * bars_h;
        float y1 = bars_bot;

        dl->AddRectFilledMultiColor(
            {x0, y0}, {x1, y1},
            col_top, col_top,   // top edge — bright
            col_bot, col_bot);  // bottom edge — dim

        // ── Hz label on bar 0 and every other bar (0, 2, 4 … 30) ──────────
        if (i % 2 == 0) {
            char label[16];
            if (freq < 1000.0f) {
                std::snprintf(label, sizeof(label), "%dHz",
                    static_cast<int>(std::roundf(freq)));
            } else {
                float khz = freq / 1000.0f;
                if (std::fabsf(khz - std::roundf(khz)) < 0.05f)
                    std::snprintf(label, sizeof(label), "%dkHz",
                        static_cast<int>(std::roundf(khz)));
                else
                    std::snprintf(label, sizeof(label), "%.1fkHz", khz);
            }

            float lw = ImGui::CalcTextSize(label).x;
            float lx = x0 + bar_w * 0.5f - lw * 0.5f;
            float ly = bars_bot + 4.0f;
            dl->AddText({lx, ly}, IM_COL32(160, 160, 170, 255), label);
        }
    }

    // Capture-status overlay (top-right corner)
    if (!capture.IsRunning()) {
        const char* msg = "No capture device";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText({ pos.x + size.x - ts.x - 8.0f, pos.y + 6.0f },
            IM_COL32(220, 80, 80, 200), msg);
    }

    // Border
    dl->AddRect(pos, {pos.x + size.x, pos.y + size.y}, IM_COL32(55, 55, 75, 255));

    ImGui::Dummy(size);
}

static void DrawBeatVisualizerTab(AudioCapture& capture)
{
    ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 100.0f) size.x = 100.0f;
    if (size.y < 100.0f) size.y = 100.0f;

    const float dt = ImGui::GetIO().DeltaTime;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Fetch drum info ───────────────────────────────────────────────────────
    AudioCapture::DrumInfo drumInfo;
    capture.GetDrumInfo(drumInfo);

    static float kickAlpha  = 0.0f;
    static float snareAlpha = 0.0f;

    if (drumInfo.kickPulse)  kickAlpha  = 1.0f;
    if (drumInfo.snarePulse) snareAlpha = 1.0f;

    constexpr float kDecay = 3.5f;
    kickAlpha  = std::max(0.0f, kickAlpha  - dt * kDecay);
    snareAlpha = std::max(0.0f, snareAlpha - dt * kDecay);

    // ── Background ────────────────────────────────────────────────────────────
    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, IM_COL32(12, 12, 18, 255));

    const float midY = pos.y + size.y * 0.5f;

    // ── Snare flash: top half, orange at top → transparent at center ──────────
    if (snareAlpha > 0.0f) {
        int a = static_cast<int>(snareAlpha * 255);
        ImU32 orange = IM_COL32(255, 128, 0, a);
        ImU32 clear  = IM_COL32(0, 0, 0, 0);
        dl->AddRectFilledMultiColor(
            { pos.x, pos.y }, { pos.x + size.x, midY },
            orange, orange, clear, clear);
    }

    // ── Kick flash: bottom half, transparent at center → purple at bottom ─────
    if (kickAlpha > 0.0f) {
        int a = static_cast<int>(kickAlpha * 255);
        ImU32 purple = IM_COL32(160, 32, 240, a);
        ImU32 clear  = IM_COL32(0, 0, 0, 0);
        dl->AddRectFilledMultiColor(
            { pos.x, midY }, { pos.x + size.x, pos.y + size.y },
            clear, clear, purple, purple);
    }

    // ── Center divider ────────────────────────────────────────────────────────
    dl->AddLine({ pos.x, midY }, { pos.x + size.x, midY }, IM_COL32(55, 55, 75, 160));

    // ── Labels (dim at rest, bright on hit) ───────────────────────────────────
    ImFont* font     = ImGui::GetFont();
    float   fontSize = std::clamp(size.y * 0.14f, 20.0f, 56.0f);

    const char* snareLabel = "SNARE";
    ImVec2 snareSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, snareLabel);
    float  snareLX = pos.x + (size.x - snareSz.x) * 0.5f;
    float  snareLY = pos.y + size.y * 0.25f - snareSz.y * 0.5f;
    int    snareA  = static_cast<int>(50 + snareAlpha * 205);
    dl->AddText(font, fontSize, { snareLX + 2.0f, snareLY + 2.0f }, IM_COL32(0, 0, 0, snareA / 2), snareLabel);
    dl->AddText(font, fontSize, { snareLX, snareLY }, IM_COL32(255, 128, 0, snareA), snareLabel);

    const char* kickLabel = "KICK";
    ImVec2 kickSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, kickLabel);
    float  kickLX = pos.x + (size.x - kickSz.x) * 0.5f;
    float  kickLY = pos.y + size.y * 0.75f - kickSz.y * 0.5f;
    int    kickA  = static_cast<int>(50 + kickAlpha * 205);
    dl->AddText(font, fontSize, { kickLX + 2.0f, kickLY + 2.0f }, IM_COL32(0, 0, 0, kickA / 2), kickLabel);
    dl->AddText(font, fontSize, { kickLX, kickLY }, IM_COL32(160, 32, 240, kickA), kickLabel);

    // ── Capture status overlay ────────────────────────────────────────────────
    if (!capture.IsRunning()) {
        const char* msg = "No capture device";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText({ pos.x + size.x - ts.x - 8.0f, pos.y + 6.0f },
            IM_COL32(220, 80, 80, 200), msg);
    }

    dl->AddRect(pos, { pos.x + size.x, pos.y + size.y }, IM_COL32(55, 55, 75, 255));

    ImGui::Dummy(size);
}

static void DrawSettingsTab(std::vector<AudioDevice>& devices, int& selectedDevice,
                            AudioCapture& capture)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Audio");
    ImGui::Spacing();

    if (devices.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No loopback devices found.");
    } else {
        static std::vector<const char*> names;
        names.clear();
        for (const auto& d : devices)
            names.push_back(d.name.c_str());

        int prev = selectedDevice;
        ImGui::SetNextItemWidth(420.0f);
        ImGui::Combo("Capture Device", &selectedDevice, names.data(), static_cast<int>(names.size()));

        if (selectedDevice != prev) {
            capture.Stop();
            capture.Start(devices[selectedDevice].id);
        }
    }

    // Refresh button — re-enumerates then restarts capture on the new index 0
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        capture.Stop();
        devices = EnumerateLoopbackDevices();
        selectedDevice = 0;
        if (!devices.empty())
            capture.Start(devices[0].id);
    }

    // Capture status indicator
    ImGui::SameLine();
    if (capture.IsRunning())
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "  Capturing");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  Stopped");

    ImGui::Spacing();
    ImGui::SeparatorText("Appearance");
    ImGui::TextDisabled("Theme and color settings will go here.");

    ImGui::Spacing();
    ImGui::SeparatorText("About");
    ImGui::Text("Music Visualizer — built with Dear ImGui + SDL2 + OpenGL3");
}

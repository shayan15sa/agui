#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL3/SDL_opengles2.h>
#else
#include <SDL3/SDL_opengl.h>
#endif

#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif

#include <chrono>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <algorithm> 
#include <cctype>
#include <string>
#include <array>
#include <aria2/aria2.h>


inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    rtrim(result);
    return result;
}

int downloadEventCallback(aria2::Session *session, aria2::DownloadEvent event,
                          aria2::A2Gid gid, void *userData) {
    (void)userData;
    switch (event) {
    case aria2::EVENT_ON_DOWNLOAD_COMPLETE:
        std::cerr << "COMPLETE";
        break;
    case aria2::EVENT_ON_DOWNLOAD_ERROR:
        std::cerr << "ERROR";
        break;
    default:
        return 0;
    }
    std::cerr << " [" << aria2::gidToHex(gid) << "] ";
    aria2::DownloadHandle *dh = aria2::getDownloadHandle(session, gid);
    if (!dh)
        return 0;
    if (dh->getNumFiles() > 0) {
        aria2::FileData f = dh->getFile(1);
        // Path may be empty if the file name has not been determined yet.
        if (f.path.empty()) {
            if (!f.uris.empty()) {
                std::cerr << f.uris[0].uri;
            }
        } else {
            std::cerr << f.path;
        }
    }
    aria2::deleteDownloadHandle(dh);
    std::cerr << std::endl;
    return 0;
}


void doAria2(int argc, char **argv) {
    int rv = 0;
    aria2::libraryInit();
    // session is actually singleton: 1 session per process
    aria2::Session *session;
    // Create default configuration. The libaria2 takes care of signal
    // handling.
    aria2::SessionConfig config;
    // Add event callback
    config.downloadEventCallback = downloadEventCallback;
    session = aria2::sessionNew(aria2::KeyVals(), config);
    // Add download item to session
    for (int i = 1; i < argc; ++i) {
        std::vector<std::string> uris = {argv[i]};
        aria2::KeyVals options;
        rv = aria2::addUri(session, nullptr, uris, options);
        if (rv < 0) {
            std::cerr << "Failed to add download " << uris[0] << std::endl;
        }
    }
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        rv = aria2::run(session, aria2::RUN_ONCE);
        if (rv != 1) {
            break;
        }
        // the application can call aria2 API to add URI or query progress
        // here
        auto now = std::chrono::steady_clock::now();
        auto count =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                .count();
        // Print progress information once per 500ms
        if (count >= 500) {
            start = now;
            aria2::GlobalStat gstat = aria2::getGlobalStat(session);
            std::cerr << "Overall #Active:" << gstat.numActive
                      << " #waiting:" << gstat.numWaiting
                      << " D:" << gstat.downloadSpeed / 1024 << "KiB/s"
                      << " U:" << gstat.uploadSpeed / 1024 << "KiB/s " << std::endl;
            std::vector<aria2::A2Gid> gids = aria2::getActiveDownload(session);
            for (const auto &gid : gids) {
                aria2::DownloadHandle *dh = aria2::getDownloadHandle(session, gid);
                if (dh) {
                    std::cerr << "    [" << aria2::gidToHex(gid) << "] "
                              << dh->getCompletedLength() << "/" << dh->getTotalLength()
                              << "("
                              << (dh->getTotalLength() > 0
                                      ? (100 * dh->getCompletedLength() /
                                         dh->getTotalLength())
                                      : 0)
                              << "%)"
                              << " D:" << dh->getDownloadSpeed() / 1024
                              << "KiB/s, U:" << dh->getUploadSpeed() / 1024 << "KiB/s"
                              << std::endl;
                    aria2::deleteDownloadHandle(dh);
                }
            }
        }
    }
    rv = aria2::sessionFinal(session);
    aria2::libraryDeinit();
}


int main() {
    // Setup SDL
    // [If using SDL_MAIN_USE_CALLBACKS: all code below until the main loop starts would likely be your SDL_AppInit() function]
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // Select GL version + let the backend select a GLSL version
    const char* glsl_version = nullptr;
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GLES 3.0 + GLSL 300 es (WebGL 2.0)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // GL 3.2 Core + generally GLSL 150
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + generally GLSL 130
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    // float main_scale = 1.5f;
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("Dear ImGui SDL3+OpenGL3 example", (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr)
    {
        printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (in docking branch: using io.ConfigDpiScaleFonts=true automatically overrides this for every window depending on the current monitor)

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If fonts are not explicitly loaded, Dear ImGui will select an embedded font: either AddFontDefaultVector() or AddFontDefaultBitmap().
    //   This selection is based on (style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi) reaching a small threshold.
    // - You can load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - If a file cannot be loaded, AddFont functions will return a nullptr. Please handle those errors in your code (e.g. use an assertion, display an error and quit).
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use FreeType for higher quality font rendering.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    // - Our Emscripten build process allows embedding fonts to be accessible at runtime from the "fonts/" folder. See Makefile.emscripten for details.
    //style.FontSizeBase = 20.0f;
    //io.Fonts->AddFontDefaultVector();
    //io.Fonts->AddFontDefaultBitmap();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
    //IM_ASSERT(font != nullptr);
    style.FontSizeBase = 20.0f;
    io.Fonts->AddFontFromFileTTF("./InterVariable.ttf");
    bool done = false;
    bool show_demo_window = false;
    bool show_my_window = true;
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        // [If using SDL_MAIN_USE_CALLBACKS: all code below would likely be your SDL_AppIterate() function]
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        static char name[256] = {0};
        static std::string content = "";
        if (show_my_window) {
            ImGui::Begin("main", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
            ImGui::SetWindowPos({0, 0});
            int x = 0;
            int y = 0;
            SDL_GetWindowSize(window, &x, &y);
            ImGui::SetWindowSize({(float) x, (float) y});
            ImGui::Text("Please enter your name: ");
            ImGui::InputTextWithHint("##", "Name", name, sizeof name);
            if (ImGui::Button("Pick a file")) {
                std::string file_path = exec("zenity --file-selection");
                std::cout << file_path;
                std::string file_buffer;
                std::ifstream the_file(file_path);
                std::cout << the_file.is_open();
                while (getline(the_file, file_buffer)) {
                    content += file_buffer;
                    content += "\n";
                }
                for (int i = file_path.size() - 1; i > 0 ; --i) {
                    if (file_path[i] == '/') {
                        std::string name = file_path.substr(i + 1, file_path.size() - i + 1);
                        std::cout << name << std::endl;
                        break;
                    }
                }
            }
            ImGui::TextWrapped("%s" ,content.c_str());
            ImGui::End();
        }

        // ImGui::Begin("abbas");
        // ImGui::Text(name);
        // ImGui::End();
        
        
        if (show_demo_window) {
          ImGui::ShowDemoWindow(&show_demo_window);
        }

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

    }
    // int rv;
    // if (argc < 2) {
    //   std::cerr << "Usage: libaria2ex URI [URI...]\n"
    //             << "\n"
    //             << "  Download given URIs in parallel in the current directory."
    //             << std::endl;
    //   exit(EXIT_SUCCESS);
    // }
    //     // Cleanup
    // [If using SDL_MAIN_USE_CALLBACKS: all code below would likely be your SDL_AppQuit() function]
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

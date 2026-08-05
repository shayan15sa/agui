#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <queue>
#include <stdio.h>
#include <thread>
#include <mutex>
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
#include <atomic>
#include <vector>
#include <aria2/aria2.h>

// Struct to safely store snapshot data without lifetime or dangling pointer issues
struct DownloadInfo {
    std::string gid;
    std::string dir;
    int64_t completedLength = 0;
    int64_t totalLength = 0;
    int downloadSpeed = 0;
    int uploadSpeed = 0;
};

struct AddDownloadInfo {
    std::string url;
    std::string folderPath;
};


aria2::Session *session;
std::mutex dhsMutex;
std::vector<DownloadInfo> dhs;
aria2::GlobalStat gstat;

// Store the last selected download folder
std::string downloadFolder = ".";

std::mutex uriQueueMutex;
std::queue<AddDownloadInfo> pendingUris;
std::atomic<bool> keepRunning{true};

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

void doAria2() {
    int rv = 0;
    auto start = std::chrono::steady_clock::now();

    while (keepRunning) {
        // 1. Process queued URIs
        {
            std::lock_guard<std::mutex> lock(uriQueueMutex);
            while (!pendingUris.empty()) {
                AddDownloadInfo adi = pendingUris.front();
                pendingUris.pop();

                std::vector<std::string> uris = {adi.url};
                aria2::KeyVals options;
                options.push_back(std::pair<std::string, std::string> ("dir", adi.folderPath));
                rv = aria2::addUri(session, nullptr, uris, options);
                if (rv < 0) {
                    std::cerr << "Failed to add download: " << adi.url << std::endl;
                }
            }
        }

        // 2. Advance aria2 engine tick
        rv = aria2::run(session, aria2::RUN_ONCE);
        if (rv != 1) {
            // No active downloads running; sleep 20ms to avoid high CPU usage
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // 3. Snapshot stats every 500ms
        auto now = std::chrono::steady_clock::now();
        auto count = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        if (count >= 500) {
            start = now;
            gstat = aria2::getGlobalStat(session);
            std::vector<aria2::A2Gid> gids = aria2::getActiveDownload(session);

            std::vector<DownloadInfo> currentDownloads;
            currentDownloads.reserve(gids.size());

            for (const auto &gid : gids) {
                aria2::DownloadHandle *dh = aria2::getDownloadHandle(session, gid);
                if (dh) {
                    DownloadInfo info;
                    info.gid = aria2::gidToHex(gid);
                    info.dir = dh->getDir();
                    info.completedLength = dh->getCompletedLength();
                    info.totalLength = dh->getTotalLength();
                    info.downloadSpeed = dh->getDownloadSpeed();
                    info.uploadSpeed = dh->getUploadSpeed();

                    currentDownloads.push_back(info);
                    aria2::deleteDownloadHandle(dh);
                }
            }

            {
                std::lock_guard<std::mutex> lock(dhsMutex);
                dhs = std::move(currentDownloads);
            }
        }
    }

    // Clean teardown on application exit
    aria2::sessionFinal(session);
    aria2::libraryDeinit();
}

int main() {
    int rv = 0;
    aria2::libraryInit();

    aria2::SessionConfig config;
    config.downloadEventCallback = downloadEventCallback;
    config.keepRunning = true;
    session = aria2::sessionNew(aria2::KeyVals(), config);
    std::thread aria2Thread(doAria2);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl_version = nullptr;
#if defined(IMGUI_IMPL_OPENGL_ES2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("Dear ImGui SDL3+OpenGL3 example", (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (window == nullptr) {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr) {
        printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

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

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

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
            ImGui::Text("Download Settings:");
            
            // Show current download folder with change button
            // {
            //     ImGui::PushID("folder_display");
            //     ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.5f, 1.0f), "Download folder: %s", downloadFolder.c_str());
            //     if (ImGui::Button("Change folder")) {
            //         std::string folder_path = exec("zenity --file-selection --directory");
            //         if (!folder_path.empty()) {
            //             std::string folder_name = folder_path;
            //             for (int i = folder_path.size() - 1; i >= 0; --i) {
            //                 if (folder_path[i] == '/' || folder_path[i] == '\\') {
            //                     folder_name = folder_path.substr(i + 1, folder_path.size() - i - 1);
            //                     break;
            //                 }
            //             }
            //             downloadFolder = folder_path;
            //             std::cout << "New folder: " << downloadFolder << std::endl;
            //         }
            //     }
            //     ImGui::PopID();
            // }

            // URL input field
            ImGui::InputText("##url", name, sizeof name);
            ImGui::SetItemTooltip("Enter URL to download");

            if (ImGui::Button("Pick download folder")) {
                std::string folder_path = exec("zenity --file-selection --directory");
                if (!folder_path.empty()) {
                    downloadFolder = folder_path;
                }
                std::cout << "Selected folder: " << folder_path << std::endl;
            }

            if (ImGui::Button("Download")) {
                if (strlen(name) > 0) {
                    // Get the download folder path
                    std::string folder_path = downloadFolder;
                    
                    std::cout << "Downloading: " << name << std::endl;
                    // Add to download queue with full path
                    std::lock_guard<std::mutex> lock(uriQueueMutex);
                    AddDownloadInfo aid = {
                        .url = name,
                        .folderPath = downloadFolder
                    };
                    pendingUris.push(aid);
                    name[0] = '\0';
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "Please enter a URL first");
                }
            }

            // Beautiful download manager display with folder info
            {
                std::lock_guard<std::mutex> lock(dhsMutex);
                if (dhs.empty()) {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No active downloads");
                    // Show current download folder
                    ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.4f, 1.0f), "Download folder: %s", downloadFolder.c_str());
                } else {
                    ImGui::Separator();
                    ImGui::Text("Downloads (%d)", dhs.size());
                    ImGui::Separator();
                    // Show download folder
                    ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.4f, 1.0f), "Saving to: %s", downloadFolder.c_str());

                    for (const auto &info : dhs) {
                        // Download card header with status
                        {
                            ImGui::PushID(info.gid.c_str());
                            
                            // Status color based on speed
                            ImVec4 statusColor;
                            int speed = info.downloadSpeed / 1024;
                            if (speed > 100) {
                                statusColor = ImVec4(0.2f, 0.8f, 0.9f, 1.0f); // Fast - cyan
                            } else if (speed > 10) {
                                statusColor = ImVec4(0.3f, 0.7f, 0.5f, 1.0f); // Normal - green
                            } else {
                                statusColor = ImVec4(0.8f, 0.5f, 0.3f, 1.0f); // Slow - orange
                            }
                            
                            ImGui::TextColored(statusColor, "%s", info.gid.c_str());
                            
                            // Progress bar
                            float progress = (float)info.completedLength / info.totalLength;
                            if (info.totalLength > 0) {
                                ImGui::ProgressBar(progress, ImVec2(-1, 20));
                            }
                            
                            ImGui::PopID();
                        }
                        
                        // Detailed info row
                        {
                            ImGui::PushID(info.gid.c_str());
                            
                            // Calculate download speed display
                            int speed_kib = info.downloadSpeed / 1024;
                            char speedStr[32];
                            if (speed_kib >= 1000) {
                                snprintf(speedStr, sizeof(speedStr), "%.2f MB/s", speed_kib / 1000.0f);
                            } else if (speed_kib > 0) {
                                snprintf(speedStr, sizeof(speedStr), "%d KiB/s", speed_kib);
                            } else {
                                snprintf(speedStr, sizeof(speedStr), "0 KiB/s");
                            }
                            
                            // Time remaining estimate
                            char timeStr[32];
                            if (info.totalLength > info.completedLength) {
                                double remaining = info.totalLength - info.completedLength;
                                double speed = info.downloadSpeed / 1024.0;
                                if (speed > 0) {
                                    double seconds = remaining / speed;
                                    if (seconds >= 3600) {
                                        snprintf(timeStr, sizeof(timeStr), "~ %d hr", (int)(seconds / 3600));
                                    } else if (seconds >= 60) {
                                        snprintf(timeStr, sizeof(timeStr), "~ %d min", (int)(seconds / 60));
                                    } else {
                                        snprintf(timeStr, sizeof(timeStr), "~ %d sec", (int)seconds);
                                    }
                                } else {
                                    snprintf(timeStr, sizeof(timeStr), "Paused");
                                }
                            } else {
                                snprintf(timeStr, sizeof(timeStr), "Complete");
                            }
                            
                            // Main info line
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), 
                                             "  %s | %s | %s", 
                                             timeStr,
                                             speedStr,
                                             info.dir.c_str());
                            
                            ImGui::PopID();
                        }
                        
                        // Progress details
                        {
                            ImGui::PushID(info.gid.c_str());
                            
                            char percentStr[32];
                            float percent = (float)info.completedLength / info.totalLength * 100.0f;
                            snprintf(percentStr, sizeof(percentStr), "%.1f%%", percent);
                            
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                             "  Progress: %s / %lld bytes", 
                                             percentStr,
                                             (long long)info.totalLength);
                            
                            ImGui::PopID();
                        }
                    }
                }
            }

            ImGui::End();
        }

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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // Detach or join background thread on shutdown
    keepRunning = false;
    if (aria2Thread.joinable()) {
        aria2Thread.join();
    }

    return 0;
}

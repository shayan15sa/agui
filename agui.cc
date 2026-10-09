#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <queue>
#include <deque>
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

#include <SDL3/SDL_tray.h>


#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <chrono>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <array>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <cfloat>

#include <aria2/aria2.h>

// Font from c files
#include "Inter.cpp"
#include "FaSolid.cpp"

// ============================================================================
// FontAwesome 6 Free Solid glyphs (vendor/fonts/fa-solid-900.ttf)
// ============================================================================

#define ICON_FA_DOWNLOAD              "\xef\x80\x99" // U+F019
#define ICON_FA_ARROW_UP              "\xef\x81\xa2" // U+F062
#define ICON_FA_ARROW_DOWN            "\xef\x81\xa3" // U+F063
#define ICON_FA_PAUSE                 "\xef\x81\x8c" // U+F04C
#define ICON_FA_PLAY                  "\xef\x81\x8b" // U+F04B
#define ICON_FA_XMARK                 "\xef\x80\x8d" // U+F00D
#define ICON_FA_CHECK                 "\xef\x80\x8c" // U+F00C
#define ICON_FA_CIRCLE_CHECK          "\xef\x81\x98" // U+F058
#define ICON_FA_TRIANGLE_EXCLAMATION  "\xef\x81\xb1" // U+F071
#define ICON_FA_FOLDER                "\xef\x81\xbb" // U+F07B
#define ICON_FA_FOLDER_OPEN           "\xef\x81\xbc" // U+F07C
#define ICON_FA_PASTE                 "\xef\x83\xaa" // U+F0EA
#define ICON_FA_CLOCK                 "\xef\x80\x97" // U+F017
#define ICON_FA_TRASH                 "\xef\x87\xb8" // U+F1F8
#define ICON_FA_OPEN_EXTERNAL         "\xef\x82\x8e" // U+F08E
#define ICON_FA_WINDOW_MINIMIZE       "\xef\x8b\x91" // U+F2D1
#define ICON_FA_POWER_OFF             "\xef\x80\x91" // U+F011

// ============================================================================
// Small string/format helpers
// ============================================================================

// Path separators: '/' everywhere, plus '\' on Windows.
#ifdef _WIN32
static constexpr const char* kPathSeparators = "/\\";
#else
static constexpr const char* kPathSeparators = "/";
#endif

static std::string baseNameOf(const std::string& path) {
    size_t pos = path.find_last_of(kPathSeparators);
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

static std::string displayNameFromUrl(const std::string& url) {
    size_t schemeEnd = url.find("://");
    std::string rest = (schemeEnd == std::string::npos) ? url : url.substr(schemeEnd + 3);
    size_t slash = rest.find('/');
    std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
    size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);
    std::string name = baseNameOf(path);
    if (name.empty()) name = host;
    return name;
}

static std::string directoryOf(const std::string& path) {
    size_t pos = path.find_last_of(kPathSeparators);
    if (pos == std::string::npos) return ".";
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

static std::string fmtBytes(int64_t bytes) {
    if (bytes <= 0) return "0 B";
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; ++u; }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

static std::string fmtSpeed(int64_t bytesPerSec) {
    if (bytesPerSec <= 0) return "0 B/s";
    return fmtBytes(bytesPerSec) + "/s";
}

static std::string fmtETA(double seconds) {
    if (seconds < 0 || seconds > 86400.0 * 30.0) return "--";
    if (seconds >= 3600.0) {
        int h = (int)(seconds / 3600);
        int m = (int)((seconds - h * 3600) / 60);
        char buf[32];
        snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
        return buf;
    }
    if (seconds >= 60.0) {
        int m = (int)(seconds / 60);
        int s = (int)(seconds - m * 60);
        char buf[32];
        snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%ds", std::max(1, (int)seconds));
    return buf;
}


#ifdef _WIN32
static std::wstring win32Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize((size_t)(n - 1));
    return w;
}

// Quote one argv element following CommandLineToArgvW rules.
static void win32AppendArg(std::wstring& cmd, const std::string& arg) {
    std::wstring w = win32Widen(arg);
    if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += w;
        return;
    }
    cmd += L'"';
    size_t backslashes = 0;
    for (wchar_t ch : w) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') cmd.append(backslashes * 2 + 1, L'\\');
        else if (backslashes > 0) { cmd.append(backslashes, L'\\'); backslashes = 0; }
        cmd += ch;
    }
    if (backslashes > 0) cmd.append(backslashes * 2, L'\\');
    cmd += L'"';
}
#else
#if defined(__APPLE__)
static char** aguiEnviron() { return *_NSGetEnviron(); }
#else
extern char **environ;
static char** aguiEnviron() { return environ; }
#endif
#endif

static int runDetached(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
#ifdef _WIN32
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd += L' ';
        win32AppendArg(cmd, argv[i]);
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | DETACHED_PROCESS,
                         nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), aguiEnviron()) != 0)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

// Fire-and-forget launch on a detached thread.
static void launchDetached(const std::vector<std::string>& argv) {
    std::thread([argv] { (void)runDetached(argv); }).detach();
}

static std::string errorString(int code) {
    switch (code) {
    case 0:  return "";
    case 1:  return "Unknown error";
    case 2:  return "Timed out";
    case 3:  return "Resource not found";
    case 4:  return "Multiple errors";
    case 5:  return "Resume not supported";
    case 6:  return "Range not supported";
    case 7:  return "Authentication failed";
    default: {
        char buf[48];
        snprintf(buf, sizeof(buf), "Error code %d", code);
        return buf;
    }
    }
}

#ifdef _WIN32
static std::mutex g_notifyMutex;

// Message-only window hosting our notification-area icon, created lazily so
// notifications work even before (or without) the main SDL window.
static HWND win32NotifyHost() {
    static HWND hwnd = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AguiNotifyHost";
        if (RegisterClassW(&wc)) {
            hwnd = CreateWindowExW(0, wc.lpszClassName, L"agui", 0,
                                   0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   wc.hInstance, nullptr);
        }
    });
    return hwnd;
}

// Balloon notification via our own tray icon. The icon is removed a few
// seconds after the balloon is shown.
static void win32NotifyBalloon(const std::string& title, const std::string& msg) {
    HWND hwnd = win32NotifyHost();
    if (!hwnd) return;
    std::wstring wTitle = win32Widen(title);
    std::wstring wMsg = win32Widen(msg);
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_APP + 101;
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcsncpy(nid.szTip, L"agui", ARRAYSIZE(nid.szTip));
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uFlags = NIF_INFO;
        wcsncpy(nid.szInfo, wMsg.c_str(), ARRAYSIZE(nid.szInfo) - 1);
        wcsncpy(nid.szInfoTitle, wTitle.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    Sleep(6000);
    {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}
#endif

#if defined(__APPLE__)
static std::string appleScriptEscape(const std::string& s) {
    std::string r;
    for (char ch : s) {
        if (ch == '\\' || ch == '"') r += '\\';
        r += ch;
    }
    return r;
}
#endif

static void desktopNotify(const std::string& title, const std::string& msg) {
#ifdef _WIN32
    std::thread([title, msg] { win32NotifyBalloon(title, msg); }).detach();
#elif defined(__APPLE__)
    launchDetached({"/usr/bin/osascript", "-e",
                    "display notification \"" + appleScriptEscape(msg) +
                    "\" with title \"" + appleScriptEscape(title) + "\""});
#else
    launchDetached({"notify-send", title, msg});
#endif
}

static void openPathOrUrl(const std::string& target) {
    if (target.empty()) return;
#ifdef _WIN32
    std::wstring w = win32Widen(target);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    launchDetached({"/usr/bin/open", target});
#else
    launchDetached({"xdg-open", target});
#endif
}

// ============================================================================
// Shared data model (guarded by g_dlMutex)
// ============================================================================

struct DownloadInfo {
    std::string gid;
    std::string name;   // display name (file basename, derived from URL as fallback)
    std::string path;   // full path of first file, may be empty early on
    std::string url;    // source URL
    int64_t completedLength = 0;
    int64_t totalLength = 0;
    int downloadSpeed = 0;
    int uploadSpeed = 0;
    aria2::DownloadStatus status = aria2::DOWNLOAD_WAITING;
    int errorCode = 0;
};

enum class CmdType { Pause, Resume, Remove, PauseAll, ResumeAll };
struct Command { CmdType type; std::string gid; };

struct AddDownloadInfo {
    std::string url;
    std::string folderPath;
};

static aria2::Session* g_session = nullptr;

static std::mutex g_dlMutex;
static std::unordered_map<std::string, DownloadInfo> g_downloads;
static std::vector<std::string> g_order;   // insertion order of gids
static aria2::GlobalStat g_gstat;

static std::mutex g_cmdMutex;
static std::queue<Command> g_cmds;

static std::mutex g_uriQueueMutex;
static std::queue<AddDownloadInfo> g_pendingUris;

static std::atomic<bool> g_keepRunning{true};

// Folder-picker result handoff (dialog callback thread -> UI thread)
static std::mutex g_pickMutex;
static bool g_folderPicked = false;
static std::string g_pickedFolder;

// Last used download folder (persisted)
static std::string g_downloadFolder = ".";

// Main SDL window (for parenting dialogs)
static SDL_Window* g_window = nullptr;
static SDL_GLContext g_gl_context = nullptr;
static float g_main_scale = 0.0f;

// ---------------------------------------------------------------------------
// Window visibility / tray state
//
// The window is never destroyed while the app lives: closing it only hides it
// so downloads keep running in the background. SDL tray callbacks run on the
// main thread and only flip atomics / enqueue commands.
// ---------------------------------------------------------------------------

static std::atomic<bool> g_done{false};          // real quit requested
static std::atomic<bool> g_windowVisible{true};  // owned by the SDL thread
static std::atomic<int>  g_visibilityRequest{0}; // -1 hide, +1 show, 0 none
static std::atomic<bool> g_trayActive{false};    // tray successfully created

static void requestHideWindow() { g_visibilityRequest.store(-1); }
static void requestToggleWindow() {
    g_visibilityRequest.store(g_windowVisible.load() ? -1 : 1);
}
static void requestQuit() { g_done.store(true); }

static void updateTray();

static void upsertDownloadLocked(const DownloadInfo& info) {
    if (g_downloads.count(info.gid) == 0) g_order.push_back(info.gid);
    g_downloads[info.gid] = info;
}

static void eraseDownloadLocked(const std::string& gid) {
    g_downloads.erase(gid);
    g_order.erase(std::remove(g_order.begin(), g_order.end(), gid), g_order.end());
}

// Resolve display name preferring real file paths over URL guesses.
static void fillFromHandle(DownloadInfo& info, aria2::DownloadHandle* dh) {
    info.completedLength = dh->getCompletedLength();
    info.totalLength = dh->getTotalLength();
    info.downloadSpeed = dh->getDownloadSpeed();
    info.uploadSpeed = dh->getUploadSpeed();
    info.status = dh->getStatus();
    info.errorCode = dh->getErrorCode();

    if (dh->getNumFiles() > 0) {
        aria2::FileData f = dh->getFile(1);
        if (!f.path.empty()) info.path = f.path;
        if (info.url.empty() && !f.uris.empty()) info.url = f.uris[0].uri;
    }

    std::string resolved;
    if (!info.path.empty()) resolved = baseNameOf(info.path);
    else if (!info.url.empty()) resolved = displayNameFromUrl(info.url);
    if (!resolved.empty()) info.name = resolved;
    if (info.name.empty()) info.name = "download-" + info.gid.substr(0, 8);
}

static void enqueueCommand(CmdType type, const std::string& gid) {
    std::lock_guard<std::mutex> lock(g_cmdMutex);
    g_cmds.push({type, gid});
}

// ============================================================================
// aria2 event callback (runs on the aria2 thread, inside run())
// ============================================================================

static int downloadEventCallback(aria2::Session*, aria2::DownloadEvent event,
                                 aria2::A2Gid gid, void* userData) {
    (void)userData;
    switch (event) {
    case aria2::EVENT_ON_DOWNLOAD_COMPLETE:
    case aria2::EVENT_ON_DOWNLOAD_ERROR: {
        bool failed = (event == aria2::EVENT_ON_DOWNLOAD_ERROR);
        std::string gidStr = aria2::gidToHex(gid);

        DownloadInfo info;
        info.gid = gidStr;
        if (aria2::DownloadHandle* dh = aria2::getDownloadHandle(g_session, gid)) {
            fillFromHandle(info, dh);
            aria2::deleteDownloadHandle(dh);
        }
        if (info.name.empty()) info.name = gidStr.substr(0, 12);

        {
            std::lock_guard<std::mutex> lock(g_dlMutex);
            upsertDownloadLocked(info);
        }
        desktopNotify(failed ? "Download failed" : "Download complete", info.name);
        break;
    }
    default:
        break;
    }
    return 0;
}

// ============================================================================
// aria2 worker thread
// ============================================================================

static void processPendingCommands() {
    std::vector<Command> batch;
    {
        std::lock_guard<std::mutex> cmdLock(g_cmdMutex);
        while (!g_cmds.empty()) {
            batch.push_back(g_cmds.front());
            g_cmds.pop();
        }
    }

    for (const Command& c : batch) {
        if (c.type == CmdType::PauseAll || c.type == CmdType::ResumeAll) {
            // Collect the affected gids from our own model: aria2 only exposes
            // active downloads directly, and we also want queued/paused ones.
            std::vector<std::string> gids;
            {
                std::lock_guard<std::mutex> lock(g_dlMutex);
                for (const std::string& gidStr : g_order) {
                    auto it = g_downloads.find(gidStr);
                    if (it == g_downloads.end()) continue;
                    aria2::DownloadStatus st = it->second.status;
                    bool match = (c.type == CmdType::PauseAll)
                                     ? (st == aria2::DOWNLOAD_ACTIVE || st == aria2::DOWNLOAD_WAITING)
                                     : (st == aria2::DOWNLOAD_PAUSED);
                    if (match) gids.push_back(gidStr);
                }
            }
            for (const std::string& gidStr : gids) {
                aria2::A2Gid gid = aria2::hexToGid(gidStr);
                if (aria2::isNull(gid)) continue;
                if (c.type == CmdType::PauseAll) aria2::pauseDownload(g_session, gid);
                else                             aria2::unpauseDownload(g_session, gid);
            }
            continue;
        }

        aria2::A2Gid gid = aria2::hexToGid(c.gid);
        if (aria2::isNull(gid)) continue;
        switch (c.type) {
        case CmdType::Pause:  aria2::pauseDownload(g_session, gid); break;
        case CmdType::Resume: aria2::unpauseDownload(g_session, gid); break;
        case CmdType::Remove: aria2::removeDownload(g_session, gid, /*force=*/true); break;
        default: break;
        }
    }
}

static void addPendingUris() {
    std::lock_guard<std::mutex> uriLock(g_uriQueueMutex);
    while (!g_pendingUris.empty()) {
        AddDownloadInfo adi = g_pendingUris.front();
        g_pendingUris.pop();

        std::vector<std::string> uris = {adi.url};
        aria2::KeyVals options;
        options.push_back({"dir", adi.folderPath});

        aria2::A2Gid gid;
        int rv = aria2::addUri(g_session, &gid, uris, options);
        if (rv < 0 || aria2::isNull(gid)) {
            std::cerr << "Failed to add download: " << adi.url << std::endl;
            continue;
        }
        DownloadInfo info;
        info.gid = aria2::gidToHex(gid);
        info.url = adi.url;
        info.name = displayNameFromUrl(adi.url);
        info.status = aria2::DOWNLOAD_WAITING;
        std::lock_guard<std::mutex> lock(g_dlMutex);
        upsertDownloadLocked(info);
    }
}

static void snapshotDownloads() {
    aria2::GlobalStat gs = aria2::getGlobalStat(g_session);
    std::vector<aria2::A2Gid> actives = aria2::getActiveDownload(g_session);

    std::unordered_set<std::string> activeSet;
    for (aria2::A2Gid gid : actives) activeSet.insert(aria2::gidToHex(gid));

    std::lock_guard<std::mutex> lock(g_dlMutex);

    // Make sure every active download has an entry (e.g. ones we did not add ourselves).
    for (const std::string& gidStr : activeSet) {
        if (g_downloads.count(gidStr) == 0) {
            DownloadInfo info;
            info.gid = gidStr;
            info.status = aria2::DOWNLOAD_ACTIVE;
            upsertDownloadLocked(info);
        }
    }

    // Refresh every tracked download that is not in a terminal state yet.
    std::vector<std::string> toDrop;
    for (const std::string& gidStr : g_order) {
        auto it = g_downloads.find(gidStr);
        if (it == g_downloads.end()) continue;
        DownloadInfo& d = it->second;

        bool terminal = d.status == aria2::DOWNLOAD_COMPLETE ||
                        d.status == aria2::DOWNLOAD_ERROR ||
                        d.status == aria2::DOWNLOAD_REMOVED;
        if (terminal) continue;

        aria2::A2Gid gid = aria2::hexToGid(gidStr);
        if (aria2::isNull(gid)) { toDrop.push_back(gidStr); continue; }

        if (aria2::DownloadHandle* dh = aria2::getDownloadHandle(g_session, gid)) {
            fillFromHandle(d, dh);
            aria2::deleteDownloadHandle(dh);
            if (d.status == aria2::DOWNLOAD_REMOVED) toDrop.push_back(gidStr);
        } else {
            toDrop.push_back(gidStr);
        }
    }
    for (const std::string& gidStr : toDrop) eraseDownloadLocked(gidStr);

    g_gstat = gs;
}

static void doAria2() {
    auto lastSnapshot = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);

    while (g_keepRunning) {
        processPendingCommands();
        addPendingUris();

        int rv = aria2::run(g_session, aria2::RUN_ONCE);
        if (rv != 1) {
            // Nothing active; avoid busy-spinning.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastSnapshot >= std::chrono::milliseconds(500)) {
            lastSnapshot = now;
            snapshotDownloads();
        }
    }

    aria2::sessionFinal(g_session);
    aria2::libraryDeinit();
}

// ============================================================================
// Config persistence (per-platform location via SDL_GetPrefPath, with
// migration from the historical ~/.config/agui/config on POSIX systems)
// ============================================================================

static std::filesystem::path legacyConfigPath() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "agui" / "config";
    const char* home = getenv("HOME");
    if (home && *home) return std::filesystem::path(home) / ".config" / "agui" / "config";
    return "agui.config";
}

static const std::filesystem::path& configPath() {
    static const std::filesystem::path p = [] {
        std::error_code ec;
        char* pref = SDL_GetPrefPath("agui", "agui");
        std::filesystem::path modern;
        if (pref) {
            modern = std::filesystem::path(pref) / "config";
            SDL_free(pref);
        }
        if (!modern.empty()) {
            // Keep existing setups where they are.
            if (std::filesystem::exists(modern, ec)) return modern;
            std::filesystem::path legacy = legacyConfigPath();
            if (std::filesystem::exists(legacy, ec)) return legacy;
            return modern;
        }
        return legacyConfigPath();
    }();
    return p;
}

static void loadConfig() {
    std::ifstream in(configPath());
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "dir") g_downloadFolder = val;
    }
}

static void saveConfig() {
    std::error_code ec;
    std::filesystem::path p = configPath();
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    if (out) out << "dir=" << g_downloadFolder << "\n";
}

// ============================================================================
// Theme
// ============================================================================

static ImVec4 Col(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

namespace palette {
    static const ImVec4 bg       = Col(15, 19, 28);    // app background
    static const ImVec4 surface  = Col(22, 27, 39);    // cards / panels
    static const ImVec4 surfaceHi= Col(28, 34, 49);    // hover surfaces
    static const ImVec4 frame    = Col(28, 34, 49);
    static const ImVec4 frameHov = Col(38, 47, 67);
    static const ImVec4 frameAct = Col(47, 58, 82);
    static const ImVec4 text     = Col(233, 237, 243);
    static const ImVec4 textDim  = Col(140, 150, 168);
    static const ImVec4 accent   = Col(66, 183, 255);  // cyan-blue
    static const ImVec4 green    = Col(52, 211, 153);
    static const ImVec4 orange   = Col(251, 180, 84);
    static const ImVec4 red      = Col(251, 113, 133);
    static const ImVec4 sep      = Col(35, 43, 58);
    static const ImVec4 popup    = Col(20, 26, 38, 250);
} // namespace palette

static void setupTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.ChildRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 12.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 0.0f;
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.WindowPadding = ImVec2(14, 12);
    s.ScrollbarSize = 13.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]           = palette::bg;
    c[ImGuiCol_ChildBg]            = palette::surface;
    c[ImGuiCol_PopupBg]            = palette::popup;
    c[ImGuiCol_Border]             = palette::sep;
    c[ImGuiCol_Text]               = palette::text;
    c[ImGuiCol_TextDisabled]       = palette::textDim;
    c[ImGuiCol_FrameBg]            = palette::frame;
    c[ImGuiCol_FrameBgHovered]     = palette::frameHov;
    c[ImGuiCol_FrameBgActive]      = palette::frameAct;
    c[ImGuiCol_TitleBg]            = palette::bg;
    c[ImGuiCol_TitleBgActive]      = palette::bg;
    c[ImGuiCol_MenuBarBg]          = palette::surface;
    c[ImGuiCol_ScrollbarBg]         = Col(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]      = Col(40, 50, 70);
    c[ImGuiCol_ScrollbarGrabHovered] = Col(52, 64, 89);
    c[ImGuiCol_ScrollbarGrabActive]  = Col(66, 80, 111);
    c[ImGuiCol_Separator]          = palette::sep;
    c[ImGuiCol_CheckMark]          = palette::accent;
    c[ImGuiCol_SliderGrab]         = palette::accent;
    c[ImGuiCol_SliderGrabActive]   = palette::accent;
    c[ImGuiCol_Button]             = Col(33, 41, 59);
    c[ImGuiCol_ButtonHovered]      = Col(44, 54, 77);
    c[ImGuiCol_ButtonActive]       = Col(54, 66, 94);
    c[ImGuiCol_Header]             = palette::frame;
    c[ImGuiCol_HeaderHovered]      = palette::frameHov;
    c[ImGuiCol_HeaderActive]       = palette::frameAct;
    c[ImGuiCol_PlotHistogram]      = palette::accent;
    c[ImGuiCol_PlotHistogramHovered] = Col(120, 205, 255);
    c[ImGuiCol_TableHeaderBg]      = palette::surface;
    c[ImGuiCol_TableBorderStrong]  = palette::sep;
    c[ImGuiCol_TableBorderLight]   = palette::sep;
    c[ImGuiCol_TextLink]           = palette::accent;
    c[ImGuiCol_ModalWindowDimBg]   = Col(0, 0, 0, 120);
}

static void setupFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 20.0f * dpiScale;

    // auto tryLoad = [](const char* rel, std::string& out) -> bool {
    //     std::filesystem::path local(rel);
    //     if (std::filesystem::exists(local)) { out = local.string(); return true; }
    //     if (const char* base = SDL_GetBasePath()) {
    //         std::filesystem::path p = std::filesystem::path(base) / rel;
    //         SDL_free((void*)base);
    //         if (std::filesystem::exists(p)) { out = p.string(); return true; }
    //     }
    //     return false;
    // };

    std::string interPath, faPath;
    // if (tryLoad("InterVariable.ttf", interPath))
    //     io.Fonts->AddFontFromFileTTF(interPath.c_str(), 0.0f);
    io.Fonts->AddFontFromMemoryCompressedTTF(u8_compressed_data, u8_compressed_size, 0.0f);
    static const ImWchar faRange[] = {0xE000, 0xF8FF, 0};
    ImFontConfig cfg;
    cfg.MergeMode = true;
    // if (tryLoad("vendor/fonts/fa-solid-900.ttf", faPath))
    //     io.Fonts->AddFontFromFileTTF(faPath.c_str(), 0.0f, &cfg, faRange);
    io.Fonts->AddFontFromMemoryCompressedTTF(FaSolid_compressed_data, FaSolid_compressed_size, 0.0f, &cfg, faRange);
}

// ============================================================================
// UI drawing helpers
// ============================================================================

static ImU32 colU32(const ImVec4& c, float alphaMul = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

static void badge(const char* label, const ImVec4& color) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float padx = 9.0f, pady = 3.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ts.y + pady * 2.0f;
    float w = ts.x + padx * 2.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), colU32(color, 0.22f), h * 0.5f);
    dl->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h), colU32(color, 0.55f), h * 0.5f);
    dl->AddText(ImVec2(p.x + padx, p.y + pady), colU32(color), label);
    ImGui::Dummy(ImVec2(w + st.ItemSpacing.x * 0.0f, h));
}

static std::string ellipsize(const std::string& text, float maxW) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    std::string out = text;
    while (out.size() > 1 && ImGui::CalcTextSize((out + "...").c_str()).x > maxW)
        out.pop_back();
    return out + "...";
}

struct UiState {
    char urlBuf[4096] = {0};
    std::string errorMsg;
    bool focusUrl = true;
};
static UiState g_ui;

static std::vector<std::string> splitTokens(const std::string& text) {
    std::istringstream iss(text);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

static void submitUrls() {
    std::vector<std::string> urls = splitTokens(g_ui.urlBuf);
    if (urls.empty()) {
        g_ui.errorMsg = "Enter a URL first.";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_uriQueueMutex);
        for (const std::string& u : urls)
            g_pendingUris.push({u, g_downloadFolder});
    }
    g_ui.urlBuf[0] = '\0';
    g_ui.errorMsg.clear();
    g_ui.focusUrl = true;
}

static void drawToolbar() {
    ImGuiStyle& st = ImGui::GetStyle();
    float avail = ImGui::GetContentRegionAvail().x;

    float pasteW = ImGui::CalcTextSize(ICON_FA_PASTE).x + st.FramePadding.x * 2.0f;
    float folderLabel = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + st.FramePadding.x * 2.0f;
    float inputW = avail - pasteW - folderLabel - st.ItemSpacing.x * 2.0f;

    if (g_ui.focusUrl) { ImGui::SetKeyboardFocusHere(); g_ui.focusUrl = false; }

    ImGui::SetNextItemWidth(inputW);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll;
    if (ImGui::InputTextWithHint("##url", "Paste one or more URLs, separated by spaces...", g_ui.urlBuf, sizeof(g_ui.urlBuf), flags)) {
        submitUrls();
    }
    if (ImGui::IsItemEdited()) g_ui.errorMsg.clear();
    ImGui::SetItemTooltip("URL to download");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PASTE)) {
        if (char* clip = SDL_GetClipboardText()) {
            std::string text(clip);
            SDL_free(clip);
            std::string cur(g_ui.urlBuf);
            if (cur.empty()) snprintf(g_ui.urlBuf, sizeof(g_ui.urlBuf), "%s", text.c_str());
            else snprintf(g_ui.urlBuf + strlen(g_ui.urlBuf), sizeof(g_ui.urlBuf) - strlen(g_ui.urlBuf), " %s", text.c_str());
            g_ui.errorMsg.clear();
        }
    }
    ImGui::SetItemTooltip("Paste from clipboard");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN)) {
        const char* startAt = g_downloadFolder.size() > 1 && g_downloadFolder[0] == '/'
                                  ? g_downloadFolder.c_str()
                                  : nullptr;
        SDL_ShowOpenFolderDialog(
            [](void*, const char* const* filelist, int) {
                std::lock_guard<std::mutex> lock(g_pickMutex);
                g_folderPicked = true;
                g_pickedFolder.clear();
                if (filelist && filelist[0]) g_pickedFolder = filelist[0];
            },
            nullptr, g_window, startAt, false);
    }
    ImGui::SetItemTooltip("Choose download folder");

    // Second row: destination + action button.
    float dlBtnW = ImGui::CalcTextSize(ICON_FA_DOWNLOAD " Download").x + st.FramePadding.x * 2.0f;
    float destMaxW = ImGui::GetContentRegionAvail().x - dlBtnW - st.ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette::textDim, "Saving to:");
    ImGui::SameLine();
    std::string shown = ellipsize(g_downloadFolder, destMaxW - ImGui::CalcTextSize("Saving to: ").x);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::SetItemTooltip("%s", g_downloadFolder.c_str());

    ImGui::SameLine();
    float cursorX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(cursorX + ImGui::GetContentRegionAvail().x - dlBtnW);
    ImGui::PushStyleColor(ImGuiCol_Button, palette::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(96, 200, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(40, 158, 228));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(10, 16, 24));
    if (ImGui::Button(ICON_FA_DOWNLOAD " Download"))
        submitUrls();
    ImGui::PopStyleColor(4);

    if (!g_ui.errorMsg.empty())
        ImGui::TextColored(palette::red, "%s", g_ui.errorMsg.c_str());
}

static void drawStatsBar(const std::vector<DownloadInfo>& list) {
    ImGuiStyle& st = ImGui::GetStyle();
    aria2::GlobalStat gs;

    {
        std::lock_guard<std::mutex> lock(g_dlMutex);
        gs = g_gstat;
    }

    int done = 0, errored = 0;
    for (const auto& d : list) {
        if (d.status == aria2::DOWNLOAD_COMPLETE) ++done;
        else if (d.status == aria2::DOWNLOAD_ERROR) ++errored;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, palette::accent);
    ImGui::TextUnformatted(ICON_FA_ARROW_DOWN);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    ImGui::TextUnformatted(fmtSpeed(gs.downloadSpeed).c_str());

    ImGui::SameLine(0, st.ItemSpacing.x * 2);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim);
    ImGui::TextUnformatted(ICON_FA_ARROW_UP);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    ImGui::TextUnformatted(fmtSpeed(gs.uploadSpeed).c_str());

    ImGui::SameLine(0, st.ItemSpacing.x * 2);
    ImGui::TextColored(palette::textDim, "|");

    ImGui::SameLine(0, st.ItemSpacing.x * 2);
    ImGui::Text("%d downloading  %d queued  %d done", gs.numActive, gs.numWaiting, done);

    // Right-aligned: minimize to tray + clear finished.
    float trashW = ImGui::CalcTextSize(ICON_FA_TRASH).x + st.FramePadding.x * 2.0f;
    float trayW = ImGui::CalcTextSize(ICON_FA_WINDOW_MINIMIZE).x + st.FramePadding.x * 2.0f;
    float maxX = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine();
    float cur = ImGui::GetCursorPosX();

    bool anyFinished = done + errored > 0;
    if (!anyFinished) ImGui::BeginDisabled();
    ImGui::SetCursorPosX(cur + maxX - trashW);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::red);
    if (ImGui::Button(ICON_FA_TRASH)) {
        std::lock_guard<std::mutex> lock(g_dlMutex);
        std::vector<std::string> doomed;
        for (const auto& [gid, d] : g_downloads)
            if (d.status == aria2::DOWNLOAD_COMPLETE || d.status == aria2::DOWNLOAD_ERROR ||
                d.status == aria2::DOWNLOAD_REMOVED)
                doomed.push_back(gid);
        for (const auto& gid : doomed) eraseDownloadLocked(gid);
    }
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Clear finished");
    if (!anyFinished) ImGui::EndDisabled();

    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(cur + maxX - trashW - trayW - st.ItemSpacing.x);
    if (!g_trayActive.load()) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_FA_WINDOW_MINIMIZE)) requestHideWindow();
    ImGui::SetItemTooltip(g_trayActive.load()
                              ? "Hide to tray (downloads keep running)  \xc2\xb7  Ctrl+H"
                              : "System tray unavailable");
    if (!g_trayActive.load()) ImGui::EndDisabled();
}

static std::string statusLabel(aria2::DownloadStatus status, int errCode, ImVec4* color) {
    switch (status) {
    case aria2::DOWNLOAD_ACTIVE:  *color = palette::accent;  return "Downloading";
    case aria2::DOWNLOAD_WAITING: *color = palette::textDim; return "Queued";
    case aria2::DOWNLOAD_PAUSED:  *color = palette::orange;  return "Paused";
    case aria2::DOWNLOAD_COMPLETE:*color = palette::green;   return "Complete";
    case aria2::DOWNLOAD_ERROR: {
        *color = palette::red;
        std::string msg = errorString(errCode);
        return msg.empty() ? "Failed" : msg;
    }
    default:                      *color = palette::textDim; return "Removed";
    }
}

static void drawDownloadCard(const DownloadInfo& d) {
    ImGuiStyle& st = ImGui::GetStyle();
    float lineH = ImGui::GetTextLineHeight();
    float barH = 18.0f;

    float padY = 12.0f;
    float cardH = padY + lineH + st.ItemSpacing.y + barH + st.ItemSpacing.y + lineH + padY + 30;

    ImGui::PushID(d.gid.c_str());
    ImGui::BeginChild("card", ImVec2(0, cardH), ImGuiChildFlags_AlwaysUseWindowPadding);

    // --- Row 1: name + status badge -------------------------------------
    ImVec4 statusColor = palette::textDim;
    std::string statusTxt = statusLabel(d.status, d.errorCode, &statusColor);

    float badgeW = ImGui::CalcTextSize(statusTxt.c_str()).x + 18.0f;
    float availW = ImGui::GetContentRegionAvail().x;
    std::string name = ellipsize(d.name, availW - badgeW - st.ItemSpacing.x * 2.0f);
    ImGui::TextUnformatted(name.c_str());
    if (d.name != name) ImGui::SetItemTooltip("%s", d.name.c_str());
    else if (!d.path.empty()) ImGui::SetItemTooltip("%s", d.path.c_str());

    ImGui::SameLine();
    float curX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(curX + ImGui::GetContentRegionAvail().x - badgeW + st.ItemSpacing.x);
    badge(statusTxt.c_str(), statusColor);

    // --- Row 2: progress bar --------------------------------------------
    float frac = d.totalLength > 0 ? (float)((double)d.completedLength / (double)d.totalLength) : 0.0f;
    frac = std::clamp(frac, 0.0f, 1.0f);

    char pct[16];
    snprintf(pct, sizeof(pct), "%.1f%%", frac * 100.0f);

    ImVec4 barColor = statusColor;
    if (d.status == aria2::DOWNLOAD_WAITING || d.status == aria2::DOWNLOAD_REMOVED)
        barColor = palette::frameHov;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, barH),
                       d.totalLength > 0 ? pct : "");
    ImGui::PopStyleColor();

    // --- Row 3: details --------------------------------------------------
    std::string details;
    if (d.totalLength > 0)
        details = fmtBytes(d.completedLength) + " of " + fmtBytes(d.totalLength);
    else if (!d.url.empty())
        details = ellipsize(d.url, ImGui::GetContentRegionAvail().x * 0.5f);
    if (d.status == aria2::DOWNLOAD_ACTIVE && d.downloadSpeed > 0) {
        details += (details.empty() ? "" : "  \xc2\xb7  ");
        details += fmtSpeed(d.downloadSpeed);
        double remain = (double)(d.totalLength - d.completedLength);
        if (remain > 0) {
            details += "  \xc2\xb7  ";
            details += ICON_FA_CLOCK " ";
            details += fmtETA(remain / d.downloadSpeed) + " left";
        }
    }
    ImGui::TextColored(palette::textDim, "%s", details.c_str());
    if (!errorString(d.errorCode).empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, palette::red);
        ImGui::TextUnformatted(errorString(d.errorCode).c_str());
        ImGui::PopStyleColor();
    }

    // --- Right-aligned action buttons ------------------------------------
    enum class Action { Toggle, Remove, OpenFile, OpenFolder, Retry };
    struct Btn { const char* icon; const char* tip; Action action; };

    std::vector<Btn> buttons;
    switch (d.status) {
    case aria2::DOWNLOAD_ACTIVE:
    case aria2::DOWNLOAD_WAITING:
        buttons.push_back({ICON_FA_PAUSE, "Pause", Action::Toggle});
        buttons.push_back({ICON_FA_XMARK, "Cancel", Action::Remove});
        break;
    case aria2::DOWNLOAD_PAUSED:
        buttons.push_back({ICON_FA_PLAY, "Resume", Action::Toggle});
        buttons.push_back({ICON_FA_XMARK, "Cancel", Action::Remove});
        break;
    case aria2::DOWNLOAD_COMPLETE:
        if (!d.path.empty())
            buttons.push_back({ICON_FA_OPEN_EXTERNAL, "Open file", Action::OpenFile});
        buttons.push_back({ICON_FA_FOLDER, "Open folder", Action::OpenFolder});
        buttons.push_back({ICON_FA_TRASH, "Remove from list", Action::Remove});
        break;
    case aria2::DOWNLOAD_ERROR:
        if (!d.url.empty())
            buttons.push_back({ICON_FA_DOWNLOAD, "Retry", Action::Retry});
        buttons.push_back({ICON_FA_TRASH, "Remove from list", Action::Remove});
        break;
    default:
        break;
    }

    float totalW = 0.0f;
    for (size_t i = 0; i < buttons.size(); ++i) {
        totalW += ImGui::CalcTextSize(buttons[i].icon).x + st.FramePadding.x * 2.0f;
        if (i + 1 < buttons.size()) totalW += st.ItemSpacing.x;
    }
    float startX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - totalW;

    for (const Btn& b : buttons) {
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(startX);
        startX += ImGui::CalcTextSize(b.icon).x + st.FramePadding.x * 2.0f + st.ItemSpacing.x;
        bool clicked = ImGui::Button(b.icon);
        ImGui::SetItemTooltip("%s", b.tip);
        if (!clicked) continue;
        switch (b.action) {
        case Action::Toggle: {
            CmdType t = (d.status == aria2::DOWNLOAD_PAUSED) ? CmdType::Resume : CmdType::Pause;
            enqueueCommand(t, d.gid);
            break;
        }
        case Action::Remove:
            enqueueCommand(CmdType::Remove, d.gid);
            if (d.status != aria2::DOWNLOAD_ACTIVE && d.status != aria2::DOWNLOAD_WAITING &&
                d.status != aria2::DOWNLOAD_PAUSED) {
                std::lock_guard<std::mutex> lock(g_dlMutex);
                eraseDownloadLocked(d.gid);
            }
            break;
        case Action::OpenFile:
            openPathOrUrl(d.path);
            break;
        case Action::OpenFolder:
            openPathOrUrl(d.path.empty() ? g_downloadFolder : directoryOf(d.path));
            break;
        case Action::Retry:
            {
                std::lock_guard<std::mutex> lock(g_uriQueueMutex);
                g_pendingUris.push({d.url, g_downloadFolder});
            }
            {
                std::lock_guard<std::mutex> lock(g_dlMutex);
                eraseDownloadLocked(d.gid);
            }
            break;
        }
    }

    ImGui::EndChild();
    ImGui::PopID();
}

static void drawEmptyState() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGuiStyle& st = ImGui::GetStyle();

    float iconScale = 2.6f;
    float iconSize = ImGui::CalcTextSize(ICON_FA_DOWNLOAD).y * iconScale;
    float lineH = ImGui::GetTextLineHeight();
    float blockH = iconSize + st.ItemSpacing.y + lineH + 4.0f + lineH;

    ImVec2 p = ImGui::GetCursorScreenPos();
    float centerY = p.y + avail.y * 0.5f;
    float startY = centerY - blockH * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    float iconW = ImGui::CalcTextSize(ICON_FA_DOWNLOAD).x * iconScale;
    dl->AddText(NULL, ImGui::GetStyle().FontSizeBase * iconScale,
                ImVec2(p.x + (avail.x - iconW) * 0.5f, startY),
                colU32(palette::accent, 0.30f), ICON_FA_DOWNLOAD);

    const char* msg = "No downloads yet";
    ImVec2 msz = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (avail.x - msz.x) * 0.5f, startY + iconSize + st.ItemSpacing.y),
                colU32(palette::textDim), msg);

    const char* hint = "Paste a URL above to get started.";
    ImVec2 hsz = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(p.x + (avail.x - hsz.x) * 0.5f, startY + iconSize + st.ItemSpacing.y + lineH + 4.0f),
                colU32(palette::textDim, 0.55f), hint);

    ImGui::Dummy(avail);
}

// ============================================================================
// Window visibility
//
// Hiding never destroys the window or the GL context: the aria2 worker thread
// is independent of the UI, so downloads keep running while hidden and showing
// the window again is instant.
// ============================================================================

static void showWindow() {
    if (!g_window) return;
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    g_windowVisible.store(true);
}

static void hideWindow() {
    if (!g_window) return;
    SDL_HideWindow(g_window);
    g_windowVisible.store(false);
}

// Apply a show/hide request posted by the tray thread. Runs on the SDL thread.
static void applyVisibilityRequest() {
    int req = g_visibilityRequest.exchange(0);
    if (req > 0 && !g_windowVisible.load()) showWindow();
    else if (req < 0 && g_windowVisible.load()) hideWindow();
}

// Closing the window hides it to the tray instead of quitting. Without a tray
// there would be no way back, so in that case close really means quit.
static void handleCloseRequest() {
    if (g_trayActive.load()) {
//        requestToggleWindow();
        hideWindow();
        static bool informed = false;
        if (!informed) {
            informed = true;
            desktopNotify("agui is still running",
                          "Downloads continue in the background. Use the tray icon to reopen.");
        }
    } else {
        requestQuit();
    }
}

static void shutdownImGuiBackends() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    SDL_GL_DestroyContext(g_gl_context);
    SDL_DestroyWindow(g_window);
    g_gl_context = nullptr;
    g_window = nullptr;
}

// Drain SDL events without touching the GPU. Used while hidden so the process
// stays responsive (tray requests, dialog callbacks) at near-zero cost.
static void pumpEventsWhileHidden() {
    SDL_Event event;
    // Block briefly instead of spinning; wakes early on any incoming event.
    SDL_WaitEventTimeout(nullptr, 100);
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) requestQuit();
    }
    updateTray();
}

void main_loop(SDL_Window *window, ImGuiIO& io) {
        applyVisibilityRequest();
        updateTray();

        if (!g_windowVisible.load()) {
            pumpEventsWhileHidden();
            return;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
//            if (event.type == SDL_EVENT_QUIT)
//                requestQuit();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                handleCloseRequest();
            if (event.type == SDL_EVENT_DROP_TEXT) {
                const char* text = event.drop.data;
                if (text && *text) {
                    std::string cur(g_ui.urlBuf);
                    if (cur.empty())
                        snprintf(g_ui.urlBuf, sizeof(g_ui.urlBuf), "%s", text);
                    else
                        snprintf(g_ui.urlBuf + strlen(g_ui.urlBuf),
                                 sizeof(g_ui.urlBuf) - strlen(g_ui.urlBuf), " %s", text);
                    g_ui.errorMsg.clear();
                    g_ui.focusUrl = true;
                }
            }
        }

        // Collect async folder-picker results.
        {
            std::lock_guard<std::mutex> lock(g_pickMutex);
            if (g_folderPicked) {
                g_folderPicked = false;
                if (!g_pickedFolder.empty() && g_pickedFolder != g_downloadFolder) {
                    g_downloadFolder = g_pickedFolder;
                    saveConfig();
                }
            }
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            return;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Shortcuts: Ctrl+H hides to tray, Ctrl+Q quits for real.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) requestQuit();
        if (g_trayActive.load() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_H))
            requestHideWindow();

        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("main", NULL,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings);

            drawToolbar();
            ImGui::Separator();

            // Snapshot of shared state for this frame.
            std::vector<DownloadInfo> list;
            {
                std::lock_guard<std::mutex> lock(g_dlMutex);
                list.reserve(g_order.size());
                for (const std::string& gid : g_order) {
                    auto it = g_downloads.find(gid);
                    if (it != g_downloads.end()) list.push_back(it->second);
                }
            }

            drawStatsBar(list);
            ImGui::Separator();

            float footerH = 0.0f;
            ImGui::BeginChild("downloads", ImVec2(0, -footerH),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            if (list.empty()) {
                drawEmptyState();
            } else {
                for (const DownloadInfo& d : list)
                    drawDownloadCard(d);
            }
            ImGui::EndChild();

            ImGui::End();
        }

        ImGui::Render();
        ImVec4 clear_color = palette::bg;
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
}



// ============================================================================
// System tray (SDL3)
// ============================================================================

enum class TrayAction { Toggle, PauseAll, ResumeAll, OpenFolder, Quit };

static SDL_Tray* g_tray = nullptr;
static SDL_TrayEntry* g_trayToggleItem = nullptr;
static SDL_TrayEntry* g_trayStatusItem = nullptr;
static SDL_TrayEntry* g_trayPauseItem = nullptr;
static SDL_TrayEntry* g_trayResumeItem = nullptr;
static SDL_Surface* g_trayIconIdle = nullptr;
static SDL_Surface* g_trayIconActive = nullptr;
static std::chrono::steady_clock::time_point g_trayLastRefresh{};
static bool g_trayTransferring = false;

static SDL_Surface* makeTrayIcon(bool active) {
    const int size = 64;
    SDL_Surface* surf = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;

    const uint8_t bgR = active ? 66 : 90;
    const uint8_t bgG = active ? 183 : 130;
    const uint8_t bgB = active ? 255 : 170;
    const int radius = size / 2 - 4;

    SDL_LockSurface(surf);
    uint8_t* pixels = static_cast<uint8_t*>(surf->pixels);
    const int pitch = surf->pitch;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint8_t* px = pixels + y * pitch + x * 4;
            const int cx = x - size / 2;
            const int cy = y - size / 2;
            if (cx * cx + cy * cy <= radius * radius) {
                px[0] = bgR;
                px[1] = bgG;
                px[2] = bgB;
                px[3] = 255;
            } else {
                px[0] = px[1] = px[2] = px[3] = 0;
            }
        }
    }

    // Simple downward arrow in the center.
    for (int y = 18; y < 46; ++y) {
        for (int x = 22; x < 42; ++x) {
            const int stem = (x >= 29 && x <= 34) ? 1 : 0;
            const int tip = (y >= 36 && x >= 26 && x <= 37 &&
                             std::abs(x - 31) <= (y - 36) / 2)
                                ? 1
                                : 0;
            if (!stem && !tip) continue;
            uint8_t* px = pixels + y * pitch + x * 4;
            px[0] = px[1] = px[2] = 245;
            px[3] = 255;
        }
    }
    SDL_UnlockSurface(surf);
    return surf;
}

static void SDLCALL onTrayEntry(void* userdata, SDL_TrayEntry* /*entry*/) {
    switch (static_cast<TrayAction>(reinterpret_cast<uintptr_t>(userdata))) {
    case TrayAction::Toggle:     requestToggleWindow(); break;
    case TrayAction::PauseAll:   enqueueCommand(CmdType::PauseAll, std::string()); break;
    case TrayAction::ResumeAll:  enqueueCommand(CmdType::ResumeAll, std::string()); break;
    case TrayAction::OpenFolder: openPathOrUrl(g_downloadFolder); break;
    case TrayAction::Quit:       requestQuit(); break;
    }
}

static void updateTray() {
    if (!g_tray) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_trayLastRefresh != std::chrono::steady_clock::time_point{} &&
        now - g_trayLastRefresh < std::chrono::milliseconds(500))
        return;
    g_trayLastRefresh = now;

    int active = 0, paused = 0, waiting = 0, done = 0, failed = 0;
    int64_t speed = 0;
    {
        std::lock_guard<std::mutex> lock(g_dlMutex);
        speed = g_gstat.downloadSpeed;
        for (const auto& [gid, d] : g_downloads) {
            switch (d.status) {
            case aria2::DOWNLOAD_ACTIVE:   ++active; break;
            case aria2::DOWNLOAD_PAUSED:   ++paused; break;
            case aria2::DOWNLOAD_WAITING:  ++waiting; break;
            case aria2::DOWNLOAD_COMPLETE: ++done; break;
            case aria2::DOWNLOAD_ERROR:    ++failed; break;
            default: break;
            }
        }
    }

    if (g_trayToggleItem)
        SDL_SetTrayEntryLabel(g_trayToggleItem,
                              g_windowVisible.load() ? "Hide window" : "Show window");

    if (g_trayStatusItem) {
        std::string status;
        if (active > 0) {
            status = std::to_string(active) + " downloading \xc2\xb7 " + fmtSpeed(speed);
            if (waiting > 0) status += " \xc2\xb7 " + std::to_string(waiting) + " queued";
        } else if (paused > 0) {
            status = std::to_string(paused) + " paused";
        } else if (waiting > 0) {
            status = std::to_string(waiting) + " queued";
        } else if (failed > 0) {
            status = std::to_string(failed) + " failed";
        } else if (done > 0) {
            status = std::to_string(done) + " completed";
        } else {
            status = "Idle";
        }
        SDL_SetTrayEntryLabel(g_trayStatusItem, status.c_str());
    }

    if (g_trayPauseItem)
        SDL_SetTrayEntryEnabled(g_trayPauseItem, active + waiting > 0);
    if (g_trayResumeItem)
        SDL_SetTrayEntryEnabled(g_trayResumeItem, paused > 0);

    const bool transferring = active > 0;
    if (transferring != g_trayTransferring) {
        g_trayTransferring = transferring;
        SDL_SetTrayIcon(g_tray, transferring ? g_trayIconActive : g_trayIconIdle);
    }

    const std::string tip = transferring ? ("agui \xe2\x80\x94 " + fmtSpeed(speed)) : "agui";
    SDL_SetTrayTooltip(g_tray, tip.c_str());
}

static void destroySystemTray() {
    if (!g_tray) return;
    SDL_DestroyTray(g_tray);
    g_tray = nullptr;
    g_trayToggleItem = nullptr;
    g_trayStatusItem = nullptr;
    g_trayPauseItem = nullptr;
    g_trayResumeItem = nullptr;
    if (g_trayIconIdle) {
        SDL_DestroySurface(g_trayIconIdle);
        g_trayIconIdle = nullptr;
    }
    if (g_trayIconActive) {
        SDL_DestroySurface(g_trayIconActive);
        g_trayIconActive = nullptr;
    }
}

static bool startSystemTray() {
    g_trayIconIdle = makeTrayIcon(false);
    g_trayIconActive = makeTrayIcon(true);

    g_tray = SDL_CreateTray(g_trayIconIdle, "agui");
    if (!g_tray) {
        fprintf(stderr, "agui: could not create system tray: %s\n", SDL_GetError());
        destroySystemTray();
        return false;
    }

    SDL_TrayMenu* menu = SDL_CreateTrayMenu(g_tray);
    if (!menu) {
        fprintf(stderr, "agui: could not create tray menu: %s\n", SDL_GetError());
        destroySystemTray();
        return false;
    }

    g_trayStatusItem = SDL_InsertTrayEntryAt(
        menu, -1, "Idle", SDL_TRAYENTRY_BUTTON | SDL_TRAYENTRY_DISABLED);
    SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);

    g_trayToggleItem = SDL_InsertTrayEntryAt(menu, -1, "Hide window", SDL_TRAYENTRY_BUTTON);
    SDL_SetTrayEntryCallback(g_trayToggleItem, onTrayEntry,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(TrayAction::Toggle)));

    g_trayPauseItem = SDL_InsertTrayEntryAt(menu, -1, "Pause all", SDL_TRAYENTRY_BUTTON);
    SDL_SetTrayEntryCallback(g_trayPauseItem, onTrayEntry,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(TrayAction::PauseAll)));

    g_trayResumeItem = SDL_InsertTrayEntryAt(menu, -1, "Resume all", SDL_TRAYENTRY_BUTTON);
    SDL_SetTrayEntryCallback(g_trayResumeItem, onTrayEntry,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(TrayAction::ResumeAll)));

    SDL_TrayEntry* folderItem =
        SDL_InsertTrayEntryAt(menu, -1, "Open download folder", SDL_TRAYENTRY_BUTTON);
    SDL_SetTrayEntryCallback(folderItem, onTrayEntry,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(TrayAction::OpenFolder)));

    SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);

    SDL_TrayEntry* quitItem = SDL_InsertTrayEntryAt(menu, -1, "Quit", SDL_TRAYENTRY_BUTTON);
    SDL_SetTrayEntryCallback(quitItem, onTrayEntry,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(TrayAction::Quit)));

    SDL_SetTrayEntryEnabled(g_trayPauseItem, false);
    SDL_SetTrayEntryEnabled(g_trayResumeItem, false);
    g_trayLastRefresh = {};
    updateTray();
    return true;
}

// ============================================================================
// Headless platform self-test (agui --self-test). Exercises the platform
// layer without a display; CI runs it on Linux, macOS and Windows.
// ============================================================================

static bool checkSelfTest(const char* what, const std::string& got, const std::string& want) {
    bool ok = (got == want);
    printf("[self-test] %-14s got='%s' want='%s' %s\n",
           what, got.c_str(), want.c_str(), ok ? "OK" : "FAIL");
    return ok;
}

static bool runSelfTest() {
    bool ok = true;
    printf("[self-test] imgui=%s sdl=%d\n", ImGui::GetVersion(), SDL_GetVersion());
    printf("[self-test] config-path=%s\n", configPath().string().c_str());
    ok &= !configPath().empty();

#ifdef _WIN32
    ok &= checkSelfTest("basename", baseNameOf("C:\\Users\\x\\file.txt"), "file.txt");
    ok &= checkSelfTest("dirname", directoryOf("C:\\Users\\x\\file.txt"), "C:\\Users\\x");
    ok &= checkSelfTest("sep-forward", baseNameOf("C:/Users/x/file.txt"), "file.txt");
    printf("[self-test] open=ShellExecuteW\n");
    printf("[self-test] notify=tray-balloon\n");
    int rc = runDetached({"cmd.exe", "/c", "exit", "0"});
#else
#ifdef __APPLE__
    printf("[self-test] open=/usr/bin/open\n");
    printf("[self-test] notify=/usr/bin/osascript\n");
#else
    printf("[self-test] open=xdg-open\n");
    printf("[self-test] notify=notify-send\n");
#endif
    ok &= checkSelfTest("basename", baseNameOf("/home/x/file.txt"), "file.txt");
    ok &= checkSelfTest("dirname", directoryOf("/home/x/file.txt"), "/home/x");
    int rc = runDetached({"true"});
#endif
    printf("[self-test] spawn-exit=%d %s\n", rc, rc == 0 ? "OK" : "FAIL");
    ok &= (rc == 0);
    int rcBad = runDetached({"agui-missing-helper-xyz"});
    printf("[self-test] spawn-missing=%d %s\n", rcBad, rcBad != 0 ? "OK" : "FAIL");
    ok &= (rcBad != 0);
    printf("[self-test] %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    bool startHidden = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--hidden") == 0 || strcmp(argv[i], "--tray") == 0) {
            startHidden = true;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            return runSelfTest() ? 0 : 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: agui [--hidden] [--self-test]\n"
                   "  --hidden, --tray   start minimized to the system tray\n"
                   "  --self-test        run headless platform checks and exit\n");
            return 0;
        }
    }

    aria2::libraryInit();

    aria2::SessionConfig config;
    config.downloadEventCallback = downloadEventCallback;
    config.keepRunning = true;
    g_session = aria2::sessionNew(aria2::KeyVals(), config);

    loadConfig();
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
    g_main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("agui", (int)(1280 * g_main_scale), (int)(800 * g_main_scale), window_flags);
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
    SDL_SetWindowMinimumSize(window, 640, 420);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);

    g_window = window;
    g_gl_context = gl_context;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    setupTheme();

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    setupFonts(g_main_scale);

    g_trayActive.store(startSystemTray());

    // Starting hidden only makes sense when there is a tray to restore from.
    if (startHidden && g_trayActive.load()) {
        g_windowVisible.store(false);
    } else {
        if (startHidden)
            fprintf(stderr, "agui: no tray available, starting with the window shown\n");
        showWindow();
    }

    while (!g_done.load()) {
        main_loop(g_window, io);
    }

    destroySystemTray();

    shutdownImGuiBackends();
    ImGui::DestroyContext();

    SDL_Quit();

    g_keepRunning = false;
    if (aria2Thread.joinable()) aria2Thread.join();

    return 0;
}

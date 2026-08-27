#define NOB_IMPLEMENTATION
#include "nob.h"

#include <string.h>
#include <ctype.h>

#define BUILD_FOLDER "build/"
#define IMGUI_DIR    "vendor/imgui/"

static bool force_rebuild = false;
static bool release_build = false;

// Keep debug and release object files separate so switching modes never
// links stale sanitizer-instrumented objects into a clean build.
static const char *obj_dir(void)
{
    return release_build ? BUILD_FOLDER "release/" : BUILD_FOLDER "debug/";
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-B") == 0) {
            force_rebuild = true;
        } else if (strcmp(argv[i], "release") == 0) {
            release_build = true;
        } else {
            nob_log(NOB_WARNING, "unknown argument: %s (supported: -B, release)", argv[i]);
        }
    }
}

static bool append_pkgconfig_flags(Nob_Cmd *cmd, const char *flag)
{
    Nob_String_Builder sb = {0};
    char query[512];
    snprintf(query, sizeof(query), "pkg-config %s appindicator3-0.1 gtk+-3.0", flag);

    FILE *pipe = popen(query, "r");
    if (!pipe) {
        nob_log(NOB_ERROR, "could not run pkg-config");
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
        nob_sb_append_buf(&sb, buf, n);
    int ok = pclose(pipe) == 0;
    if (!ok || !sb.items) {
        nob_sb_free(sb);
        nob_log(NOB_ERROR, "pkg-config could not find appindicator3-0.1/gtk+-3.0");
        return false;
    }

    // pkg-config output is whitespace separated; split into args
    size_t start = 0;
    for (size_t i = 0; i <= sb.count; ++i) {
        if (i == sb.count || isspace((unsigned char)sb.items[i])) {
            if (i > start) {
                size_t len = i - start;
                char *arg = malloc(len + 1);
                if (!arg) { nob_sb_free(sb); pclose(pipe); return false; }
                memcpy(arg, &sb.items[start], len);
                arg[len] = '\0';
                // NOTE: deliberately leaked; the process is short-lived
                nob_da_append_many(cmd, &arg, 1);
            }
            start = i + 1;
        }
    }
    nob_sb_free(sb);
    return true;
}

static void append_cxx_base(Nob_Cmd *cmd)
{
    nob_cmd_append(cmd, "clang++", "-std=c++17", "-Wall", "-Wextra");
}

static bool append_cxx_compile_flags(Nob_Cmd *cmd)
{
    append_cxx_base(cmd);
    if (release_build) {
        nob_cmd_append(cmd, "-O2", "-DNDEBUG");
    } else {
        nob_cmd_append(cmd,
            "-g",
            "-O1",
            "-fno-omit-frame-pointer",
            "-fsanitize=address,undefined");
    }
    return append_pkgconfig_flags(cmd, "--cflags");
}

static bool append_cxx_link_flags(Nob_Cmd *cmd)
{
    append_cxx_base(cmd);
    if (!release_build) {
        nob_cmd_append(cmd, "-fsanitize=address,undefined");
    }
    if (!append_pkgconfig_flags(cmd, "--libs")) return false;
    nob_cmd_append(cmd, "-laria2", "-lSDL3", "-lGL", "-ldl");
    return true;
}

static bool compile_cpp_to_o(const char *src, const char *name)
{
    Nob_String_Builder obj_path = {0};
    nob_sb_append_cstr(&obj_path, obj_dir());
    nob_sb_append_cstr(&obj_path, name);
    nob_da_append(&obj_path, 0);

    bool fresh = !force_rebuild && nob_needs_rebuild1(obj_path.items, src) <= 0;
    if (fresh) {
        nob_sb_free(obj_path);
        return true;
    }

    Nob_Cmd cmd = {0};
    if (!append_cxx_compile_flags(&cmd)) return false;
    nob_cmd_append(&cmd,
        "-I", IMGUI_DIR,
        "-I", IMGUI_DIR "backends",
        "-c", src,
        "-o", obj_path.items);
    bool ok = nob_cmd_run(&cmd);
    nob_sb_free(obj_path);
    return ok;
}

static bool link_app(void)
{
    const char *names[] = {
        "agui.o",
        "imgui.o",
        "imgui_draw.o",
        "imgui_tables.o",
        "imgui_widgets.o",
        "imgui_impl_sdl3.o",
        "imgui_impl_opengl3.o",
        "imgui_demo.o",
    };

    Nob_String_Builder paths[NOB_ARRAY_LEN(names)] = {0};
    const char *objs[NOB_ARRAY_LEN(names)] = {0};
    for (size_t i = 0; i < NOB_ARRAY_LEN(names); ++i) {
        nob_sb_append_cstr(&paths[i], obj_dir());
        nob_sb_append_cstr(&paths[i], names[i]);
        nob_da_append(&paths[i], 0);
        objs[i] = paths[i].items;
    }

    Nob_String_Builder out_sb = {0};
    nob_sb_append_cstr(&out_sb, BUILD_FOLDER "agui");
    nob_da_append(&out_sb, 0);
    const char *out = out_sb.items;

    bool needs = force_rebuild || nob_needs_rebuild(out, objs, NOB_ARRAY_LEN(objs)) > 0;
    bool ok = true;
    if (needs) {
        Nob_Cmd cmd = {0};
        if (!append_cxx_link_flags(&cmd)) {
            ok = false;
        } else {
            nob_cmd_append(&cmd, "-o", out);
            for (size_t i = 0; i < NOB_ARRAY_LEN(objs); ++i) {
                nob_cmd_append(&cmd, objs[i]);
            }
            ok = nob_cmd_run(&cmd);
        }
    }

    for (size_t i = 0; i < NOB_ARRAY_LEN(paths); ++i) nob_sb_free(paths[i]);
    nob_sb_free(out_sb);
    return ok;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    parse_args(argc, argv);

    nob_log(NOB_INFO, "build mode: %s", release_build ? "release" : "debug (asan+ubsan)");
    if (force_rebuild) nob_log(NOB_INFO, "force rebuild enabled (-B)");

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;
    if (!nob_mkdir_if_not_exists(obj_dir())) return 1;

    if (!compile_cpp_to_o("agui.cc",                                             "agui.o"))                return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui.cpp",                                 "imgui.o"))               return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_draw.cpp",                            "imgui_draw.o"))          return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_tables.cpp",                          "imgui_tables.o"))        return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_widgets.cpp",                         "imgui_widgets.o"))       return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_demo.cpp",                            "imgui_demo.o"))          return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "backends/imgui_impl_sdl3.cpp",              "imgui_impl_sdl3.o"))     return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "backends/imgui_impl_opengl3.cpp",           "imgui_impl_opengl3.o"))  return 1;

    if (!link_app()) return 1;
    nob_log(NOB_INFO, "built %s (%s)", BUILD_FOLDER "agui", release_build ? "release" : "debug");
    return 0;
}

#define NOB_IMPLEMENTATION
#include "nob.h"

#include <string.h>

#define BUILD_FOLDER "build/"
#define IMGUI_DIR    "vendor/imgui/"

static bool force_rebuild = false;
static bool release_build = false;

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

static void append_cxx_base(Nob_Cmd *cmd)
{
    nob_cmd_append(cmd, "clang++", "-std=c++17", "-Wall", "-Wextra");
}

static void append_cxx_compile_flags(Nob_Cmd *cmd)
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
}

static void append_cxx_link_flags(Nob_Cmd *cmd)
{
    append_cxx_base(cmd);
    if (!release_build) {
        nob_cmd_append(cmd, "-fsanitize=address,undefined");
    }
}

static bool compile_cpp_to_o(const char *src, const char *obj)
{
    if (!force_rebuild && nob_needs_rebuild1(obj, src) <= 0) return true;

    Nob_Cmd cmd = {0};
    append_cxx_compile_flags(&cmd);
    nob_cmd_append(&cmd,
        "-I", IMGUI_DIR,
        "-I", IMGUI_DIR "backends",
        "-c", src,
        "-o", obj);
    return nob_cmd_run(&cmd);
}

static bool link_app(void)
{
    const char *objs[] = {
        BUILD_FOLDER "agui.o",
        BUILD_FOLDER "imgui.o",
        BUILD_FOLDER "imgui_draw.o",
        BUILD_FOLDER "imgui_tables.o",
        BUILD_FOLDER "imgui_widgets.o",
        BUILD_FOLDER "imgui_impl_sdl3.o",
        BUILD_FOLDER "imgui_impl_opengl3.o",
        BUILD_FOLDER "imgui_demo.o",
    };

    const char *out = BUILD_FOLDER "agui";
    if (!force_rebuild && nob_needs_rebuild(out, objs, NOB_ARRAY_LEN(objs)) <= 0) return true;

    Nob_Cmd cmd = {0};
    append_cxx_link_flags(&cmd);
    nob_cmd_append(&cmd, "-o", out);
    for (size_t i = 0; i < NOB_ARRAY_LEN(objs); ++i) {
        nob_cmd_append(&cmd, objs[i]);
    }
    nob_cmd_append(&cmd, "-laria2", "-lSDL3", "-lGL", "-ldl");
    return nob_cmd_run(&cmd);
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    parse_args(argc, argv);

    nob_log(NOB_INFO, "build mode: %s", release_build ? "release" : "debug (asan+ubsan)");
    if (force_rebuild) nob_log(NOB_INFO, "force rebuild enabled (-B)");

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;

    if (!compile_cpp_to_o("agui.cc",                              BUILD_FOLDER "agui.o"))            return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui.cpp",                        BUILD_FOLDER "imgui.o"))                  return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_draw.cpp",                   BUILD_FOLDER "imgui_draw.o"))             return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_tables.cpp",                 BUILD_FOLDER "imgui_tables.o"))           return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_widgets.cpp",                BUILD_FOLDER "imgui_widgets.o"))          return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "imgui_demo.cpp",                   BUILD_FOLDER "imgui_demo.o"))             return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "backends/imgui_impl_sdl3.cpp",     BUILD_FOLDER "imgui_impl_sdl3.o"))         return 1;
    if (!compile_cpp_to_o(IMGUI_DIR "backends/imgui_impl_opengl3.cpp",  BUILD_FOLDER "imgui_impl_opengl3.o"))      return 1;

    if (!link_app()) return 1;
    return 0;
}

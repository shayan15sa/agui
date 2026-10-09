# agui: A GUI for aria2
[![build](https://github.com/shayan15sa/agui/actions/workflows/build.yml/badge.svg)](https://github.com/shayan15sa/agui/actions/workflows/build.yml)
I couldn't find anything simple enough for this need so I decided to make my own.
It uses libaria2 and ImGui (SDL3 + OpenGL3).

## Features

- Add downloads by pasting one or more URLs (space separated), drag & drop text
  onto the window, or paste via the clipboard button.
- Live download list with progress bars, speeds, sizes, and ETA.
- Per-download actions: pause / resume / cancel / retry / open file / open folder.
- Completed and failed downloads stay in the list with status badges and error
  descriptions; clear them all with the trash button.
- Global stats bar: aggregate download/upload speed and item counts.
- Desktop notifications on completion/failure (native backend per OS).
- Download folder picker uses native SDL dialogs; the choice is persisted in
  the per-platform config location (`SDL_GetPrefPath`, migrated from
  `~/.config/agui/config` when present).

## Building

```
git clone --recursive https://github.com/shayan15sa/agui.git

# Linux:
./nob            # debug build with ASan+UBSan into build/agui
./nob release    # optimized build
./nob -B         # force full rebuild

# macOS / Windows (and Linux too):
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

`./nob` is a Linux-only shortcut; other platforms use the CMake path above
(Windows via MSYS2 UCRT64, macOS via Homebrew). `./build/agui --self-test`
runs headless platform checks (config path, open/notify backends, process
launch).

Requires: clang++, pkg-config, libaria2, SDL3 (>=3.2 for native dialogs and
system tray), OpenGL. The Inter UI font and the FontAwesome icon font are loaded
from the repo (`vendor/fonts/`).

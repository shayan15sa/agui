# agui: A GUI for aria2
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
- Desktop notifications on completion/failure (notify-send).
- Download folder picker uses native SDL dialogs; the choice is persisted in
  `~/.config/agui/config`.

## Building

```
./nob            # debug build with ASan+UBSan into build/agui
./nob release    # optimized build
./nob -B         # force full rebuild

cmake -B build && cmake --build build   # equivalent CMake path
```

Requires: clang++, pkg-config, libaria2, SDL3 (>=3.2 for native dialogs),
GTK3 + libappindicator (system tray), OpenGL. The Inter UI font and the
FontAwesome icon font are loaded from the repo (`vendor/fonts/`).

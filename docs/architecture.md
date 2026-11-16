# Architecture

This layout separates project-owned code, upstream dependencies, resources and
generated artifacts. It does not change playback behavior or the existing Qt
class names.

## Application modules

- **`src/app/main.cpp`** initializes SDL and Qt, enforces a single instance and
  handles the `--autostart` entry path.
- **`src/gui/`** contains the main window, its signal/slot connections, system
  tray menu and Qt Designer form. It calls `WallpaperAPI` instead of managing
  decoding directly.
- **`src/wallpaper/wallpaper_api.hpp`** provides the Qt-facing wallpaper library
  and playback worker. Qt's MOC processes this header because it declares
  `QObject` classes.
- **`third_party/lwallpaper_core`** is a git submodule pinned to a core commit.
  Its `livewallpaper::Wallpaper` public API owns decoding, native windows,
  rendering, visibility pause and timing. `transcodeToScreenResolution` handles
  conversion. The Qt worker only calls the library and forwards its status/errors;
  it does not include the private FFmpeg/Direct3D engine headers.
- Core implementation and optimization tests are maintained in the core
  repository. GUI collection publication/cancellation tests live here.

Project includes are rooted at `src/`, for example
`#include "wallpaper/wallpaper_api.hpp"`. Includes within one module may remain
relative. Do not add application code to `third_party/`.

## Resources and builds

`resources/lwallpaperGUI.qrc` keeps the existing `:/icons/lwallpaperGUI.ico`
resource URL using an alias, so moving the icon does not break the tray icon.
The Windows resource file uses the same icon file.

CMake is the only build system. It runs Qt MOC/UIC/RCC, links the native
dependencies, and deploys runtime DLLs and Qt plugins after linking. Dependency
paths are configurable in `cmake/Dependencies.cmake`.

Visual Studio opens the repository as a CMake folder or uses the solution
generated inside `build/`. There is no second set of handwritten project files
to keep in sync. All generated files, DLLs and executables stay under `build/`;
the default Release executable is `build/bin/Release/lwallpaper.exe`.

Legacy project directories and previous build caches have been removed.
Application data is separate from project sources; no migration of the current
wallpaper library is needed.

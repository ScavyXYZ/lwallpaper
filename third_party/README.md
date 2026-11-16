# Third-party dependencies

Application code belongs in `src/`; keep external packages here without modifying
their sources. Each package retains its upstream license files.

| Directory | Package | Expected contents |
| --- | --- | --- |
| `ffmpeg/` | FFmpeg 9.0.2, Gyan full shared development build (avcodec 63) | `include/`, `lib/*.lib`, `bin/*.dll` |
| `SDL3/` | SDL 3.4.20 official Visual C++ development package | `include/SDL3/`, `lib/x64/SDL3.lib`, `lib/x64/SDL3.dll` |
| `rang/` | rang console colors | `rang.hpp` (license in header) |

Qt is installed separately, not vendored. Use its MSVC x64 package (currently
tested with Qt 6.11.1); pass `-QtRoot` to `scripts/build.ps1` or set `Qt6_ROOT`
when configuring CMake.

Download SDL's Visual C++ development archive from
<https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20> if `lib/x64/` is
missing. Extract the **x64** libraries and DLL into `SDL3/lib/x64/`; do not use
the x86 or ARM64 versions.

FFmpeg binaries/import libraries and SDL binaries/import libraries are managed
with Git LFS. After cloning, run `git lfs install` and `git lfs pull`.
The CMake cache variables `FFMPEG_ROOT` and `SDL3_ROOT` allow external packages
instead of these default directories.

## Pinned downloads and verification

The installed packages are the latest stable releases found on 2026-11-11,
not development snapshots. [versions.json](versions.json) records the exact
upstream download URLs, SHA-256 checksums and installed archive subtrees. Both
downloaded archives were verified against their publishers' checksums.

Use the **shared development** FFmpeg package from
<https://www.gyan.dev/ffmpeg/builds/> / Gyan's linked GitHub mirror, not a static
CLI-only archive. Always replace the headers, import libraries and DLLs together
and configure a clean build after an ABI-major upgrade. The current DLLs are:

```text
avcodec-63.dll  avdevice-63.dll  avfilter-12.dll  avformat-63.dll
avutil-61.dll   swresample-7.dll swscale-10.dll   SDL3.dll
```

Only SDL's x64 libraries are vendored; unrelated architectures and FFmpeg's
MinGW import libraries, CLI tools and generated documentation are omitted.
FFmpeg CLI is installed separately when running integration tests.

**Distribution:** Gyan's full shared FFmpeg build is GPLv3, including libx264;
it is not an LGPL-only build. Retain the upstream licenses and comply with
their redistribution/source obligations. This update does not change the
license of the project's own source files.

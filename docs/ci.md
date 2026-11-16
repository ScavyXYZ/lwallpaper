# Continuous integration

GitHub Actions builds the GUI against its pinned `lwallpaper_core` submodule
on Windows Server 2025 in Release and Debug. The installed Visual Studio
2022/2026 toolchain is discovered using `vswhere`. Qt 6.8.3
MSVC 2022 is installed from the official Qt archives by a SHA-pinned action.
This is a reproducible CI baseline; local builds may use newer compatible Qt.

Triggers: push, pull request and manual dispatch. Source checkout includes
submodules and verified Git LFS dependencies. The core's shared CI script
downloads the exact Gyan FFmpeg CLI archive pinned in its dependency manifest,
verifies SHA-256, and adds it to PATH before CMake registers media tests.

CTest runs unit, media/decoder, and atomic Qt import/shutdown tests. The VGUI
edition also runs its offscreen UI/HiDPI tests. Hardware GPU smoke tests are
excluded on hosted runners; local GPU coverage remains available. No automatic
test attaches to the desktop wallpaper.

Successful jobs upload the application with matching Qt/FFmpeg/SDL runtime
files, dependency versions and licenses. Test executables are excluded.
Artifacts expire after 14 days; they are not GitHub Releases. Debug builds are
testing artifacts, not redistributable release packages. Review all dependency
licenses, including Qt and FFmpeg, before distributing release packages.
JUnit results, CMake/CTest diagnostics and available UI screenshots are uploaded
even after failure.

The workflows use `contents: read`, no custom secrets, commit-pinned actions,
and ordinary `pull_request` rather than privileged `pull_request_target`.

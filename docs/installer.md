# Windows installer

Build Release first, then run `./scripts/build-installer.ps1`. Inno Setup 6
must be installed. CI builds the installer only for Release.

Optional arguments: `-Version 1.3`, `-BuildDirectory <staged-release-folder>`,
`-OutputDirectory <folder>`, `-RedistPath <Microsoft-signed-vc-redist.exe>`.
Without RedistPath, the script downloads the official Microsoft x64 runtime
and verifies its Authenticode signature before bundling it.

Installers go to `build/installer/`, alongside SHA-256 files. Setup installs
for the current user under LocalAppData/Programs. It creates a Start Menu
shortcut and an uninstaller. Desktop shortcut and current-user autostart are
optional; no scheduled task is silently added. Wallpaper settings and media
are not deleted on uninstall.

The missing Visual C++ x64 prerequisite is installed with a separate UAC prompt;
failure to install it aborts setup. Setup itself is not code-signed: Windows
SmartScreen may warn until a signing certificate is configured.

GUI and Rose use different installation directories, shortcuts, autostart
names and AppIds. They currently share the application's single-instance mutex,
so do not run both editions simultaneously. Rose remains local unless a remote
is explicitly configured.

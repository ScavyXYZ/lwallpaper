<div align="center">

<img src="./assets/lwallpaper_logo.png" alt="LWallpaper Logo" width="180">

# LWallpaper

### A lightweight Windows live wallpaper manager written in C++

Turn your favorite videos into beautiful animated desktop wallpapers.

<br>

[![Platform](https://img.shields.io/badge/platform-Windows-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://www.microsoft.com/windows/)
[![Language](https://img.shields.io/badge/language-C%2B%2B-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![Qt](https://img.shields.io/badge/Qt-Widgets-41CD52?style=for-the-badge&logo=qt&logoColor=white)](https://www.qt.io/)
[![SDL3](https://img.shields.io/badge/SDL3-Rendering-000000?style=for-the-badge)](https://www.libsdl.org/)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-Media-007808?style=for-the-badge&logo=ffmpeg&logoColor=white)](https://ffmpeg.org/)

</div>

---

## ✨ Features

- 🎬 **Video wallpapers** — use your own videos as animated wallpapers
- 🖥️ **Automatic screen adaptation** — videos are transcoded to the current screen resolution
- ⚡ **Hardware decoding** — uses D3D11VA when supported by the video codec/GPU
- 🔄 **Seamless looping** — wallpapers automatically restart when the video reaches the end
- 🗂️ **Wallpaper library** — keep multiple wallpapers and switch between them
- ➕ **Easy importing** — add videos directly from the GUI
- ❌ **Wallpaper management** — remove wallpapers you no longer need
- 🖱️ **System tray support** — close the window without actually stopping the application
- 🚀 **Autostart support** — restore the last selected wallpaper on application startup
- 🔒 **Single-instance protection** — launching the application twice brings the existing instance to the foreground
- 🧵 **Multithreaded playback** — video decoding and wallpaper playback run independently from the UI
- 🎯 **Frame timing** — frames are rendered according to their presentation timestamps for smoother playback

---

## 🛠️ Tech Stack

LWallpaper combines several technologies to provide smooth video playback directly on the Windows desktop.

| Technology | Purpose |
|---|---|
| **C++** | Core application logic |
| **Qt Widgets** | Graphical user interface |
| **SDL3** | Rendering video frames |
| **FFmpeg** | Video decoding and transcoding |
| **Win32 API** | Windows desktop integration |
| **D3D11VA** | Optional hardware-accelerated video decoding |

The application uses FFmpeg's format, codec and scaling APIs for decoding and pixel-format conversion.

SDL3 is used to create the rendering window and display YUV video frames.

---

## 🎥 Supported Video Formats

The GUI currently allows importing:

- `.mp4`
- `.mkv`
- `.webm`
- `.avi`

The selected video is processed before being added to the wallpaper library. :contentReference[oaicite:4]{index=4}

During processing, the video is converted to the current physical screen resolution and encoded using H.264 with YUV420P output.
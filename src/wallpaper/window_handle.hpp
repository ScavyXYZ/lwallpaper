#pragma once
#include <windows.h>

namespace wallpaper {
    // Wallpaper windows must be destroyed on the thread that created them,
    // including when decoder/renderer initialization throws.
    class WindowHandle {
    public:
        explicit WindowHandle(HWND handle) : handle_(handle) {}
        ~WindowHandle() { if (handle_) DestroyWindow(handle_); }
        WindowHandle(const WindowHandle&) = delete;
        WindowHandle& operator=(const WindowHandle&) = delete;
        HWND get() const { return handle_; }
    private:
        HWND handle_;
    };
}

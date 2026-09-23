#include "lwallpaperGUI.h"
#include <QtWidgets/QApplication>
#include <QStringList>
#include <SDL3/SDL.h>
#include <windows.h>

static const wchar_t* kSingleInstanceMutexName = L"Local\\LWallpaperGUI-SingleInstance-8F3E2B1A";

static const wchar_t* kShowExistingInstanceMessage = L"LWallpaperGUI-ShowExistingInstance-8F3E2B1A";

int main(int argc, char* argv[])
{
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    if (alreadyRunning) {
        UINT showMsg = RegisterWindowMessageW(kShowExistingInstanceMessage);
        PostMessageW(HWND_BROADCAST, showMsg, 0, 0);

        if (singleInstanceMutex) CloseHandle(singleInstanceMutex);
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (singleInstanceMutex) { ReleaseMutex(singleInstanceMutex); CloseHandle(singleInstanceMutex); }
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    QApplication app(argc, argv);
    lwallpaperGUI window;
    window.installShowRequestFilter();

    QStringList args = QApplication::arguments();
    bool isAutostart = args.contains("--autostart");

    if (isAutostart) {
        window.startLastWallpaperQuietly();
    }
    else {
        window.show();
    }

    int result = app.exec();

    SDL_Quit();

    if (singleInstanceMutex) {
        ReleaseMutex(singleInstanceMutex);
        CloseHandle(singleInstanceMutex);
    }

    return result;
}

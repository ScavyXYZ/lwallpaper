#include "lwallpaperGUI.h"
#include <QtWidgets/QApplication>
#include <QStringList>
#include <SDL3/SDL.h> 

int main(int argc, char* argv[])
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    QApplication app(argc, argv);
    lwallpaperGUI window;

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

    return result;
}
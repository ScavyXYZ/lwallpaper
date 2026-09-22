#pragma once

#include <QtWidgets/QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QCloseEvent>
#include "ui_lwallpaperGUI.h"
#include "wallpaper_api.hpp"

class lwallpaperGUI : public QMainWindow
{
    Q_OBJECT

public:
    lwallpaperGUI(QWidget* parent = nullptr);
    ~lwallpaperGUI();

    void startLastWallpaperQuietly();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_pushButtonStart_clicked();
    void on_pushButtonStop_clicked();
    void on_pushButtonAdd_clicked();
    void on_pushButtonDelete_clicked();
    void updateDeleteButtonState();

private:
    Ui::lwallpaperGUIClass ui;
    WallpaperAPI* m_api;

    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;
};
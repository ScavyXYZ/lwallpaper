#include "wallpaper/wallpaper_api.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>

namespace {
    void check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool import(const QString& input, const QString& output, bool cancelled = false) {
        TranscodeWorker worker(input, output);
        bool notified = false, success = false;
        QObject::connect(&worker, &TranscodeWorker::finished, [&](bool ok, const QString& error) {
            notified = true;
            success = ok;
            if (!ok) std::cerr << "Import result: " << error.toStdString() << '\n';
        });
        if (cancelled) worker.cancel();
        worker.doWork();
        check(notified, "Import must emit completion");
        return success;
    }

    QByteArray contents(const QString& path) {
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "Cannot read worker output");
        return file.readAll();
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    app.setApplicationName("ImportShutdown-" + QString::number(QCoreApplication::applicationPid()));
    const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    struct Cleanup { QString path; ~Cleanup() { QDir(path).removeRecursively(); } } cleanup{config};
    av_log_set_level(AV_LOG_ERROR);
    try {
        check(argc == 3, "Expected fixture and scratch directory");
        QTemporaryDir directory(QString::fromUtf8(argv[2]) + "/worker-XXXXXX");
        check(directory.isValid(), "Cannot create worker test directory");
        const QString input = directory.filePath(QString::fromUtf8("відео тест.mp4"));
        check(QFile::copy(QString::fromUtf8(argv[1]), input), "Cannot create Unicode input fixture");
        const QString output = directory.filePath("ready.mp4");
        check(import(input, output), "Atomic import failed");
        check(contents(input) == contents(output), "Fast import must preserve original bytes");

        QFile existing(directory.filePath("existing.mp4"));
        check(existing.open(QIODevice::WriteOnly), "Cannot create existing-file fixture");
        existing.write("preserve this file");
        existing.close();
        check(!import(input, existing.fileName()), "Import must not overwrite an existing file");
        check(contents(existing.fileName()) == "preserve this file", "Failed import destroyed an existing file");

        const QString cancelled = directory.filePath("cancelled.mp4");
        check(!import(input, cancelled, true), "Cancelled import should fail");
        check(!QFile::exists(cancelled), "Cancelled import left a published file");
        check(QDir(directory.path()).entryList({".import-*"}, QDir::Files | QDir::Hidden).isEmpty(),
            "Temporary files must be cleaned after success/failure/cancellation");
        // No GUI event loop: shutdown must not depend on a queued completion.
        for (int i = 0; i < 10; ++i) {
            WallpaperAPI api;
            api.add(input, QString("shutdown-%1.mp4").arg(i));
            if (i % 2) QThread::msleep(20);
        }
        check(QDir(config + "/LWallpaper/wallpapers").entryList(
            {".import-*"}, QDir::Files | QDir::Hidden).isEmpty(),
            "Shutdown must join import threads and clean temporary files");
        std::cout << "PASS: atomic imports, Unicode paths, existing-file preservation, cancellation cleanup\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QFileInfo>
#include <atomic>
#include <livewallpaper/livewallpaper.hpp>

// Qt owns collection management and worker lifetimes. Playback and video
// conversion are implemented only by the pinned livewallpaper core library.
class WallpaperWorker : public QObject {
    Q_OBJECT
public:
    WallpaperWorker(const QString& path, int, int) : m_path(path) {}
public slots:
    void startPlayback() {
        emit statusChanged("Initializing environment...");
        livewallpaper::Wallpaper player([](livewallpaper::LogLevel, std::string_view) {});
        const std::string error = player.play(m_path.toUtf8().toStdString());
        if (!error.empty()) {
            emit errorOccurred(QString::fromStdString(error));
        } else {
            emit statusChanged("Wallpaper started.");
            while (!m_cancelled && player.isPlaying()) QThread::msleep(20);
            player.stop();
            if (!m_cancelled && !player.lastError().empty())
                emit errorOccurred(QString::fromStdString(player.lastError()));
            emit statusChanged("Wallpaper stopped.");
        }
        emit finished();
    }
    void stopPlayback() { m_cancelled = true; }
signals:
    void finished();
    void errorOccurred(const QString& message);
    void statusChanged(const QString& status);
private:
    QString m_path;
    std::atomic<bool> m_cancelled{false};
};

class TranscodeWorker : public QObject {
    Q_OBJECT
public:
    TranscodeWorker(const QString& input, const QString& output) : m_input(input), m_output(output) {}
public slots:
    void doWork() {
        QTemporaryFile temporary(QFileInfo(m_output).absolutePath() + "/.import-XXXXXX.mp4");
        if (!temporary.open()) { emit finished(false, "Cannot create temporary import file"); return; }
        const QString path = temporary.fileName();
        temporary.close();
        const auto error = livewallpaper::transcodeToScreenResolution(
            m_input.toUtf8().toStdString(), path.toUtf8().toStdString(), m_cancelled);
        if (m_cancelled) {
            emit finished(false, "Processing cancelled by the user.");
        } else if (!error.empty()) {
            emit finished(false, QString::fromStdString(error));
        } else if (!temporary.rename(m_output)) {
            emit finished(false, "Cannot commit imported video (destination may already exist)");
        } else {
            temporary.setAutoRemove(false);
            emit finished(true, "");
        }
    }
    void cancel() { m_cancelled = true; }
signals:
    void finished(bool success, const QString& errorMsg);
private:
    QString m_input, m_output;
    std::atomic<bool> m_cancelled{false};
};

class WallpaperAPI : public QObject {
    Q_OBJECT
public:
    explicit WallpaperAPI(QObject* parent = nullptr) : QObject(parent) { loadIndex(); }
    ~WallpaperAPI() {
        cancelTranscode();
        stop();
        for (auto* thread : findChildren<QThread*>(QString(), Qt::FindDirectChildrenOnly)) {
            thread->quit();
            thread->wait();
        }
    }
    bool isPlaying() const { return m_thread && m_thread->isRunning(); }
    QStringList getAvailableWallpapers() const { return m_wallpapers.values(); }

    bool run(const QString& filename) {
        stop();
        const QString path = getAppDataPath() + "/wallpapers/" + filename;
        if (!QFile::exists(path)) { emit errorOccurred("File not found: " + filename); return false; }
        m_thread = new QThread(this);
        m_worker = new WallpaperWorker(path, 0, 0);
        m_worker->moveToThread(m_thread);
        connect(m_thread, &QThread::started, m_worker, &WallpaperWorker::startPlayback);
        connect(m_worker, &WallpaperWorker::statusChanged, this, &WallpaperAPI::statusChanged);
        connect(m_worker, &WallpaperWorker::errorOccurred, this, &WallpaperAPI::errorOccurred);
        connect(m_worker, &WallpaperWorker::finished, m_thread, &QThread::quit, Qt::DirectConnection);
        connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
        connect(m_thread, &QThread::destroyed, this, &WallpaperAPI::playbackStopped);
        m_activeFilename = filename;
        m_thread->start();
        return true;
    }
    void stop() {
        if (m_worker) m_worker->stopPlayback();
        if (m_thread) { m_thread->quit(); m_thread->wait(); }
        m_worker = nullptr;
        m_thread = nullptr;
        m_activeFilename.clear();
    }
    bool isActive(const QString& filename) const { return isPlaying() && m_activeFilename == filename; }

    void add(const QString& inputPath, const QString& targetFilename) {
        if (!QFile::exists(inputPath)) {
            emit errorOccurred("File does not exist.");
            emit transcodeFinished(false, targetFilename, "File does not exist.");
            return;
        }
        QDir dir(getAppDataPath() + "/wallpapers");
        if (!dir.exists()) dir.mkpath(".");
        if (m_wallpapers.contains(targetFilename)) {
            emit statusChanged("Video already exists in the list.");
            emit transcodeFinished(true, targetFilename, "");
            return;
        }
        auto* thread = new QThread(this);
        auto* worker = new TranscodeWorker(inputPath, dir.absoluteFilePath(targetFilename));
        worker->moveToThread(thread);
        connect(this, &WallpaperAPI::cancelTranscodeSignal, worker, &TranscodeWorker::cancel, Qt::DirectConnection);
        connect(thread, &QThread::started, worker, &TranscodeWorker::doWork);
        connect(worker, &TranscodeWorker::finished, this,
            [this, targetFilename](bool success, const QString& error) {
                if (success) {
                    m_wallpapers.insert(targetFilename);
                    saveIndex();
                    emit wallpaperAdded(targetFilename);
                }
                emit transcodeFinished(success, targetFilename, error);
            });
        connect(worker, &TranscodeWorker::finished, thread, &QThread::quit, Qt::DirectConnection);
        connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    bool remove(const QString& filename) {
        if (!m_wallpapers.contains(filename)) return false;
        if (isActive(filename)) {
            emit errorOccurred("Stop the wallpaper before deleting the video that's currently playing.");
            return false;
        }
        const QString path = getAppDataPath() + "/wallpapers/" + filename;
        if (QFile::exists(path) && !QFile::remove(path)) {
            emit errorOccurred("Failed to delete the video file from disk.");
            return false;
        }
        m_wallpapers.remove(filename);
        saveIndex();
        emit statusChanged("Wallpaper deleted successfully.");
        return true;
    }
    void cancelTranscode() { emit cancelTranscodeSignal(); }
signals:
    void errorOccurred(const QString& message);
    void statusChanged(const QString& status);
    void wallpaperAdded(const QString& filename);
    void transcodeFinished(bool success, const QString& filename, const QString& errorMsg);
    void cancelTranscodeSignal();
    void playbackStopped();
private:
    void loadIndex() {
        QFile file(getAppDataPath() + "/vid_index.txt");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty()) m_wallpapers.insert(line);
        }
    }
    void saveIndex() {
        QFile file(getAppDataPath() + "/vid_index.txt");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&file);
        for (const auto& filename : m_wallpapers) out << filename << "\n";
    }
    QString getAppDataPath() const {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/LWallpaper";
        QDir().mkpath(path);
        return path;
    }
    QSet<QString> m_wallpapers;
    QPointer<QThread> m_thread;
    QPointer<WallpaperWorker> m_worker;
    QString m_activeFilename;
};

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <atomic>
#include <string>
#include <QPointer>
#include <QStandardPaths>

#include "wallpaper_core.hpp"
#include "d3d11_zero_copy.hpp"

class WallpaperWorker : public QObject {
	Q_OBJECT
public:
	WallpaperWorker(const QString& videoPath, int w, int h)
		: m_videoPath(videoPath), m_w(w), m_h(h), m_cancelled(false) {
	}

public slots:
	void startPlayback() {
		try {
			emit statusChanged("Initializing environment...");
			timeBeginPeriod(1);

			HWND parent = wallpaper::wallpaper_hwnd(m_w, m_h);
			while (!parent && !m_cancelled.load()) {
				QThread::msleep(15);
				parent = wallpaper::wallpaper_hwnd(m_w, m_h);
			}
			if (!parent) {
				// Cancelled (e.g. Stop pressed) while still waiting for the desktop's
				// WorkerW window - nothing was created yet, so just exit cleanly.
				timeEndPeriod(1);
				emit finished();
				return;
			}

			HINSTANCE inst = GetModuleHandle(nullptr);
			HWND child_hwnd = wallpaper::create_wallpaper_child(inst, parent, m_w, m_h);

			FrameQueue queue(kFrameQueueCapacity);

			// Try the zero-copy D3D11 render path first: it must be created (and its
			// device known) before the decoder starts, since the decoder needs that
			// device to share hwaccel with. If this fails for any reason - old GPU,
			// driver quirk, shader compile issue - zero_copy is left invalid and we
			// fall straight back to the existing SDL renderer below, unchanged.
			wallpaper::D3D11ZeroCopyRenderer zero_copy;
			bool zero_copy_ready = zero_copy.initialize(child_hwnd, m_w, m_h);
			if (!zero_copy_ready) {
				LOG_WARN("Zero-copy D3D11 renderer unavailable (" << zero_copy.last_error()
					<< "), falling back to SDL render path");
			}

			Decoder decoder(m_videoPath.toStdString(), queue, m_cancelled);
			if (zero_copy_ready) {
				decoder.external_d3d_device = zero_copy.device();
			}
			decoder.start();

			emit statusChanged("Wallpaper started.");

			if (zero_copy_ready && decoder.is_zero_copy_active()) {
				LOG_INFO("Using zero-copy D3D11 render path");
				zero_copy.set_sync_callbacks(decoder.hw_lock, decoder.hw_unlock, decoder.hw_lock_ctx);
				run_zero_copy_loop(zero_copy, queue, decoder.done_flag());
			}
			else {
				// Either zero-copy setup failed, or the decoder itself fell back to
				// software/non-shared-device decode (e.g. no D3D11VA support at all) -
				// either way frames in the queue are ordinary CPU-side YUV420P/NV12
				// data, which only the SDL renderer's upload path understands.
				Renderer renderer(child_hwnd, m_w, m_h);
				renderer.run(queue, m_cancelled, decoder.done_flag(), &decoder.nv12_passthrough_ok);
			}

			m_cancelled = true;
			queue.cancel();
			decoder.join();

			DestroyWindow(child_hwnd);
			timeEndPeriod(1);
			emit statusChanged("Wallpaper stopped.");
		}
		catch (const std::exception& ex) {
			timeEndPeriod(1);
			emit errorOccurred(QString::fromStdString(ex.what()));
		}
		emit finished();
	}

	void stopPlayback() {
		m_cancelled = true;
	}

signals:
	void finished();
	void errorOccurred(const QString& message);
	void statusChanged(const QString& status);

private:
	// Same frame-pacing logic as Renderer::run() (wait until each frame's pts is
	// due, presented via yield-based fine wait rather than a CPU-spinning busy
	// loop), but presenting through the zero-copy D3D11 path instead of an SDL
	// texture upload. If present_frame reports the renderer is no longer valid
	// (e.g. device lost), this returns early; the caller treats that the same
	// as normal queue exhaustion; the caller only reaches this method as a
	// one-shot alternative to Renderer::run() and does not currently retry
	// this playback session on the SDL path if it happens mid-stream, since a
	// device-lost event significant enough to abort zero-copy is rare and the
	// next Start click will simply re-negotiate the render path from scratch.
	void run_zero_copy_loop(wallpaper::D3D11ZeroCopyRenderer& zero_copy,
		FrameQueue& queue, std::atomic<bool>& decoder_done) {
		double perf_freq = static_cast<double>(SDL_GetPerformanceFrequency());
		double pts_origin = -1.0;
		double wall_origin = 0.0;
		double last_pts = 0.0;
		bool   first_frame = true;
		int    frame_count = 0;

		while (!m_cancelled) {
			wallpaper::pump_win32_events(m_cancelled);
			if (m_cancelled) break;

			YUVFrame frame;
			if (!queue.pop(frame, m_cancelled, decoder_done)) break;

			if (!first_frame && frame.pts_seconds < last_pts - 0.5) {
				pts_origin = frame.pts_seconds;
				wall_origin = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq;
			}
			last_pts = frame.pts_seconds;

			if (first_frame) {
				pts_origin = frame.pts_seconds;
				wall_origin = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq;
				first_frame = false;
			}

			double elapsed = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq - wall_origin;
			double frame_time = frame.pts_seconds - pts_origin;
			double wait_sec = frame_time - elapsed;

			if (wait_sec > 0.002) {
				SDL_Delay(static_cast<Uint32>((wait_sec - 0.002) * 1000.0));
			}
			while ((static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq - wall_origin) < frame_time) {
				std::this_thread::yield();
			}

			if (!frame.is_raw_hw_frame) {
				// Shouldn't happen (the decoder only takes this render path when it
				// confirmed zero-copy is active), but a mismatched frame here would
				// otherwise be silently misinterpreted as a D3D11 surface. Skip it.
				LOG_WARN("run_zero_copy_loop: got a non-raw-hw frame, skipping");
				continue;
			}

			if (!zero_copy.present_frame(frame.frame.get())) {
				if (!zero_copy.is_valid()) {
					LOG_ERROR("Zero-copy renderer lost (" << zero_copy.last_error() << "), stopping playback");
					break;
				}
				// A single frame failing to present (e.g. a transient SRV creation
				// error) isn't fatal - just skip it and keep going. Still log the
				// first few so a persistent failure (as opposed to one bad frame)
				// is visible instead of silently dropping every frame forever.
				if (present_fail_count_ < 5) {
					LOG_WARN("present_frame failed (" << zero_copy.last_error() << ")");
				}
				present_fail_count_++;
			}
			frame_count++;
			if (frame_count % 100 == 0) {
				LOG_INFO("Zero-copy: presented " << frame_count << " frames, " << present_fail_count_ << " failures so far");
			}
		}
		LOG_INFO("Zero-copy render loop finished, total frames presented: " << frame_count);
	}

	QString m_videoPath;
	int m_w, m_h;
	std::atomic<bool> m_cancelled;
	int present_fail_count_ = 0;
};

class TranscodeWorker : public QObject {
	Q_OBJECT
public:
	TranscodeWorker(const QString& input, const QString& output)
		: m_input(input), m_output(output), m_cancelled(false) {
	}

public slots:
	void doWork() {
		try {
			wallpaper::transcode_video_to_screen(m_input.toStdString(), m_output.toStdString(), m_cancelled);

			if (m_cancelled.load()) {
				QFile::remove(m_output);
				emit finished(false, "Processing cancelled by the user.");
			}
			else {
				emit finished(true, "");
			}
		}
		catch (const std::exception& ex) {
			QFile::remove(m_output);
			emit finished(false, QString::fromStdString(ex.what()));
		}
	}

	void cancel() {
		m_cancelled.store(true);
	}

signals:
	void finished(bool success, const QString& errorMsg);

private:
	QString m_input;
	QString m_output;
	std::atomic<bool> m_cancelled;
};

class WallpaperAPI : public QObject {
	Q_OBJECT
public:
	explicit WallpaperAPI(QObject* parent = nullptr)
		: QObject(parent), m_thread(nullptr), m_worker(nullptr) {
		loadIndex();
	}

	~WallpaperAPI() { stop(); }

	QStringList getAvailableWallpapers() const { return m_wallpapers.values(); }

	bool run(const QString& filename) {
		stop();

		QString videoPath = getAppDataPath() + "/wallpapers/" + filename;
		if (!QFile::exists(videoPath)) {
			emit errorOccurred("File not found: " + filename);
			return false;
		}

		auto [sw, sh] = wallpaper::physical_screen_size();

		m_thread = new QThread(this);
		m_worker = new WallpaperWorker(videoPath, sw, sh);
		m_worker->moveToThread(m_thread);

		connect(m_thread, &QThread::started, m_worker, &WallpaperWorker::startPlayback);
		connect(m_worker, &WallpaperWorker::statusChanged, this, &WallpaperAPI::statusChanged);
		connect(m_worker, &WallpaperWorker::errorOccurred, this, &WallpaperAPI::errorOccurred);

		connect(m_worker, &WallpaperWorker::finished, m_thread, &QThread::quit);
		connect(m_worker, &WallpaperWorker::finished, m_worker, &WallpaperWorker::deleteLater);
		connect(m_thread, &QThread::finished, m_thread, &QThread::deleteLater);
		connect(m_thread, &QThread::destroyed, this, &WallpaperAPI::playbackStopped);
		m_thread->start();
		m_activeFilename = filename;
		return true;
	}

	void stop() {
		if (m_worker) {
			m_worker->stopPlayback();
		}
		if (m_thread) {
			m_thread->quit();
			m_thread->wait();
		}
		m_worker = nullptr;
		m_thread = nullptr;
		m_activeFilename.clear();
	}

	// True while `filename` is the one currently being decoded/rendered. Used to
	// stop the user from deleting a video file out from under an active decoder.
	bool isActive(const QString& filename) const {
		return m_thread != nullptr && m_activeFilename == filename;
	}

	void add(const QString& inputPath, const QString& targetFilename) {
		if (!QFile::exists(inputPath)) {
			emit errorOccurred("File does not exist.");
			emit transcodeFinished(false, targetFilename, "File does not exist.");
			return;
		}

		QDir dir(getAppDataPath() + "/wallpapers");
		if (!dir.exists()) dir.mkpath(".");
		QString outputPath = dir.absoluteFilePath(targetFilename);

		if (m_wallpapers.contains(targetFilename)) {
			emit statusChanged("Video already exists in the list.");
			emit transcodeFinished(true, targetFilename, "");
			return;
		}

		QThread* tThread = new QThread(this);
		TranscodeWorker* tWorker = new TranscodeWorker(inputPath, outputPath);
		tWorker->moveToThread(tThread);

		connect(this, &WallpaperAPI::cancelTranscodeSignal, tWorker, &TranscodeWorker::cancel, Qt::DirectConnection);
		connect(tThread, &QThread::started, tWorker, &TranscodeWorker::doWork);

		connect(tWorker, &TranscodeWorker::finished, this, [this, tThread, targetFilename](bool success, const QString& errorMsg) {
			if (success) {
				m_wallpapers.insert(targetFilename);
				saveIndex();
				emit wallpaperAdded(targetFilename);
				emit transcodeFinished(true, targetFilename, "");
			}
			else {
				emit transcodeFinished(false, targetFilename, errorMsg);
			}
			tThread->quit();
			});

		connect(tThread, &QThread::finished, tWorker, &TranscodeWorker::deleteLater);
		connect(tThread, &QThread::finished, tThread, &QThread::deleteLater);

		tThread->start();
	}
	bool remove(const QString& filename) {
		if (!m_wallpapers.contains(filename)) {
			return false;
		}

		if (isActive(filename)) {
			emit errorOccurred("Stop the wallpaper before deleting the video that's currently playing.");
			return false;
		}

		QString videoPath = getAppDataPath() + "/wallpapers/" + filename;
		if (QFile::exists(videoPath)) {
			if (!QFile::remove(videoPath)) {
				emit errorOccurred("Failed to delete the video file from disk.");
				return false;
			}
		}

		m_wallpapers.remove(filename);

		saveIndex();

		emit statusChanged("Wallpaper deleted successfully.");
		return true;
	}

	void cancelTranscode() {
		emit cancelTranscodeSignal();
	}

signals:
	void errorOccurred(const QString& message);
	void statusChanged(const QString& status);
	void wallpaperAdded(const QString& filename);
	void transcodeFinished(bool success, const QString& filename, const QString& errorMsg);
	void cancelTranscodeSignal();
	void playbackStopped();

private:
	void loadIndex() {
		m_wallpapers.clear();
		QFile file(getAppDataPath() + "/vid_index.txt");
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
		QTextStream in(&file);
		while (!in.atEnd()) {
			QString line = in.readLine().trimmed();
			if (!line.isEmpty()) m_wallpapers.insert(line);
		}
		file.close();
	}

	void saveIndex() {
		QFile file(getAppDataPath() + "/vid_index.txt");
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
		QTextStream out(&file);
		for (const QString& wp : m_wallpapers) out << wp << "\n";
		file.close();
	}
	QString getAppDataPath() const {
		QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
		QString appDir = baseDir + "/LWallpaper";

		QDir dir(appDir);
		if (!dir.exists()) {
			dir.mkpath(".");
		}
		return appDir;
	}
	QSet<QString> m_wallpapers;
	QPointer<QThread> m_thread;
	QPointer<WallpaperWorker> m_worker;
	QString m_activeFilename;
};
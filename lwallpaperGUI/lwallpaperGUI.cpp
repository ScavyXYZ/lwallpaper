#include "lwallpaperGUI.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProgressDialog>
#include <QStandardPaths>

lwallpaperGUI::lwallpaperGUI(QWidget* parent)
	: QMainWindow(parent)
{
	ui.setupUi(this);
	m_api = new WallpaperAPI(this);
	ui.listWidget->addItems(m_api->getAvailableWallpapers());

	connect(m_api, &WallpaperAPI::statusChanged, this, [this](const QString& status) {
		ui.statusBar->showMessage(status);
		});

	connect(m_api, &WallpaperAPI::errorOccurred, this, [this](const QString& err) {
		QMessageBox::critical(this, "Error", err);
		ui.pushButtonStart->setEnabled(true);
		ui.pushButtonStop->setEnabled(false);
		ui.pushButtonAdd->setEnabled(true);
		ui.pushButtonDelete->setEnabled(true);
		});

	connect(m_api, &WallpaperAPI::wallpaperAdded, this, [this](const QString& newWp) {
		ui.listWidget->addItem(newWp);
		});

	connect(m_api, &WallpaperAPI::playbackStopped, this, [this]() {
		ui.pushButtonStart->setEnabled(true);
		ui.pushButtonAdd->setEnabled(true);
		ui.pushButtonDelete->setEnabled(true);
		ui.statusBar->showMessage("Playback fully stopped.");
		});
	m_trayMenu = new QMenu(this);

	QAction* showAction = m_trayMenu->addAction("Open Settings");
	QAction* stopWallpaperAction = m_trayMenu->addAction("Stop Wallpaper");
	m_trayMenu->addSeparator();
	QAction* quitAction = m_trayMenu->addAction("Exit");

	connect(showAction, &QAction::triggered, this, &lwallpaperGUI::showNormal);
	connect(stopWallpaperAction, &QAction::triggered, this, &lwallpaperGUI::on_pushButtonStop_clicked);
	connect(quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);

	m_trayIcon = new QSystemTrayIcon(this);
	m_trayIcon->setContextMenu(m_trayMenu);

	QIcon appIcon(":/icons/lwallpaperGUI.ico");

	m_trayIcon->setIcon(appIcon);
	this->setWindowIcon(appIcon);
	m_trayIcon->show();

	connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::DoubleClick) {
			this->showNormal();
			this->activateWindow();
		}
		});
	ui.pushButtonStop->setEnabled(false);
}

lwallpaperGUI::~lwallpaperGUI()
{
}

void lwallpaperGUI::on_pushButtonStart_clicked()
{
	QListWidgetItem* current = ui.listWidget->currentItem();
	if (!current) {
		ui.statusBar->showMessage("Please select a video from the list first!");
		return;
	}

	QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/LWallpaper";
	QDir().mkpath(appDataPath);

	ui.pushButtonStart->setEnabled(false);
	ui.pushButtonDelete->setEnabled(false);
	ui.pushButtonAdd->setEnabled(false);

	QString filename = current->text();
	if (m_api->run(filename)) {
		QFile file(appDataPath + "/last_wallpaper.txt");
		if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
			QTextStream out(&file);
			out << filename;
			file.close();
		}

		ui.pushButtonStop->setEnabled(true);
	}
	else {
		ui.pushButtonStart->setEnabled(true);
		ui.pushButtonDelete->setEnabled(true);
		ui.pushButtonAdd->setEnabled(true);
	}
}

void lwallpaperGUI::on_pushButtonStop_clicked()
{
	ui.pushButtonStop->setEnabled(false);

	QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/LWallpaper";
	QFile::remove(appDataPath + "/last_wallpaper.txt");
	ui.statusBar->showMessage("Stopping wallpaper...");
	m_api->stop();
}

void lwallpaperGUI::on_pushButtonAdd_clicked()
{
	QString filePath = QFileDialog::getOpenFileName(this, "Select a Wallpaper Video", "", "Video Files (*.mp4 *.mkv *.webm *.avi)");
	if (filePath.isEmpty()) return;

	QFileInfo info(filePath);
	QString fileName = info.fileName();

	ui.pushButtonStart->setEnabled(false);
	ui.pushButtonStop->setEnabled(false);
	ui.pushButtonAdd->setEnabled(false);
	ui.pushButtonDelete->setEnabled(false);

	QProgressDialog* progress = new QProgressDialog("Transcoding video to fit the screen...", "Cancel", 0, 0, this);
	progress->setWindowModality(Qt::WindowModal);
	progress->setWindowTitle("Please Wait");
	progress->setMinimum(0);
	progress->setMaximum(0);

	connect(progress, &QProgressDialog::canceled, this, [this]() {
		m_api->cancelTranscode();
		ui.statusBar->showMessage("Conversion cancelled.");
		});

	QMetaObject::Connection* conn = new QMetaObject::Connection();
	*conn = connect(m_api, &WallpaperAPI::transcodeFinished, this, [this, progress, conn](bool success, const QString& name, const QString& err) {
		progress->close();
		progress->deleteLater();

		if (success) {
			ui.statusBar->showMessage("Video added successfully: " + name);
		}
		else if (!err.isEmpty() && err != "Processing cancelled by the user.") {
			QMessageBox::critical(this, "Conversion Error", err);
		}

		ui.pushButtonStart->setEnabled(true);
		ui.pushButtonStop->setEnabled(true);
		ui.pushButtonAdd->setEnabled(true);
		ui.pushButtonDelete->setEnabled(true);

		QObject::disconnect(*conn);
		delete conn;
		});

	m_api->add(filePath, fileName);
	progress->show();
}

void lwallpaperGUI::startLastWallpaperQuietly()
{
	QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/LWallpaper";
	QFile file(appDataPath + "/last_wallpaper.txt");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QTextStream in(&file);
		QString lastVideo = in.readLine().trimmed();
		file.close();

		if (!lastVideo.isEmpty()) {
			m_api->run(lastVideo);
		}
	}
}

void lwallpaperGUI::on_pushButtonDelete_clicked()
{
	QListWidgetItem* current = ui.listWidget->currentItem();
	if (!current) {
		ui.statusBar->showMessage("Please select a video to delete first!");
		return;
	}

	QString filename = current->text();

	QMessageBox::StandardButton reply;
	reply = QMessageBox::question(this,
		"Confirm Deletion",
		QString("Are you sure you want to permanently delete the wallpaper \"%1\"?").arg(filename),
		QMessageBox::Yes | QMessageBox::No
	);

	if (reply == QMessageBox::Yes) {
		if (m_api->remove(filename)) {
			int row = ui.listWidget->row(current);
			delete ui.listWidget->takeItem(row);
		}
	}
}

void lwallpaperGUI::closeEvent(QCloseEvent* event)
{
	if (m_trayIcon && m_trayIcon->isVisible()) {
		this->hide();
		event->ignore();

		m_trayIcon->showMessage(
			"Lwallpaper",
			"The application has been minimized to the tray and is still running.",
			QSystemTrayIcon::Information,
			2000
		);
	}
	else {
		event->accept();
	}
}

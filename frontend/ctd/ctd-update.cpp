/******************************************************************************
    CTD OBS: update check (docs/ctd/updates.md).

    CTD OBS asks GitHub which CTD OBS releases exist: one anonymous GET of
    https://api.github.com/repos/ItsLimitlezz/ctd-obs/releases with a
    "User-Agent: CTD-OBS/<version>" header, no token, nothing about the user
    or the PC. Automatically ~10 s after start, at most once per 24 h (Settings
    ▸ CTD ▸ Updates turns it off), or with CTD ▸ Check for Updates….

      Stable channel   published releases ctd-v<X.Y.Z>
      Test builds      also prereleases ctd-v<X.Y.Z>-test.<N> (CI on ctd/main)

    Drafts are never offered. The comparison and parsing live in
    ctd-update-logic.cpp (unit tests: test/ctd-update).

    Nothing is shown or installed while any output is active (streaming,
    recording, replay buffer, virtual camera, the vertical canvas's outputs):
    the notice waits until they have all stopped.

      Windows   downloads CTD-OBS-Setup-*-x64.exe to a temp folder, checks its
                SHA-256 against the release's SHA256SUMS.txt, asks to close,
                starts the installer and closes CTD OBS (which saves the scene
                collection as usual).
      macOS     downloads the app zip to Downloads, checks its SHA-256, shows it
                in Finder with the install steps (docs/ctd/macos-run.md).
                TODO(notarized build): with a Developer ID signed, notarized
                app, replace /Applications/CTD OBS.app automatically instead.

    OBS's own updaters stay off (frontend/obs-main.cpp opt_disable_updater,
    no Sparkle on macOS).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "ctd-update-logic.hpp"
#include "ctd-version.hpp"

#include <widgets/OBSBasic.hpp>
#include <OBSApp.hpp>

#include <qt-wrappers.hpp>
#include <util/curl/curl-helper.h>

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>

static const char kReleasesUrl[] = "https://api.github.com/repos/ItsLimitlezz/ctd-obs/releases?per_page=50";

static const int kStartupDelayMs = 10 * 1000;
static const int kHoldPollMs = 5 * 1000;
static const long long kAutoCheckIntervalSec = 24 * 60 * 60;
static const size_t kMaxApiResponse = 8 * 1024 * 1024;
static const size_t kMaxChecksumFile = 64 * 1024;

/* user.ini */
static const char *const kSection = kCtdUpdateSection;
static const char *const kKeyAuto = kCtdUpdateKeyAuto;
static const char *const kKeyChannel = kCtdUpdateKeyChannel;
static const char kKeyLastCheck[] = "LastCheck";
static const char kKeySkipVersion[] = "SkipVersion";

static const char kMacInstallCommand[] = "xattr -dr com.apple.quarantine \"/Applications/CTD OBS.app\"";

static CtdUpdateChannel CtdConfiguredChannel()
{
	const char *channel = config_get_string(App()->GetUserConfig(), kSection, kKeyChannel);
	return channel && strcmp(channel, "test") == 0 ? CtdUpdateChannel::Test : CtdUpdateChannel::Stable;
}

static QString CtdChannelName(CtdUpdateChannel channel)
{
	return QTStr(channel == CtdUpdateChannel::Test ? "CTD.Settings.Updates.Channel.Test"
						       : "CTD.Settings.Updates.Channel.Stable");
}

static CtdVersion CtdCurrentVersion()
{
	CtdVersion current;
	CtdParseVersion(CtdReleaseVersionString(), current);
	return current;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* HTTP (worker thread)                                                                                              */

struct CtdTransfer {
	const std::atomic<bool> *cancel = nullptr;
	std::function<void(int64_t now, int64_t total)> progress;
	std::string *text = nullptr; /* response kept in memory, up to maxText bytes */
	size_t maxText = 0;
	QFile *file = nullptr; /* or written to this file */
	bool tooLarge = false;
};

static size_t CtdWrite(char *ptr, size_t size, size_t nmemb, void *data)
{
	auto *transfer = static_cast<CtdTransfer *>(data);
	const size_t total = size * nmemb;
	if (transfer->text) {
		if (transfer->text->size() + total > transfer->maxText) {
			transfer->tooLarge = true;
			return 0;
		}
		transfer->text->append(ptr, total);
		return total;
	}
	if (transfer->file) {
		return transfer->file->write(ptr, static_cast<qint64>(total)) == static_cast<qint64>(total) ? total : 0;
	}
	return 0;
}

static int CtdXferInfo(void *data, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
	auto *transfer = static_cast<CtdTransfer *>(data);
	if (transfer->cancel && transfer->cancel->load()) {
		return 1;
	}
	if (transfer->progress) {
		transfer->progress(static_cast<int64_t>(dlnow), static_cast<int64_t>(dltotal));
	}
	return 0;
}

/* GET over HTTPS only. No cookies, no token: the only thing sent besides the URL is the User-Agent (and, for the
 * API, GitHub's Accept and API version headers). */
static bool CtdHttpGet(const std::string &url, bool api, CtdTransfer &transfer, std::string &error)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		error = "curl_easy_init failed";
		return false;
	}

	char errorBuffer[CURL_ERROR_SIZE] = {};
	const std::string userAgent = "CTD-OBS/" + CtdReleaseVersionString();
	struct curl_slist *headers = nullptr;
	if (api) {
		headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
		headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	/* downloads take as long as they take, but not stalled for a minute */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	if (api) {
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	}
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CtdWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CtdXferInfo);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	curl_obs_set_revoke_setting(curl);

	const CURLcode code = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (code == CURLE_OK) {
		return true;
	}
	if (transfer.tooLarge) {
		error = "the response is too large";
	} else if (code == CURLE_ABORTED_BY_CALLBACK) {
		error = "cancelled";
	} else {
		error = *errorBuffer ? errorBuffer : curl_easy_strerror(code);
	}
	return false;
}

/* ---------------------------------------------------------------------------------------------------------------- */

class CtdUpdater : public QObject {
public:
	explicit CtdUpdater(OBSBasic *main);
	~CtdUpdater() override;

	void AutoCheck();
	void Check(bool manual);

private:
	OBSBasic *main;
	QThread *worker = nullptr;
	std::shared_ptr<std::atomic<bool>> cancelFlag;
	bool busy = false;

	/* an update found but not shown yet (an output is active, or another dialog is open) */
	bool hasPending = false;
	bool pendingManual = false;
	bool heldNoticeShown = false;
	CtdRelease pending;
	QTimer holdTimer;

	QPointer<QProgressDialog> progress;

	void RunWorker(std::function<void(const std::atomic<bool> &cancel)> work);
	void StopWorker();

	void CheckFinished(bool manual, bool ok, const std::vector<CtdRelease> &releases, const std::string &error);
	void ShowPendingWhenIdle();
	void ShowUpdateDialog(const CtdRelease &release);
	bool RefuseWhileOutputsActive();
	void Download(const CtdRelease &release);
	void DownloadFinished(const CtdRelease &release, const QString &path, bool ok, bool cancelled,
			      const QString &error);
	void InstallWindows(const CtdRelease &release, const QString &path);
	void ShowMacSteps(const CtdRelease &release, const QString &path);
};

CtdUpdater::CtdUpdater(OBSBasic *main_) : QObject(main_), main(main_)
{
	holdTimer.setInterval(kHoldPollMs);
	connect(&holdTimer, &QTimer::timeout, this, &CtdUpdater::ShowPendingWhenIdle);
	connect(qApp, &QCoreApplication::aboutToQuit, this, &CtdUpdater::StopWorker);
}

CtdUpdater::~CtdUpdater()
{
	StopWorker();
}

void CtdUpdater::RunWorker(std::function<void(const std::atomic<bool> &cancel)> work)
{
	StopWorker();
	cancelFlag = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>> flag = cancelFlag;
	worker = QThread::create([work, flag]() { work(*flag); });
	worker->setObjectName(QStringLiteral("ctd-update"));
	worker->start();
}

void CtdUpdater::StopWorker()
{
	if (cancelFlag) {
		cancelFlag->store(true);
	}
	if (worker) {
		worker->wait();
		delete worker;
		worker = nullptr;
	}
}

void CtdUpdater::AutoCheck()
{
	if (main->isClosing()) {
		return;
	}
	if (!config_get_bool(App()->GetUserConfig(), kSection, kKeyAuto)) {
		blog(LOG_INFO, "[CTD] Update check: automatic check is off");
		return;
	}
	const long long last = config_get_int(App()->GetUserConfig(), kSection, kKeyLastCheck);
	const long long now = static_cast<long long>(time(nullptr));
	if (last > 0 && now >= last && now - last < kAutoCheckIntervalSec) {
		blog(LOG_INFO, "[CTD] Update check: last check less than 24 h ago, skipped");
		return;
	}
	Check(false);
}

void CtdUpdater::Check(bool manual)
{
	if (busy) {
		return;
	}
	if (CtdCurrentPlatform() == CtdUpdatePlatform::Other) {
		return;
	}
	busy = true;
	if (manual) {
		QApplication::setOverrideCursor(Qt::BusyCursor);
	}

	blog(LOG_INFO, "[CTD] Update check (%s, %s channel): CTD OBS %s", manual ? "manual" : "automatic",
	     CtdConfiguredChannel() == CtdUpdateChannel::Test ? "test" : "stable", CtdReleaseVersionString().c_str());

	RunWorker([this, manual](const std::atomic<bool> &cancel) {
		std::string text;
		std::string error;
		CtdTransfer transfer;
		transfer.cancel = &cancel;
		transfer.text = &text;
		transfer.maxText = kMaxApiResponse;

		std::vector<CtdRelease> releases;
		bool ok = CtdHttpGet(kReleasesUrl, true, transfer, error);
		if (ok) {
			ok = CtdParseReleases(text, releases, error);
		}
		if (cancel.load()) {
			return;
		}
		QMetaObject::invokeMethod(
			this, [this, manual, ok, releases, error]() { CheckFinished(manual, ok, releases, error); },
			Qt::QueuedConnection);
	});
}

void CtdUpdater::CheckFinished(bool manual, bool ok, const std::vector<CtdRelease> &releases, const std::string &error)
{
	busy = false;
	if (manual) {
		QApplication::restoreOverrideCursor();
	}
	if (main->isClosing()) {
		return;
	}

	if (!ok) {
		blog(LOG_WARNING, "[CTD] Update check failed: %s", error.c_str());
		if (manual) {
			OBSMessageBox::warning(main, QTStr("CTD.Update.Title"),
					       QTStr("CTD.Update.CheckFailed").arg(QString::fromStdString(error)));
		}
		return;
	}

	config_set_int(App()->GetUserConfig(), kSection, kKeyLastCheck, static_cast<int64_t>(time(nullptr)));
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);

	const CtdUpdateChannel channel = CtdConfiguredChannel();
	const CtdRelease *update = CtdPickUpdate(releases, CtdCurrentVersion(), channel, CtdCurrentPlatform());
	if (!update) {
		blog(LOG_INFO, "[CTD] Update check: %zu releases, CTD OBS %s is up to date", releases.size(),
		     CtdReleaseVersionString().c_str());
		if (manual) {
			OBSMessageBox::information(main, QTStr("CTD.Update.Title"),
						   QTStr("CTD.Update.UpToDate")
							   .arg(QString::fromStdString(CtdReleaseVersionString()),
								CtdChannelName(channel)));
		}
		return;
	}

	blog(LOG_INFO, "[CTD] Update check: %s is available", update->tag.c_str());
	const char *skipped = config_get_string(App()->GetUserConfig(), kSection, kKeySkipVersion);
	if (!manual && skipped && update->tag == skipped) {
		blog(LOG_INFO, "[CTD] Update check: %s was skipped by the user", update->tag.c_str());
		return;
	}

	pending = *update;
	hasPending = true;
	pendingManual = manual;
	heldNoticeShown = false;
	ShowPendingWhenIdle();
}

void CtdUpdater::ShowPendingWhenIdle()
{
	if (!hasPending || main->isClosing()) {
		holdTimer.stop();
		return;
	}

	if (OBSBasic::CtdAnyOutputActive()) {
		if (!holdTimer.isActive()) {
			blog(LOG_INFO, "[CTD] Update %s held until every output has stopped", pending.tag.c_str());
			holdTimer.start();
		}
		/* asked for by hand: say so once, without the update dialog */
		if (pendingManual && !heldNoticeShown) {
			heldNoticeShown = true;
			OBSMessageBox::information(
				main, QTStr("CTD.Update.Title"),
				QTStr("CTD.Update.Held").arg(QString::fromStdString(pending.version.ToString())));
		}
		return;
	}

	/* never on top of another dialog (Settings, a prompt): try again shortly */
	if (QApplication::activeModalWidget()) {
		if (!holdTimer.isActive()) {
			holdTimer.start();
		}
		return;
	}

	holdTimer.stop();
	hasPending = false;
	const CtdRelease release = pending;
	ShowUpdateDialog(release);
}

void CtdUpdater::ShowUpdateDialog(const CtdRelease &release)
{
	QDialog dialog(main);
	dialog.setWindowTitle(QTStr("CTD.Update.Title"));
	dialog.setMinimumSize(520, 380);
	auto *layout = new QVBoxLayout(&dialog);

	const QString version = QString::fromStdString(release.version.ToString());
	auto *heading = new QLabel(
		QTStr(release.version.IsTest() ? "CTD.Update.AvailableTest" : "CTD.Update.Available").arg(version),
		&dialog);
	QFont headingFont = heading->font();
	headingFont.setBold(true);
	headingFont.setPointSizeF(headingFont.pointSizeF() * 1.25);
	heading->setFont(headingFont);
	layout->addWidget(heading);

	auto *current =
		new QLabel(QTStr("CTD.Update.Current").arg(QString::fromStdString(CtdVersionString())), &dialog);
	layout->addWidget(current);

	auto *notes = new QTextBrowser(&dialog);
	notes->setOpenExternalLinks(true);
	notes->setOpenLinks(true);
	if (release.notes.empty()) {
		notes->setPlainText(QTStr("CTD.Update.NoNotes"));
	} else {
		notes->setMarkdown(QString::fromStdString(release.notes));
	}
	layout->addWidget(notes, 1);

	if (!release.pageUrl.empty()) {
		auto *page = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
						.arg(QString::fromStdString(release.pageUrl).toHtmlEscaped(),
						     QTStr("CTD.Update.ReleasePage").toHtmlEscaped()),
					&dialog);
		page->setTextFormat(Qt::RichText);
		page->setOpenExternalLinks(true);
		layout->addWidget(page);
	}

	auto *buttons = new QDialogButtonBox(&dialog);
	QPushButton *install = buttons->addButton(QTStr("CTD.Update.Install"), QDialogButtonBox::AcceptRole);
	QPushButton *skip = buttons->addButton(QTStr("CTD.Update.Skip"), QDialogButtonBox::DestructiveRole);
	buttons->addButton(QTStr("CTD.Update.Later"), QDialogButtonBox::RejectRole);
	install->setDefault(true);
	layout->addWidget(buttons);

	QAbstractButton *clicked = nullptr;
	connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton *button) {
		clicked = button;
		dialog.done(button == install ? QDialog::Accepted : QDialog::Rejected);
	});
	dialog.exec();

	if (main->isClosing()) {
		return;
	}
	if (clicked == install) {
		blog(LOG_INFO, "[CTD] Update %s: download and install", release.tag.c_str());
		Download(release);
	} else if (clicked == skip) {
		blog(LOG_INFO, "[CTD] Update %s: skipped", release.tag.c_str());
		config_set_string(App()->GetUserConfig(), kSection, kKeySkipVersion, release.tag.c_str());
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	} else {
		blog(LOG_INFO, "[CTD] Update %s: later", release.tag.c_str());
	}
}

bool CtdUpdater::RefuseWhileOutputsActive()
{
	if (!OBSBasic::CtdAnyOutputActive()) {
		return false;
	}
	blog(LOG_INFO, "[CTD] Update: not while an output is active");
	OBSMessageBox::information(main, QTStr("CTD.Update.Title"), QTStr("CTD.Update.StopOutputs"));
	return true;
}

/* The release's asset name, if it's safe to use as a file name */
static bool CtdSafeFileName(const std::string &name)
{
	return !name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

void CtdUpdater::Download(const CtdRelease &release)
{
	if (busy || RefuseWhileOutputsActive()) {
		return;
	}

	const CtdReleaseAsset *asset = release.PlatformAsset(CtdCurrentPlatform());
	const CtdReleaseAsset *sums = release.ChecksumAsset();
	if (!asset || !sums || !CtdSafeFileName(asset->name)) {
		return;
	}

#ifdef __APPLE__
	QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	if (dir.isEmpty()) {
		dir = QDir::tempPath();
	}
#else
	const QString dir = QDir::tempPath() + QStringLiteral("/ctd-obs-update");
#endif
	QDir().mkpath(dir);
	const QString path = QDir(dir).filePath(QString::fromStdString(asset->name));

	busy = true;
	progress = new QProgressDialog(QTStr("CTD.Update.Downloading").arg(QString::fromStdString(asset->name)),
				       QTStr("Cancel"), 0, 100, main);
	progress->setWindowTitle(QTStr("CTD.Update.Title"));
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->setAutoClose(false);
	progress->setAutoReset(false);
	progress->setValue(0);
	connect(progress.data(), &QProgressDialog::canceled, this, [this]() {
		if (cancelFlag) {
			cancelFlag->store(true);
		}
	});

	const std::string assetUrl = asset->url;
	const std::string assetName = asset->name;
	const std::string sumsUrl = sums->url;

	RunWorker([this, release, path, assetUrl, assetName, sumsUrl](const std::atomic<bool> &cancel) {
		auto finish = [&](bool ok, const QString &error) {
			const bool cancelled = cancel.load();
			QMetaObject::invokeMethod(
				this,
				[this, release, path, ok, cancelled, error]() {
					DownloadFinished(release, path, ok, cancelled, error);
				},
				Qt::QueuedConnection);
		};

		/* 1. the expected checksum */
		std::string sumsText;
		std::string error;
		CtdTransfer sumsTransfer;
		sumsTransfer.cancel = &cancel;
		sumsTransfer.text = &sumsText;
		sumsTransfer.maxText = kMaxChecksumFile;
		if (!CtdHttpGet(sumsUrl, false, sumsTransfer, error)) {
			finish(false, QString::fromStdString("SHA256SUMS.txt: " + error));
			return;
		}
		const auto sums = CtdParseSha256Sums(sumsText);
		const auto expected = sums.find(assetName);
		if (expected == sums.end()) {
			finish(false, QTStr("CTD.Update.ChecksumMissing").arg(QString::fromStdString(assetName)));
			return;
		}

		/* 2. the file, as <name>.part until it checks out */
		const QString partPath = path + QStringLiteral(".part");
		QFile file(partPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			finish(false, file.errorString());
			return;
		}
		CtdTransfer transfer;
		transfer.cancel = &cancel;
		transfer.file = &file;
		int lastPercent = -1;
		transfer.progress = [this, &lastPercent](int64_t now, int64_t total) {
			const int percent = total > 0 ? static_cast<int>(now * 100 / total) : 0;
			if (percent == lastPercent) {
				return;
			}
			lastPercent = percent;
			QMetaObject::invokeMethod(
				this,
				[this, percent]() {
					if (progress) {
						progress->setValue(percent);
					}
				},
				Qt::QueuedConnection);
		};
		const bool downloaded = CtdHttpGet(assetUrl, false, transfer, error);
		file.close();
		if (!downloaded) {
			QFile::remove(partPath);
			finish(false, QString::fromStdString(error));
			return;
		}

		/* 3. its SHA-256 */
		QCryptographicHash hash(QCryptographicHash::Sha256);
		if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
			QFile::remove(partPath);
			finish(false, file.errorString());
			return;
		}
		file.close();
		const std::string actual = hash.result().toHex().toStdString();
		if (actual != expected->second) {
			blog(LOG_WARNING, "[CTD] Update: %s SHA-256 %s, expected %s", assetName.c_str(), actual.c_str(),
			     expected->second.c_str());
			QFile::remove(partPath);
			finish(false, QTStr("CTD.Update.ChecksumMismatch").arg(QString::fromStdString(assetName)));
			return;
		}

		QFile::remove(path);
		if (!QFile::rename(partPath, path)) {
			QFile::remove(partPath);
			finish(false, QStringLiteral("could not rename %1").arg(partPath));
			return;
		}
		finish(true, QString());
	});
}

void CtdUpdater::DownloadFinished(const CtdRelease &release, const QString &path, bool ok, bool cancelled,
				  const QString &error)
{
	busy = false;
	if (progress) {
		progress->close();
		progress->deleteLater();
	}
	if (main->isClosing()) {
		return;
	}
	if (cancelled) {
		blog(LOG_INFO, "[CTD] Update %s: download cancelled", release.tag.c_str());
		return;
	}
	if (!ok) {
		blog(LOG_WARNING, "[CTD] Update %s: download failed: %s", release.tag.c_str(), QT_TO_UTF8(error));
		OBSMessageBox::warning(main, QTStr("CTD.Update.Title"), QTStr("CTD.Update.DownloadFailed").arg(error));
		return;
	}

	blog(LOG_INFO, "[CTD] Update %s: downloaded and SHA-256 verified", release.tag.c_str());
	if (CtdCurrentPlatform() == CtdUpdatePlatform::Windows) {
		InstallWindows(release, path);
	} else {
		ShowMacSteps(release, path);
	}
}

void CtdUpdater::InstallWindows(const CtdRelease &release, const QString &path)
{
#ifdef _WIN32
	if (RefuseWhileOutputsActive()) {
		return;
	}

	QMessageBox box(main);
	box.setWindowTitle(QTStr("CTD.Update.Title"));
	box.setIcon(QMessageBox::Question);
	box.setText(QTStr("CTD.Update.Win.Confirm").arg(QString::fromStdString(release.version.ToString())));
	QPushButton *install = box.addButton(QTStr("CTD.Update.Win.CloseAndInstall"), QMessageBox::AcceptRole);
	box.addButton(QTStr("Cancel"), QMessageBox::RejectRole);
	box.setDefaultButton(install);
	box.exec();
	if (box.clickedButton() != install || main->isClosing()) {
		blog(LOG_INFO, "[CTD] Update %s: install cancelled", release.tag.c_str());
		return;
	}
	/* an output may have started while the question was open */
	if (RefuseWhileOutputsActive()) {
		return;
	}

	/* The installer needs administrator rights (per-machine install), so it goes through ShellExecuteEx, which asks
	 * for them. It waits for CTD OBS to have closed before it copies files (WaitForAppClosed in ctd-obs.nsi). */
	const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC;
	info.hwnd = reinterpret_cast<HWND>(main->winId());
	info.lpVerb = L"open";
	info.lpFile = file.c_str();
	info.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&info)) {
		const DWORD code = GetLastError();
		blog(LOG_WARNING, "[CTD] Update %s: the installer did not start (error %lu)", release.tag.c_str(),
		     static_cast<unsigned long>(code));
		if (code != ERROR_CANCELLED) {
			OBSMessageBox::warning(
				main, QTStr("CTD.Update.Title"),
				QTStr("CTD.Update.Win.LaunchFailed").arg(QDir::toNativeSeparators(path)));
		}
		return;
	}

	blog(LOG_INFO, "[CTD] Update %s: installer started, closing CTD OBS", release.tag.c_str());
	/* the usual close: saves the scene collection and settings */
	QTimer::singleShot(0, main, [this]() { main->close(); });
#else
	UNUSED_PARAMETER(release);
	UNUSED_PARAMETER(path);
#endif
}

void CtdUpdater::ShowMacSteps(const CtdRelease &release, const QString &path)
{
#ifdef __APPLE__
	QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path});
#endif

	QDialog dialog(main);
	dialog.setWindowTitle(QTStr("CTD.Update.Title"));
	auto *layout = new QVBoxLayout(&dialog);

	auto *steps = new QLabel(QTStr("CTD.Update.Mac.Steps")
					 .arg(QString::fromStdString(release.version.ToString()).toHtmlEscaped(),
					      QFileInfo(path).fileName().toHtmlEscaped(),
					      QDir::toNativeSeparators(QFileInfo(path).absolutePath()).toHtmlEscaped()),
				 &dialog);
	steps->setTextFormat(Qt::RichText);
	steps->setWordWrap(true);
	steps->setTextInteractionFlags(Qt::TextBrowserInteraction);
	layout->addWidget(steps);

	auto *row = new QHBoxLayout();
	auto *command = new QLineEdit(QString::fromLatin1(kMacInstallCommand), &dialog);
	command->setReadOnly(true);
	command->setMinimumWidth(420);
	row->addWidget(command, 1);
	auto *copy = new QPushButton(QTStr("CTD.Update.Mac.Copy"), &dialog);
	connect(copy, &QPushButton::clicked, &dialog,
		[]() { QGuiApplication::clipboard()->setText(QString::fromLatin1(kMacInstallCommand)); });
	row->addWidget(copy);
	layout->addLayout(row);

	auto *after = new QLabel(QTStr("CTD.Update.Mac.After"), &dialog);
	after->setTextFormat(Qt::RichText);
	after->setWordWrap(true);
	layout->addWidget(after);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.exec();
}

/* ---------------------------------------------------------------------------------------------------------------- */

static void CtdUpdateCleanup()
{
#ifdef _WIN32
	/* installers downloaded by an earlier run (after an update they have done their job) */
	QDir dir(QDir::tempPath() + QStringLiteral("/ctd-obs-update"));
	if (dir.exists()) {
		dir.removeRecursively();
	}
#endif
}

void OBSBasic::CtdUpdateInit()
{
	config_t *config = App()->GetUserConfig();
	config_set_default_bool(config, kSection, kKeyAuto, true);
	/* a test build follows test builds until the user picks Stable */
	config_set_default_string(config, kSection, kKeyChannel, CtdIsTestBuild() ? "test" : "stable");

	CtdUpdateCleanup();

	ctdUpdater = new CtdUpdater(this);
	if (CtdCurrentPlatform() != CtdUpdatePlatform::Other) {
		QTimer::singleShot(kStartupDelayMs, ctdUpdater, [this]() { ctdUpdater->AutoCheck(); });
	}
}

void OBSBasic::CtdCheckForUpdates()
{
	if (ctdUpdater) {
		ctdUpdater->Check(true);
	}
}

/******************************************************************************
    CTD OBS: updates from GitHub Releases (docs/ctd/updates.md).

    CTD OBS asks GitHub which CTD OBS releases exist: one anonymous GET of
    https://api.github.com/repos/ItsLimitlezz/ctd-obs/releases with a
    "User-Agent: CTD-OBS/<version>" header, no token, nothing about the user
    or the computer.

      Stable channel   published releases ctd-v<X.Y.Z>
      Test builds      also prereleases ctd-v<X.Y.Z>-test.<N> (CI on ctd/main)

    Drafts are never offered. The comparison and parsing live in
    ctd-update-logic.cpp (unit tests: test/ctd-update).

    At launch (CtdStartupUpdateCheck, from obs-main.cpp before the main
    window, plugins and scenes load): checked with a hard 3 s limit while the
    splash says "Checking for updates…". An update shows the update dialog
    right there; installing it never loads the main window. Offline, rate
    limited, slow or broken answers: startup goes on, one "[CTD] update
    check:" log line. A "nothing new" answer is reused for an hour. Skipped
    for automated launches (--startstreaming and friends, --minimize-to-tray),
    safe mode, --disable-updater and after a crash (obs-main.cpp decides).
    Settings ▸ CTD ▸ Updates turns it off.

    While running: CTD ▸ Check for Updates…. Nothing is shown or installed
    while any output is active (streaming, recording, replay buffer, virtual
    camera, the vertical canvas's outputs): the notice waits until they have
    all stopped.

      Windows   downloads CTD-OBS-Setup-*-x64.exe to a temp folder, checks its
                SHA-256 against the release's SHA256SUMS.txt, starts the
                installer and quits (from a running CTD OBS: asks first, then
                closes it the normal way, which saves the scene collection).
      macOS     downloads the app zip, checks its SHA-256, unzips it with
                ditto, then a small helper script (detached) waits for CTD OBS
                to quit, swaps the app bundle in place (the old one goes to the
                Trash, or is put back if the copy fails) and opens the new one.
                When the bundle's folder isn't writable: shows the zip in
                Finder with the install steps (docs/ctd/macos-run.md) instead.

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

#include "ctd-startup.hpp"
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
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
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

static const int kLaunchCheckLimitMs = 3000;
static const long kLaunchCheckCurlTimeoutSec = 3;
static const long kManualCheckTimeoutSec = 30;
static const int kHoldPollMs = 5 * 1000;
static const size_t kMaxApiResponse = 8 * 1024 * 1024;
static const size_t kMaxChecksumFile = 64 * 1024;

#ifndef _WIN32
static const char kMacInstallCommand[] = "xattr -dr com.apple.quarantine \"/Applications/CTD OBS.app\"";
#endif

static config_t *CtdConfig()
{
	return App()->GetUserConfig();
}

static CtdUpdateChannel CtdConfiguredChannel()
{
	const char *channel = config_get_string(CtdConfig(), kCtdUpdateSection, kCtdUpdateKeyChannel);
	return channel && strcmp(channel, "test") == 0 ? CtdUpdateChannel::Test : CtdUpdateChannel::Stable;
}

static const char *CtdChannelKey(CtdUpdateChannel channel)
{
	return channel == CtdUpdateChannel::Test ? "test" : "stable";
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

static void CtdUpdateSetDefaults()
{
	config_t *config = CtdConfig();
	config_set_default_bool(config, kCtdUpdateSection, kCtdUpdateKeyAuto, true);
	/* a test build follows test builds until the user picks Stable */
	config_set_default_string(config, kCtdUpdateSection, kCtdUpdateKeyChannel,
				  CtdIsTestBuild() ? "test" : "stable");
}

/* Where downloads and the macOS update helper live */
static QString CtdUpdateWorkDir()
{
	return QDir::tempPath() + QStringLiteral("/ctd-obs-update");
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* HTTP (worker threads)                                                                                             */

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
 * API, GitHub's Accept and API version headers). timeoutSec 0: no overall limit (downloads), but not stalled for a
 * minute. */
static bool CtdHttpGet(const std::string &url, bool api, long timeoutSec, CtdTransfer &transfer, std::string &error)
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
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	if (timeoutSec > 0) {
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSec);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
	} else {
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
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

static CtdFetchReleases CtdReleasesFetcher(long timeoutSec)
{
	return [timeoutSec](const std::atomic<bool> &cancel, std::string &body, std::string &error) {
		CtdTransfer transfer;
		transfer.cancel = &cancel;
		transfer.text = &body;
		transfer.maxText = kMaxApiResponse;
		return CtdHttpGet(kReleasesUrl, true, timeoutSec, transfer, error);
	};
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* The update dialog                                                                                                 */

enum class CtdUpdateChoice { Install, Skip, Later };

static CtdUpdateChoice CtdAskToUpdate(QWidget *parent, const CtdRelease &release)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QTStr("CTD.Update.Title"));
	dialog.setMinimumSize(520, 380);
	auto *layout = new QVBoxLayout(&dialog);

	const QString version = QString::fromStdString(release.version.ToString());
	auto *heading = new QLabel(
		QTStr(release.version.IsTest() ? "CTD.Update.AvailableTest" : "CTD.Update.Available").arg(version),
		&dialog);
	QFont headingFont = heading->font();
	headingFont.setBold(true);
	if (headingFont.pointSizeF() > 0) {
		headingFont.setPointSizeF(headingFont.pointSizeF() * 1.25);
	}
	heading->setFont(headingFont);
	layout->addWidget(heading);

	layout->addWidget(
		new QLabel(QTStr("CTD.Update.Current").arg(QString::fromStdString(CtdVersionString())), &dialog));

	auto *notes = new QTextBrowser(&dialog);
	notes->setOpenExternalLinks(true);
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

	if (CtdCurrentPlatform() == CtdUpdatePlatform::MacOS) {
		auto *permissions = new QLabel(QTStr("CTD.Update.Mac.PermissionsNote"), &dialog);
		permissions->setWordWrap(true);
		layout->addWidget(permissions);
	}

	auto *buttons = new QDialogButtonBox(&dialog);
	QPushButton *install = buttons->addButton(QTStr("CTD.Update.Install"), QDialogButtonBox::AcceptRole);
	QPushButton *skip = buttons->addButton(QTStr("CTD.Update.Skip"), QDialogButtonBox::DestructiveRole);
	buttons->addButton(QTStr("CTD.Update.Later"), QDialogButtonBox::RejectRole);
	install->setDefault(true);
	layout->addWidget(buttons);

	QAbstractButton *clicked = nullptr;
	QObject::connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton *button) {
		clicked = button;
		dialog.done(button == install ? QDialog::Accepted : QDialog::Rejected);
	});
	dialog.exec();

	if (clicked == install) {
		blog(LOG_INFO, "[CTD] Update %s: download and install", release.tag.c_str());
		return CtdUpdateChoice::Install;
	}
	if (clicked == skip) {
		blog(LOG_INFO, "[CTD] Update %s: skipped", release.tag.c_str());
		config_set_string(CtdConfig(), kCtdUpdateSection, kCtdUpdateKeySkip, release.tag.c_str());
		config_save_safe(CtdConfig(), "tmp", nullptr);
		return CtdUpdateChoice::Skip;
	}
	blog(LOG_INFO, "[CTD] Update %s: later", release.tag.c_str());
	return CtdUpdateChoice::Later;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Download, SHA-256 check and (macOS) unzip                                                                         */

struct CtdDownload {
	QString file;      /* the verified installer (Windows) or zip (macOS) */
	QString macNewApp; /* macOS: the unzipped CTD OBS.app, empty if unzipping failed */
	QString macError;  /* why it couldn't be unzipped */
};

/* The release's asset name, if it's safe to use as a file name */
static bool CtdSafeFileName(const std::string &name)
{
	return !name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

#ifdef __APPLE__
/* Runs a command line tool, true if it exited with 0; output in out */
static bool CtdRun(const QString &program, const QStringList &args, QString *out = nullptr, int timeoutMs = 120000)
{
	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start(program, args);
	if (!process.waitForFinished(timeoutMs)) {
		process.kill();
		process.waitForFinished(1000);
		return false;
	}
	if (out) {
		*out = QString::fromUtf8(process.readAll()).trimmed();
	}
	return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

static QString CtdPlistValue(const QString &bundle, const QString &key)
{
	QString value;
	if (!CtdRun(QStringLiteral("/usr/libexec/PlistBuddy"),
		    {QStringLiteral("-c"), QStringLiteral("Print :") + key,
		     bundle + QStringLiteral("/Contents/Info.plist")},
		    &value, 10000)) {
		return QString();
	}
	return value;
}

/* The running app bundle (…/CTD OBS.app), or empty when not running from one */
static QString CtdRunningBundle()
{
	QDir dir(QCoreApplication::applicationDirPath()); /* …/CTD OBS.app/Contents/MacOS */
	if (!dir.cdUp() || !dir.cdUp() || !dir.absolutePath().endsWith(QStringLiteral(".app"))) {
		return QString();
	}
	return dir.absolutePath();
}

/* Unzips the verified zip into workDir/new and checks it is a CTD OBS app bundle (worker thread) */
static QString CtdMacUnpack(const QString &zip, const QString &workDir, QString &error)
{
	const QString target = workDir + QStringLiteral("/new");
	QDir(target).removeRecursively();
	QDir().mkpath(target);
	QString out;
	if (!CtdRun(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), zip, target},
		    &out)) {
		error = QStringLiteral("ditto: ") + out;
		return QString();
	}

	QStringList apps;
	QDirIterator it(target, {QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
	while (it.hasNext()) {
		apps << it.next();
	}
	if (apps.size() != 1) {
		error = QStringLiteral("expected one app in the zip, found %1").arg(apps.size());
		return QString();
	}
	const QString app = apps.first();

	const QString identifier = CtdPlistValue(app, QStringLiteral("CFBundleIdentifier"));
	const QString running = CtdRunningBundle();
	const QString expected = running.isEmpty() ? QStringLiteral("com.cinetechdigital.ctd-obs")
						   : CtdPlistValue(running, QStringLiteral("CFBundleIdentifier"));
	if (identifier.isEmpty() || identifier != expected) {
		error = QStringLiteral("the zip holds '%1', not %2").arg(identifier, expected);
		return QString();
	}
	const QString executable = CtdPlistValue(app, QStringLiteral("CFBundleExecutable"));
	if (executable.isEmpty() || !QFileInfo(app + QStringLiteral("/Contents/MacOS/") + executable).isExecutable()) {
		error = QStringLiteral("the app in the zip has no executable");
		return QString();
	}
	if (!CtdRun(QStringLiteral("/usr/bin/codesign"),
		    {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), app}, &out)) {
		/* ad hoc signatures don't always pass --strict: log it, the SHA-256 already matched */
		blog(LOG_WARNING, "[CTD] Update: codesign --verify on the new app: %s", QT_TO_UTF8(out));
	}
	/* downloaded by CTD OBS itself, so not quarantined; removed anyway */
	CtdRun(QStringLiteral("/usr/bin/xattr"), {QStringLiteral("-dr"), QStringLiteral("com.apple.quarantine"), app});
	return app;
}
#endif

/* Downloads this platform's asset, checks it against SHA256SUMS.txt and (macOS) unzips it, with a progress window.
 * Blocks (a local event loop keeps the UI alive). Shows errors itself; false on error or cancel. */
static bool CtdDownloadRelease(QWidget *parent, const CtdRelease &release, CtdDownload &result)
{
	const CtdReleaseAsset *asset = release.PlatformAsset(CtdCurrentPlatform());
	const CtdReleaseAsset *sums = release.ChecksumAsset();
	if (!asset || !sums || !CtdSafeFileName(asset->name)) {
		return false;
	}

	const QString workDir = CtdUpdateWorkDir();
	QDir().mkpath(workDir);
	const QString path = QDir(workDir).filePath(QString::fromStdString(asset->name));
	const std::string assetUrl = asset->url;
	const std::string assetName = asset->name;
	const std::string sumsUrl = sums->url;

	QProgressDialog progress(QTStr("CTD.Update.Downloading").arg(QString::fromStdString(asset->name)),
				 QTStr("Cancel"), 0, 100, parent);
	progress.setWindowTitle(QTStr("CTD.Update.Title"));
	progress.setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
	progress.setMinimumDuration(0);
	progress.setAutoClose(false);
	progress.setAutoReset(false);
	progress.setValue(0);
	progress.show();

	auto cancel = std::make_shared<std::atomic<bool>>(false);
	QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel]() { cancel->store(true); });
	QMetaObject::Connection quitConnection =
		QObject::connect(qApp, &QCoreApplication::aboutToQuit, &progress, [cancel]() { cancel->store(true); });

	struct Outcome {
		bool ok = false;
		QString error;
		QString macNewApp;
		QString macError;
	};
	auto outcome = std::make_shared<Outcome>();
	QPointer<QProgressDialog> progressPtr(&progress);

	QThread *worker = QThread::create([=]() {
		const std::atomic<bool> &stop = *cancel;

		/* 1. the expected checksum */
		std::string sumsText;
		std::string error;
		CtdTransfer sumsTransfer;
		sumsTransfer.cancel = &stop;
		sumsTransfer.text = &sumsText;
		sumsTransfer.maxText = kMaxChecksumFile;
		if (!CtdHttpGet(sumsUrl, false, 0, sumsTransfer, error)) {
			outcome->error = QString::fromStdString("SHA256SUMS.txt: " + error);
			return;
		}
		const auto checksums = CtdParseSha256Sums(sumsText);
		const auto expected = checksums.find(assetName);
		if (expected == checksums.end()) {
			outcome->error = QTStr("CTD.Update.ChecksumMissing").arg(QString::fromStdString(assetName));
			return;
		}

		/* 2. the file, as <name>.part until it checks out */
		const QString partPath = path + QStringLiteral(".part");
		QFile file(partPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			outcome->error = file.errorString();
			return;
		}
		CtdTransfer transfer;
		transfer.cancel = &stop;
		transfer.file = &file;
		int lastPercent = -1;
		transfer.progress = [&lastPercent, progressPtr](int64_t now, int64_t total) {
			const int percent = total > 0 ? static_cast<int>(now * 100 / total) : 0;
			if (percent == lastPercent) {
				return;
			}
			lastPercent = percent;
			QMetaObject::invokeMethod(
				qApp,
				[progressPtr, percent]() {
					if (progressPtr) {
						progressPtr->setValue(percent);
					}
				},
				Qt::QueuedConnection);
		};
		const bool downloaded = CtdHttpGet(assetUrl, false, 0, transfer, error);
		file.close();
		if (!downloaded) {
			QFile::remove(partPath);
			outcome->error = QString::fromStdString(error);
			return;
		}

		/* 3. its SHA-256 */
		QCryptographicHash hash(QCryptographicHash::Sha256);
		if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
			QFile::remove(partPath);
			outcome->error = file.errorString();
			return;
		}
		file.close();
		const std::string actual = hash.result().toHex().toStdString();
		if (actual != expected->second) {
			blog(LOG_WARNING, "[CTD] Update: %s SHA-256 %s, expected %s", assetName.c_str(), actual.c_str(),
			     expected->second.c_str());
			QFile::remove(partPath);
			outcome->error = QTStr("CTD.Update.ChecksumMismatch").arg(QString::fromStdString(assetName));
			return;
		}
		QFile::remove(path);
		if (!QFile::rename(partPath, path)) {
			QFile::remove(partPath);
			outcome->error = QStringLiteral("could not rename %1").arg(partPath);
			return;
		}

#ifdef __APPLE__
		/* 4. unzipped, ready to swap in */
		QMetaObject::invokeMethod(
			qApp,
			[progressPtr]() {
				if (progressPtr) {
					progressPtr->setLabelText(QTStr("CTD.Update.Unpacking"));
					progressPtr->setRange(0, 0);
				}
			},
			Qt::QueuedConnection);
		QString macError;
		outcome->macNewApp = CtdMacUnpack(path, workDir, macError);
		outcome->macError = macError;
#endif
		outcome->ok = true;
	});

	QEventLoop loop;
	QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
	worker->start();
	loop.exec();
	worker->wait();
	delete worker;
	QObject::disconnect(quitConnection);
	/* read before close(): closing a QProgressDialog emits canceled() */
	const bool cancelled = cancel->load();
	progress.close();

	if (cancelled) {
		blog(LOG_INFO, "[CTD] Update %s: download cancelled", release.tag.c_str());
		return false;
	}
	if (!outcome->ok) {
		blog(LOG_WARNING, "[CTD] Update %s: download failed: %s", release.tag.c_str(),
		     QT_TO_UTF8(outcome->error));
		OBSMessageBox::warning(parent, QTStr("CTD.Update.Title"),
				       QTStr("CTD.Update.DownloadFailed").arg(outcome->error));
		return false;
	}

	blog(LOG_INFO, "[CTD] Update %s: downloaded and SHA-256 verified", release.tag.c_str());
	result.file = path;
	result.macNewApp = outcome->macNewApp;
	result.macError = outcome->macError;
	return true;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Install                                                                                                           */

#ifndef _WIN32
/* macOS fallback: the verified zip in Finder (moved to Downloads) with the install steps */
static void CtdShowManualSteps(QWidget *parent, const CtdRelease &release, const QString &zip)
{
	QString path = zip;
#ifdef __APPLE__
	const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	if (!downloads.isEmpty()) {
		const QString moved = QDir(downloads).filePath(QFileInfo(zip).fileName());
		QFile::remove(moved);
		if (QFile::rename(zip, moved) || QFile::copy(zip, moved)) {
			path = moved;
		}
	}
	QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path});
#endif

	QDialog dialog(parent);
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
	QObject::connect(copy, &QPushButton::clicked, &dialog,
			 []() { QGuiApplication::clipboard()->setText(QString::fromLatin1(kMacInstallCommand)); });
	row->addWidget(copy);
	layout->addLayout(row);

	auto *after = new QLabel(QTStr("CTD.Update.Mac.After"), &dialog);
	after->setTextFormat(Qt::RichText);
	after->setWordWrap(true);
	layout->addWidget(after);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.exec();
}
#endif

#ifdef __APPLE__
/* The helper that swaps the bundle once CTD OBS has quit: arguments pid, new app, target, backup, log */
static const char kMacHelperScript[] = R"sh(#!/bin/sh
# CTD OBS update helper (frontend/ctd/ctd-update.cpp): waits for CTD OBS to quit, replaces the app bundle, opens it.
pid="$1"; new_app="$2"; target="$3"; backup="$4"; log="$5"
exec >> "$log" 2>&1
echo "$(date '+%Y-%m-%d %H:%M:%S') waiting for CTD OBS (pid $pid) to quit"
n=0
while kill -0 "$pid" 2> /dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 1200 ]; then
    echo "CTD OBS still running after 10 minutes: nothing changed"
    exit 1
  fi
  sleep 0.5
done
rm -rf "$backup"
if ! mv "$target" "$backup"; then
  echo "could not move the old app aside: nothing changed"
  open "$target"
  exit 1
fi
if ditto "$new_app" "$target"; then
  xattr -dr com.apple.quarantine "$target" 2> /dev/null
  echo "installed $target"
  trash="$HOME/.Trash/CTD OBS (before update $(date '+%Y-%m-%d %H.%M.%S')).app"
  mv "$backup" "$trash" 2> /dev/null && echo "old app moved to the Trash" || echo "old app kept at $backup"
  rm -rf "$new_app"
  open "$target"
else
  echo "copy failed: putting the old app back"
  rm -rf "$target"
  mv "$backup" "$target" && echo "old app restored"
  open "$target"
  exit 1
fi
)sh";

/* Whether CTD OBS can replace its own bundle; why not in reason */
static bool CtdMacCanReplace(QString &target, QString &reason)
{
	target = CtdRunningBundle();
	if (target.isEmpty()) {
		reason = QStringLiteral("not running from an app bundle");
		return false;
	}
	if (target.contains(QStringLiteral("/AppTranslocation/"))) {
		reason = QStringLiteral("macOS runs it from a read-only copy (App Translocation)");
		return false;
	}
	const QFileInfo bundle(target);
	const QFileInfo folder(bundle.absolutePath());
	if (!folder.isWritable() || !bundle.isWritable()) {
		reason = QStringLiteral("%1 is not writable").arg(folder.absoluteFilePath());
		return false;
	}
	return true;
}

static bool CtdMacStartHelper(const QString &newApp, const QString &target, QString &error)
{
	const QString workDir = CtdUpdateWorkDir();
	const QString script = workDir + QStringLiteral("/ctd-obs-update.sh");
	QFile file(script);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
	    file.write(kMacHelperScript) != static_cast<qint64>(sizeof(kMacHelperScript) - 1)) {
		error = file.errorString();
		return false;
	}
	file.close();

	const QStringList args = {
		script, QString::number(QCoreApplication::applicationPid()), newApp,
		target, workDir + QStringLiteral("/CTD OBS (previous).app"), workDir + QStringLiteral("/update.log")};
	if (!QProcess::startDetached(QStringLiteral("/bin/sh"), args)) {
		error = QStringLiteral("could not start /bin/sh");
		return false;
	}
	blog(LOG_INFO, "[CTD] Update: helper started, it replaces %s once CTD OBS has quit", QT_TO_UTF8(target));
	return true;
}
#endif

#ifdef _WIN32
/* The installer needs administrator rights (per-machine install), so it goes through ShellExecuteEx, which asks for
 * them. It waits for CTD OBS to have exited before it copies files (WaitForAppClosed in ctd-obs.nsi). */
static bool CtdStartInstaller(QWidget *parent, const CtdRelease &release, const QString &path)
{
	const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC;
	info.hwnd = parent ? reinterpret_cast<HWND>(parent->winId()) : nullptr;
	info.lpVerb = L"open";
	info.lpFile = file.c_str();
	info.nShow = SW_SHOWNORMAL;
	if (ShellExecuteExW(&info)) {
		blog(LOG_INFO, "[CTD] Update %s: installer started", release.tag.c_str());
		return true;
	}
	const DWORD code = GetLastError();
	blog(LOG_WARNING, "[CTD] Update %s: the installer did not start (error %lu)", release.tag.c_str(),
	     static_cast<unsigned long>(code));
	if (code != ERROR_CANCELLED) {
		OBSMessageBox::warning(parent, QTStr("CTD.Update.Title"),
				       QTStr("CTD.Update.Win.LaunchFailed").arg(QDir::toNativeSeparators(path)));
	}
	return false;
}
#endif

/* Installs a downloaded update. Returns true when CTD OBS must quit now (the installer or the swap helper is
 * waiting for it). From a running CTD OBS (confirmClose) it asks first; mayProceed is checked right before the point
 * of no return (outputs may have started meanwhile). */
static bool CtdInstall(QWidget *parent, const CtdRelease &release, const CtdDownload &download, bool confirmClose,
		       const std::function<bool()> &mayProceed)
{
	const QString version = QString::fromStdString(release.version.ToString());

	auto confirm = [&](const char *text) {
		if (!confirmClose) {
			return true;
		}
		QMessageBox box(parent);
		box.setWindowTitle(QTStr("CTD.Update.Title"));
		box.setIcon(QMessageBox::Question);
		box.setText(QTStr(text).arg(version));
		QPushButton *install = box.addButton(QTStr("CTD.Update.CloseAndInstall"), QMessageBox::AcceptRole);
		box.addButton(QTStr("Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(install);
		box.exec();
		if (box.clickedButton() != install) {
			blog(LOG_INFO, "[CTD] Update %s: install cancelled", release.tag.c_str());
			return false;
		}
		return true;
	};

#ifdef _WIN32
	if (!confirm("CTD.Update.Win.Confirm") || (mayProceed && !mayProceed())) {
		return false;
	}
	return CtdStartInstaller(parent, release, download.file);
#elif defined(__APPLE__)
	QString target;
	QString reason = download.macError;
	if (download.macNewApp.isEmpty() || !CtdMacCanReplace(target, reason)) {
		blog(LOG_INFO, "[CTD] Update %s: can't replace the app (%s), showing the steps instead",
		     release.tag.c_str(), QT_TO_UTF8(reason));
		CtdShowManualSteps(parent, release, download.file);
		return false;
	}
	if (!confirm("CTD.Update.Mac.Confirm") || (mayProceed && !mayProceed())) {
		return false;
	}
	QString error;
	if (!CtdMacStartHelper(download.macNewApp, target, error)) {
		blog(LOG_WARNING, "[CTD] Update %s: %s, showing the steps instead", release.tag.c_str(),
		     QT_TO_UTF8(error));
		CtdShowManualSteps(parent, release, download.file);
		return false;
	}
	return true;
#else
	UNUSED_PARAMETER(confirm);
	UNUSED_PARAMETER(mayProceed);
	CtdShowManualSteps(parent, release, download.file);
	return false;
#endif
}

/* Last run's leftovers: downloaded installers, the macOS helper's work (its log goes into this run's log first) */
static void CtdUpdateCleanup()
{
	QDir dir(CtdUpdateWorkDir());
	if (!dir.exists()) {
		return;
	}
	QFile log(dir.filePath(QStringLiteral("update.log")));
	if (log.open(QIODevice::ReadOnly)) {
		for (const QByteArray &line : log.readAll().split('\n')) {
			if (!line.trimmed().isEmpty()) {
				blog(LOG_INFO, "[CTD] Update helper: %s", line.constData());
			}
		}
		log.close();
	}
	dir.removeRecursively();
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* At launch, before the main window                                                                                 */

bool CtdStartupUpdateCheck()
{
	if (CtdCurrentPlatform() == CtdUpdatePlatform::Other) {
		return true;
	}
	CtdUpdateSetDefaults();
	CtdUpdateCleanup();

	config_t *config = CtdConfig();
	if (!config_get_bool(config, kCtdUpdateSection, kCtdUpdateKeyAuto)) {
		blog(LOG_INFO, "[CTD] update check: off (Settings > CTD > Updates)");
		return true;
	}

	const CtdUpdateChannel channel = CtdConfiguredChannel();
	const std::string version = CtdReleaseVersionString();
	const long long now = static_cast<long long>(time(nullptr));
	const char *cachedVersion = config_get_string(config, kCtdUpdateSection, kCtdUpdateKeyCacheVersion);
	const char *cachedChannel = config_get_string(config, kCtdUpdateSection, kCtdUpdateKeyCacheChannel);
	if (CtdUpToDateCacheValid(now, config_get_int(config, kCtdUpdateSection, kCtdUpdateKeyCacheTime),
				  cachedVersion ? cachedVersion : "", cachedChannel ? cachedChannel : "", version,
				  CtdChannelKey(channel))) {
		blog(LOG_INFO, "[CTD] update check: CTD OBS %s was up to date less than an hour ago, not asked again",
		     version.c_str());
		return true;
	}

	CtdSplashSetStage(CtdSplashStage::Updates);
	const CtdCheckResult result = CtdRunCheck(
		CtdReleasesFetcher(kLaunchCheckCurlTimeoutSec), CtdCurrentVersion(), channel, CtdCurrentPlatform(),
		kLaunchCheckLimitMs, []() { QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }, 30);

	switch (result.status) {
	case CtdCheckResult::Status::Failed:
	case CtdCheckResult::Status::TimedOut:
		blog(LOG_INFO, "[CTD] update check: %s (%s), starting CTD OBS %s", CtdCheckStatusName(result.status),
		     result.error.c_str(), version.c_str());
		return true;
	case CtdCheckResult::Status::UpToDate:
		blog(LOG_INFO, "[CTD] update check: CTD OBS %s is up to date (%s channel, %zu releases)",
		     version.c_str(), CtdChannelKey(channel), result.releaseCount);
		config_set_int(config, kCtdUpdateSection, kCtdUpdateKeyCacheTime, now);
		config_set_string(config, kCtdUpdateSection, kCtdUpdateKeyCacheVersion, version.c_str());
		config_set_string(config, kCtdUpdateSection, kCtdUpdateKeyCacheChannel, CtdChannelKey(channel));
		config_save_safe(config, "tmp", nullptr);
		return true;
	case CtdCheckResult::Status::UpdateAvailable:
		break;
	}

	const CtdRelease &release = result.update;
	config_set_int(config, kCtdUpdateSection, kCtdUpdateKeyCacheTime, 0);
	const char *skipped = config_get_string(config, kCtdUpdateSection, kCtdUpdateKeySkip);
	if (skipped && release.tag == skipped) {
		blog(LOG_INFO, "[CTD] update check: %s is available, skipped by the user", release.tag.c_str());
		return true;
	}
	blog(LOG_INFO, "[CTD] update check: %s is available (CTD OBS %s)", release.tag.c_str(), version.c_str());

	CtdSplashSuspend();
	bool quit = false;
	if (CtdAskToUpdate(nullptr, release) == CtdUpdateChoice::Install) {
		CtdDownload download;
		if (CtdDownloadRelease(nullptr, release, download)) {
			quit = CtdInstall(nullptr, release, download, false, {});
		}
	}
	if (quit) {
		blog(LOG_INFO, "[CTD] Update %s: quitting for the update", release.tag.c_str());
		return false;
	}
	CtdSplashResume();
	return true;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* While running: CTD ▸ Check for Updates…                                                                           */

class CtdUpdater : public QObject {
public:
	explicit CtdUpdater(OBSBasic *main);
	~CtdUpdater() override;

	void Check();

private:
	OBSBasic *main;
	QThread *worker = nullptr;
	std::shared_ptr<std::atomic<bool>> cancelFlag;
	bool busy = false;

	/* an update found but not shown yet (an output is active, or another dialog is open) */
	bool hasPending = false;
	bool heldNoticeShown = false;
	CtdRelease pending;
	QTimer holdTimer;

	void StopWorker();
	void CheckFinished(bool ok, const std::vector<CtdRelease> &releases, const std::string &error);
	void ShowPendingWhenIdle();
	bool RefuseWhileOutputsActive();
	void Offer(const CtdRelease &release);
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

void CtdUpdater::Check()
{
	if (busy || CtdCurrentPlatform() == CtdUpdatePlatform::Other) {
		return;
	}
	busy = true;
	QApplication::setOverrideCursor(Qt::BusyCursor);
	blog(LOG_INFO, "[CTD] Update check (manual, %s channel): CTD OBS %s", CtdChannelKey(CtdConfiguredChannel()),
	     CtdReleaseVersionString().c_str());

	StopWorker();
	cancelFlag = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>> flag = cancelFlag;
	worker = QThread::create([this, flag]() {
		std::string body;
		std::string error;
		std::vector<CtdRelease> releases;
		bool ok = CtdReleasesFetcher(kManualCheckTimeoutSec)(*flag, body, error);
		if (ok) {
			ok = CtdParseReleases(body, releases, error);
		}
		if (flag->load()) {
			return;
		}
		QMetaObject::invokeMethod(
			this, [this, ok, releases, error]() { CheckFinished(ok, releases, error); },
			Qt::QueuedConnection);
	});
	worker->setObjectName(QStringLiteral("ctd-update-check"));
	worker->start();
}

void CtdUpdater::CheckFinished(bool ok, const std::vector<CtdRelease> &releases, const std::string &error)
{
	busy = false;
	QApplication::restoreOverrideCursor();
	if (main->isClosing()) {
		return;
	}

	if (!ok) {
		blog(LOG_WARNING, "[CTD] Update check failed: %s", error.c_str());
		OBSMessageBox::warning(main, QTStr("CTD.Update.Title"),
				       QTStr("CTD.Update.CheckFailed").arg(QString::fromStdString(error)));
		return;
	}

	const CtdUpdateChannel channel = CtdConfiguredChannel();
	const CtdRelease *update = CtdPickUpdate(releases, CtdCurrentVersion(), channel, CtdCurrentPlatform());
	if (!update) {
		blog(LOG_INFO, "[CTD] Update check: %zu releases, CTD OBS %s is up to date", releases.size(),
		     CtdReleaseVersionString().c_str());
		OBSMessageBox::information(main, QTStr("CTD.Update.Title"),
					   QTStr("CTD.Update.UpToDate")
						   .arg(QString::fromStdString(CtdReleaseVersionString()),
							CtdChannelName(channel)));
		return;
	}

	/* asked for by hand: offered even if it was skipped */
	blog(LOG_INFO, "[CTD] Update check: %s is available", update->tag.c_str());
	pending = *update;
	hasPending = true;
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
		if (!heldNoticeShown) {
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
	Offer(release);
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

void CtdUpdater::Offer(const CtdRelease &release)
{
	if (CtdAskToUpdate(main, release) != CtdUpdateChoice::Install || main->isClosing()) {
		return;
	}
	if (RefuseWhileOutputsActive()) {
		return;
	}

	busy = true;
	CtdDownload download;
	const bool downloaded = CtdDownloadRelease(main, release, download);
	busy = false;
	if (!downloaded || main->isClosing()) {
		return;
	}

	if (CtdInstall(main, release, download, true, [this]() { return !RefuseWhileOutputsActive(); })) {
		blog(LOG_INFO, "[CTD] Update %s: closing CTD OBS for the update", release.tag.c_str());
		/* the usual close: saves the scene collection and settings */
		QTimer::singleShot(0, main, [this]() { main->close(); });
	}
}

/* ---------------------------------------------------------------------------------------------------------------- */

void OBSBasic::CtdUpdateInit()
{
	CtdUpdateSetDefaults();
	ctdUpdater = new CtdUpdater(this);
}

void OBSBasic::CtdCheckForUpdates()
{
	if (ctdUpdater) {
		ctdUpdater->Check();
	}
}

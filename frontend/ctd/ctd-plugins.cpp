/******************************************************************************
    CTD OBS: plugins installed for OBS Studio (docs/ctd/plugins.md).

    CTD OBS loads third-party plugins only from its own folder:

      macOS    ~/Library/Application Support/ctd-obs/plugins/<name>.plugin
      Windows  C:\ProgramData\ctd-obs\plugins\<name>
      Linux    $XDG_DATA_HOME/ctd-obs/plugins/<name>,
               $XDG_CONFIG_HOME/ctd-obs/plugins/<name> (legacy layout)

    never from OBS Studio's (frontend/plugin-manager/PluginModuleLoader_*).
    Plugins installed for OBS Studio are built against its libobs and Qt and
    can crash CTD OBS, and CTD OBS must not change anything of OBS Studio's.

    Once, the first time CTD OBS finds plugins in OBS Studio's folders, it
    lists them here ("these belong to your regular OBS and aren't loaded in
    CTD OBS"), each with Copy into CTD OBS. CTD ▸ Advanced ▸ Plugins from
    OBS Studio… opens the same list at any time. Copying never moves, changes
    or deletes OBS Studio's files.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-plugin-isolation-logic.hpp"

#include <widgets/OBSBasic.hpp>
#include <OBSApp.hpp>

#include <qt-wrappers.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern bool opt_start_streaming;
extern bool opt_start_recording;
extern bool opt_start_replaybuffer;
extern bool opt_start_virtualcam;
extern bool opt_minimize_tray;

namespace fs = std::filesystem;

namespace {

/* [CTDPlugins] in the app config (global.ini / user.ini app section) */
constexpr const char *kConfigSection = "CTDPlugins";
constexpr const char *kNoticeShownKey = "StockNoticeShown";

/* OBS Studio's own modules (its install folder has them next to third-party DLLs). CTD OBS builds them all in as
 * core modules, so obs_get_module_allow_disable() already says so; this list covers modules a CTD OBS build leaves
 * out on some platform. */
const char *const kObsStudioModules[] = {
	"aja",
	"aja-output-ui",
	"coreaudio-encoder",
	"decklink",
	"decklink-captions",
	"decklink-output-ui",
	"frontend-tools",
	"image-source",
	"nv-filters",
	"obs-browser",
	"obs-ffmpeg",
	"obs-filters",
	"obs-libfdk",
	"obs-nvenc",
	"obs-outputs",
	"obs-qsv11",
	"obs-text",
	"obs-transitions",
	"obs-vst",
	"obs-webrtc",
	"obs-websocket",
	"obs-x264",
	"rtmp-services",
	"text-freetype2",
	"vlc-video",
	"win-capture",
	"win-dshow",
	"win-wasapi",
	"enc-amf",
	"obs-amf-test",
};

bool IsBundledModule(const std::string &name)
{
	if (!obs_get_module_allow_disable(name.c_str())) {
		return true; /* one of CTD OBS's core modules */
	}
	for (const char *own : kObsStudioModules) {
		if (name == own) {
			return true;
		}
	}
	return false;
}

fs::path ToPath(const std::string &utf8)
{
#if defined(__cpp_char8_t)
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
	return fs::u8path(utf8);
#endif
}

fs::path ToPath(const char *utf8)
{
	return utf8 ? ToPath(std::string(utf8)) : fs::path();
}

[[maybe_unused]] fs::path ToPath(const QString &text)
{
	return ToPath(text.toStdString());
}

QString FromPath(const fs::path &path)
{
#if defined(__cpp_char8_t)
	const std::u8string text = path.u8string();
	return QString::fromUtf8(reinterpret_cast<const char *>(text.data()), static_cast<qsizetype>(text.size()));
#else
	return QString::fromStdString(path.u8string());
#endif
}

[[maybe_unused]] fs::path ConfigPath(const char *name)
{
	BPtr<char> path = os_get_config_path_ptr(name);
	return ToPath(path.Get());
}

[[maybe_unused]] fs::path ProgramDataPath(const char *name)
{
	BPtr<char> path = os_get_program_data_path_ptr(name);
	return ToPath(path.Get());
}

[[maybe_unused]] fs::path XdgDir(const char *variable, const char *homeFallback)
{
	const char *value = getenv(variable);
	if (value && *value) {
		return ToPath(value);
	}
	const char *home = getenv("HOME");
	return (home && *home) ? ToPath(home) / homeFallback : fs::path();
}

/* OBS Studio's plugin folders on this platform, and the CTD OBS folder each one's plugins are copied to */
std::vector<CtdStockPluginRoot> StockRoots()
{
	std::vector<CtdStockPluginRoot> roots;

#if defined(__APPLE__)
	const fs::path ctd = ConfigPath("ctd-obs/plugins");
	roots.push_back({ConfigPath("obs-studio/plugins"), ctd, CtdPluginLayout::Bundle, {}});
	/* /Library/Application Support: plugin installers (.pkg) may install for every user */
	roots.push_back({ProgramDataPath("obs-studio/plugins"), ctd, CtdPluginLayout::Bundle, {}});
#elif defined(_WIN32)
	const fs::path ctd = ProgramDataPath("ctd-obs/plugins");
	roots.push_back({ProgramDataPath("obs-studio/plugins"), ctd, CtdPluginLayout::Folder, {}});

	/* Older plugin installers copy into OBS Studio's own install folder */
	QSettings obsKey("HKEY_LOCAL_MACHINE\\SOFTWARE\\OBS Studio", QSettings::NativeFormat);
	QString obsDir = obsKey.value("Default").toString();
	if (obsDir.isEmpty()) {
		const QString programFiles = qEnvironmentVariable("ProgramFiles");
		if (!programFiles.isEmpty()) {
			obsDir = programFiles + "/obs-studio";
		}
	}
	if (!obsDir.isEmpty()) {
		const fs::path obs = ToPath(QDir::fromNativeSeparators(obsDir));
		roots.push_back(
			{obs / "obs-plugins" / "64bit", ctd, CtdPluginLayout::LegacyDll, obs / "data" / "obs-plugins"});
	}
#else
	const fs::path dataHome = XdgDir("XDG_DATA_HOME", ".local/share");
	const fs::path configHome = XdgDir("XDG_CONFIG_HOME", ".config");
	if (!dataHome.empty()) {
		roots.push_back({dataHome / "obs-studio" / "plugins",
				 dataHome / "ctd-obs" / "plugins",
				 CtdPluginLayout::Folder,
				 {}});
	}
	if (!configHome.empty()) {
		roots.push_back({configHome / "obs-studio" / "plugins",
				 configHome / "ctd-obs" / "plugins",
				 CtdPluginLayout::Folder,
				 {}});
	}
#endif

	/* a folder the platform can't name comes back empty: drop it */
	std::vector<CtdStockPluginRoot> usable;
	for (CtdStockPluginRoot &root : roots) {
		if (!root.stockDir.empty() && !root.ctdDir.empty()) {
			usable.push_back(std::move(root));
		}
	}
	return usable;
}

/* Where CTD OBS's own third-party plugins go on this platform (shown and opened from the list) */
fs::path CtdPluginDir()
{
#if defined(_WIN32)
	return ProgramDataPath("ctd-obs/plugins");
#elif defined(__APPLE__)
	return ConfigPath("ctd-obs/plugins");
#else
	const fs::path dataHome = XdgDir("XDG_DATA_HOME", ".local/share");
	return dataHome.empty() ? fs::path() : dataHome / "ctd-obs" / "plugins";
#endif
}

void LogStockPlugins(const std::vector<CtdStockPlugin> &plugins)
{
	if (plugins.empty()) {
		blog(LOG_INFO, "[CTD] plugins: none installed for OBS Studio");
		return;
	}
	for (const CtdStockPlugin &plugin : plugins) {
		blog(LOG_INFO, "[CTD] plugins: '%s' is installed for OBS Studio (%s)%s, not loaded",
		     plugin.name.c_str(), FromPath(plugin.source).toUtf8().constData(),
		     plugin.alreadyInCtd ? "; one with that name is in CTD OBS's folder" : "");
	}
}

class StockPluginsDialog : public QDialog {
public:
	StockPluginsDialog(QWidget *parent, std::vector<CtdStockPlugin> plugins)
		: QDialog(parent),
		  plugins_(std::move(plugins))
	{
		setWindowTitle(QTStr("CTD.StockPlugins.Title"));
		setMinimumWidth(560);

		auto *layout = new QVBoxLayout(this);

		auto *heading = new QLabel(QTStr("CTD.StockPlugins.Heading"), this);
		heading->setTextFormat(Qt::PlainText);
		heading->setWordWrap(true);
		QFont headingFont = heading->font();
		headingFont.setBold(true);
		heading->setFont(headingFont);
		layout->addWidget(heading);

		auto *text = new QLabel(QTStr("CTD.StockPlugins.Text"), this);
		text->setTextFormat(Qt::PlainText);
		text->setWordWrap(true);
		layout->addWidget(text);

		auto *list = new QWidget(this);
		auto *grid = new QGridLayout(list);
		grid->setColumnStretch(0, 1);
		for (size_t i = 0; i < plugins_.size(); i++) {
			const CtdStockPlugin &plugin = plugins_[i];
			const int row = static_cast<int>(i);

			auto *name = new QLabel(list);
			name->setTextFormat(Qt::PlainText);
			name->setText(QString::fromStdString(plugin.name));
			QFont nameFont = name->font();
			nameFont.setBold(true);
			name->setFont(nameFont);
			auto *where = new QLabel(list);
			where->setTextFormat(Qt::PlainText);
			where->setText(QDir::toNativeSeparators(FromPath(plugin.source)));
			where->setTextInteractionFlags(Qt::TextSelectableByMouse);
			where->setWordWrap(true);
			auto *cell = new QVBoxLayout();
			cell->setSpacing(0);
			cell->addWidget(name);
			cell->addWidget(where);
			grid->addLayout(cell, row, 0);

			auto *copy = new QPushButton(QTStr("CTD.StockPlugins.Copy"), list);
			if (plugin.alreadyInCtd) {
				copy->setText(QTStr("CTD.StockPlugins.AlreadyInCtd"));
				copy->setEnabled(false);
			}
			connect(copy, &QPushButton::clicked, this, [this, i, copy]() { Copy(i, copy); });
			grid->addWidget(copy, row, 1, Qt::AlignRight | Qt::AlignVCenter);
		}

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(list);
		layout->addWidget(scroll, 1);

		auto *warning = new QLabel(QTStr("CTD.StockPlugins.Warning"), this);
		warning->setTextFormat(Qt::PlainText);
		warning->setWordWrap(true);
		layout->addWidget(warning);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
		QPushButton *openFolder =
			buttons->addButton(QTStr("CTD.StockPlugins.OpenFolder"), QDialogButtonBox::ActionRole);
		connect(openFolder, &QPushButton::clicked, this, []() {
			const fs::path dir = CtdPluginDir();
			if (dir.empty()) {
				return;
			}
			std::error_code ec;
			fs::create_directories(dir, ec);
			QDesktopServices::openUrl(QUrl::fromLocalFile(FromPath(dir)));
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);
	}

private:
	std::vector<CtdStockPlugin> plugins_;

	void Copy(size_t index, QPushButton *button)
	{
		if (index >= plugins_.size()) {
			return;
		}
		const CtdStockPlugin plugin = plugins_[index];
		const QString name = QString::fromStdString(plugin.name);

		QMessageBox confirm(this);
		confirm.setIcon(QMessageBox::Warning);
		confirm.setWindowTitle(QTStr("CTD.StockPlugins.Title"));
		confirm.setTextFormat(Qt::PlainText);
		confirm.setText(QTStr("CTD.StockPlugins.Confirm").arg(name));
		confirm.setInformativeText(QTStr("CTD.StockPlugins.ConfirmText")
						   .arg(name, QDir::toNativeSeparators(FromPath(plugin.destination))));
		QPushButton *yes = confirm.addButton(QTStr("CTD.StockPlugins.Copy"), QMessageBox::AcceptRole);
		confirm.addButton(QMessageBox::Cancel);
		confirm.setDefaultButton(QMessageBox::Cancel);
		confirm.exec();
		if (confirm.clickedButton() != yes) {
			return;
		}

		button->setEnabled(false);
		button->setText(QTStr("CTD.StockPlugins.Copying"));

		/* A plugin can be large; copy off the UI thread and report back to it. The button may be gone by then
		 * (dialog closed), so only a guarded pointer crosses back. */
		QPointer<QPushButton> guard(button);
		std::thread([plugin, guard, name]() {
			std::string error;
			const bool copied = CtdCopyStockPlugin(plugin, error);
			if (copied) {
				blog(LOG_INFO, "[CTD] plugins: copied '%s' from %s to %s", plugin.name.c_str(),
				     FromPath(plugin.source).toUtf8().constData(),
				     FromPath(plugin.destination).toUtf8().constData());
			} else {
				blog(LOG_WARNING, "[CTD] plugins: copying '%s' failed: %s", plugin.name.c_str(),
				     error.c_str());
			}
			const QString reason = QString::fromStdString(error);
			QMetaObject::invokeMethod(
				qApp,
				[guard, copied, name, reason]() {
					if (!guard) {
						return;
					}
					if (copied) {
						guard->setText(QTStr("CTD.StockPlugins.Copied"));
						return;
					}
					guard->setText(QTStr("CTD.StockPlugins.Copy"));
					guard->setEnabled(true);
					OBSMessageBox::warning(guard->window(), QTStr("CTD.StockPlugins.Title"),
							       QTStr("CTD.StockPlugins.CopyFailed").arg(name, reason));
				},
				Qt::QueuedConnection);
		}).detach();
	}
};

} // namespace

void OBSBasic::CtdStockPluginsNotice(bool atLaunch)
{
	std::vector<CtdStockPlugin> plugins = CtdFindStockPlugins(StockRoots(), IsBundledModule);

	if (atLaunch) {
		LogStockPlugins(plugins);

		const bool unattended = opt_start_streaming || opt_start_recording || opt_start_replaybuffer ||
					opt_start_virtualcam || opt_minimize_tray;
		if (plugins.empty() || unattended || Active() || !isVisible() ||
		    config_get_bool(App()->GetAppConfig(), kConfigSection, kNoticeShownKey)) {
			return;
		}
		/* only plugins CTD OBS doesn't already have a copy of are worth interrupting for */
		bool anyNew = false;
		for (const CtdStockPlugin &plugin : plugins) {
			anyNew = anyNew || !plugin.alreadyInCtd;
		}
		if (!anyNew) {
			return;
		}
		config_set_bool(App()->GetAppConfig(), kConfigSection, kNoticeShownKey, true);
		config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	}

	if (plugins.empty()) {
		OBSMessageBox::information(this, QTStr("CTD.StockPlugins.Title"), QTStr("CTD.StockPlugins.None"));
		return;
	}

	StockPluginsDialog dialog(this, std::move(plugins));
	dialog.exec();
}

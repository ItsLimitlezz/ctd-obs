/******************************************************************************
    CTD OBS: the first-run step (integration gap 16).

    On a fresh config CTD OBS asks to pair with CTD Relay instead of opening
    OBS's auto-configuration wizard straight away. "Pair with CTD Relay"
    shows the CTD Relay dock ready for its address; the wizard stays one
    click away (a button here, and Tools ▸ Auto-Configuration Wizard).
    Without the CTD plugin it runs the wizard as OBS does.

    Called here, registered by the CTD plugin (ctd-obs-plugin, src/ui/multistream-dock.cpp):

      void ctd_relay_show_pairing()
          Shows and raises the CTD Relay dock and focuses its address field
          when it isn't connected. UI thread only. Without it, the dock is
          shown and raised here.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QDockWidget>
#include <QMessageBox>
#include <QPushButton>

/* The CTD Relay dock's id (the CTD plugin's saved dock id; never rename it) */
static const char *const kCtdRelayDockId = "ctd-multistream";

void OBSBasic::CtdShowRelayPairing()
{
	calldata_t cd = {};
	const bool handled = proc_handler_call(obs_get_proc_handler(), "ctd_relay_show_pairing", &cd);
	calldata_free(&cd);
	if (handled) {
		return;
	}

	if (QDockWidget *relay = findChild<QDockWidget *>(QString::fromLatin1(kCtdRelayDockId))) {
		relay->setVisible(true);
		relay->raise();
		relay->activateWindow();
	}
}

void OBSBasic::CtdFirstRunWelcome()
{
	if (!findChild<QDockWidget *>(QString::fromLatin1(kCtdRelayDockId))) {
		/* no CTD plugin (e.g. safe mode): OBS's own first run */
		on_autoConfigure_triggered();
		return;
	}

	QMessageBox box(this);
	box.setWindowTitle(QTStr("CTD.Welcome.Title"));
	box.setIcon(QMessageBox::NoIcon);
	box.setTextFormat(Qt::PlainText);
	box.setText(QTStr("CTD.Welcome.Heading"));
	box.setInformativeText(QTStr("CTD.Welcome.Text"));
	QPushButton *pair = box.addButton(QTStr("CTD.Welcome.Pair"), QMessageBox::AcceptRole);
	QPushButton *wizard = box.addButton(QTStr("CTD.Welcome.Wizard"), QMessageBox::ActionRole);
	box.addButton(QTStr("CTD.Welcome.Later"), QMessageBox::RejectRole);
	box.setDefaultButton(pair);
	box.exec();

	if (box.clickedButton() == pair) {
		CtdShowRelayPairing();
	} else if (box.clickedButton() == wizard) {
		on_autoConfigure_triggered();
	}
}

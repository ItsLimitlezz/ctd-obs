/******************************************************************************
    CTD OBS: the splash screen, from the launch update check until the main
    window is up (frontend/ctd/ctd-startup.hpp).

    CTD Night look: #04070c background, cyan #39e7ff to magenta #ff00b8
    accents, #f2fbff text, #8ba3af secondary, Space Mono (bundled, registered
    in obs-main.cpp). The CTD OBS icon, the "CTD OBS" wordmark, the version
    (and test build number), a thin progress bar and one status line that
    follows startup, plus the OBS Studio credit (GPL).

    It only paints: no network or disk work. Startup runs on this (the UI)
    thread, so each stage repaints it and lets Qt flush it to the screen.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-startup.hpp"
#include "ctd-version.hpp"

#include <OBSApp.hpp>

#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QWidget>

namespace {

const QColor kBackground(0x04, 0x07, 0x0c);
const QColor kCyan(0x39, 0xe7, 0xff);
const QColor kMagenta(0xff, 0x00, 0xb8);
const QColor kText(0xf2, 0xfb, 0xff);
const QColor kSecondary(0x8b, 0xa3, 0xaf);

const int kWidth = 640;
const int kHeight = 400;
const int kRadius = 16;
const int kMargin = 56;

class CtdSplash : public QWidget {
public:
	CtdSplash()
		/* not "stay on top": a dialog OBS shows while starting (an error, a prompt) must come out above it */
		: QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint),
		  icon(QStringLiteral(":/res/images/obs.png"))
	{
		setObjectName(QStringLiteral("ctdSplash"));
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_DeleteOnClose, false);
		setFixedSize(kWidth, kHeight);
		setWindowTitle(QStringLiteral("CTD OBS"));

		if (QScreen *screen = QGuiApplication::primaryScreen()) {
			QRect area = screen->availableGeometry();
			move(area.center() - rect().center());
		}

		if (CtdIsTestBuild()) {
			version = QTStr("CTD.Splash.TestBuild")
					  .arg(QString::number(CTD_OBS_TEST_NUMBER),
					       QString::fromStdString(CtdReleaseVersionString()));
		} else {
			version = QTStr("CTD.Splash.Version").arg(QString::fromStdString(CtdReleaseVersionString()));
		}
		credit = QTStr("CTD.Splash.BasedOn").arg(QString::fromLatin1(CtdBaseObsVersion()));
	}

	void SetStage(CtdSplashStage stage)
	{
		switch (stage) {
		case CtdSplashStage::Updates:
			status = QTStr("CTD.Splash.Updates");
			progress = 0.12;
			break;
		case CtdSplashStage::Plugins:
			status = QTStr("CTD.Splash.Plugins");
			progress = 0.35;
			break;
		case CtdSplashStage::Scenes:
			status = QTStr("CTD.Splash.Scenes");
			progress = 0.7;
			break;
		case CtdSplashStage::Starting:
			status = QTStr("CTD.Splash.Starting");
			progress = 0.92;
			break;
		}
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

		const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		QPainterPath shape;
		shape.addRoundedRect(frame, kRadius, kRadius);

		p.fillPath(shape, kBackground);
		p.save();
		p.setClipPath(shape);
		/* faint cyan and magenta light from two corners */
		QRadialGradient cyanGlow(QPointF(0, 0), kWidth * 0.75);
		QColor c = kCyan;
		c.setAlphaF(0.10f);
		cyanGlow.setColorAt(0, c);
		c.setAlphaF(0.0f);
		cyanGlow.setColorAt(1, c);
		p.fillRect(rect(), cyanGlow);
		QRadialGradient magentaGlow(QPointF(kWidth, kHeight), kWidth * 0.7);
		QColor m = kMagenta;
		m.setAlphaF(0.09f);
		magentaGlow.setColorAt(0, m);
		m.setAlphaF(0.0f);
		magentaGlow.setColorAt(1, m);
		p.fillRect(rect(), magentaGlow);
		p.restore();

		QLinearGradient accent(0, 0, kWidth, 0);
		accent.setColorAt(0, kCyan);
		accent.setColorAt(1, kMagenta);

		/* hairline border in the accent gradient */
		QColor borderCyan = kCyan;
		QColor borderMagenta = kMagenta;
		borderCyan.setAlphaF(0.45f);
		borderMagenta.setAlphaF(0.45f);
		QLinearGradient border(0, 0, kWidth, kHeight);
		border.setColorAt(0, borderCyan);
		border.setColorAt(1, borderMagenta);
		p.setPen(QPen(QBrush(border), 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(shape);

		/* icon (256 px master, drawn at 112 logical px: sharp at 2x) */
		const int iconSize = 112;
		const QRect iconRect(kMargin, 96, iconSize, iconSize);
		if (!icon.isNull()) {
			p.drawPixmap(iconRect, icon);
		}

		/* wordmark and version */
		const int textLeft = kMargin + iconSize + 32;
		QFont wordmark(QStringLiteral("Space Mono"));
		wordmark.setBold(true);
		wordmark.setPixelSize(44);
		p.setFont(wordmark);
		p.setPen(kText);
		p.drawText(QRect(textLeft, 104, kWidth - textLeft - kMargin, 56), Qt::AlignLeft | Qt::AlignVCenter,
			   QStringLiteral("CTD OBS"));

		QFont small(QStringLiteral("Space Mono"));
		small.setPixelSize(14);
		p.setFont(small);
		p.setPen(kSecondary);
		p.drawText(QRect(textLeft, 164, kWidth - textLeft - kMargin, 24), Qt::AlignLeft | Qt::AlignVCenter,
			   version);

		/* status line and progress bar */
		QFont statusFont(QStringLiteral("Space Mono"));
		statusFont.setPixelSize(13);
		p.setFont(statusFont);
		p.setPen(kText);
		p.drawText(QRect(kMargin, 290, kWidth - 2 * kMargin, 22), Qt::AlignLeft | Qt::AlignVCenter, status);

		const QRectF track(kMargin, 320, kWidth - 2 * kMargin, 3);
		QColor trackColor = kSecondary;
		trackColor.setAlphaF(0.2f);
		p.setPen(Qt::NoPen);
		p.setBrush(trackColor);
		p.drawRoundedRect(track, 1.5, 1.5);
		QRectF fill = track;
		fill.setWidth(track.width() * progress);
		QLinearGradient bar(track.left(), 0, track.right(), 0);
		bar.setColorAt(0, kCyan);
		bar.setColorAt(1, kMagenta);
		p.setBrush(bar);
		p.drawRoundedRect(fill, 1.5, 1.5);

		/* credit (GPL) */
		QFont creditFont(QStringLiteral("Space Mono"));
		creditFont.setPixelSize(11);
		p.setFont(creditFont);
		p.setPen(kSecondary);
		p.drawText(QRect(kMargin, kHeight - 52, kWidth - 2 * kMargin, 20), Qt::AlignLeft | Qt::AlignVCenter,
			   credit);
	}

private:
	QPixmap icon;
	QString version;
	QString credit;
	QString status;
	double progress = 0.0;
};

QPointer<CtdSplash> splash;
bool finished = false;

/* Paint now: startup is busy on this thread, so the event loop isn't turning yet */
void Flush()
{
	if (!splash || !splash->isVisible()) {
		return;
	}
	splash->repaint();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

} // namespace

void CtdSplashShow()
{
	if (splash || finished) {
		return;
	}
	splash = new CtdSplash();
	splash->SetStage(CtdSplashStage::Updates);
	splash->show();
	splash->raise();
	Flush();
}

void CtdSplashSetStage(CtdSplashStage stage)
{
	if (!splash) {
		return;
	}
	splash->SetStage(stage);
	Flush();
}

void CtdSplashSuspend()
{
	if (splash) {
		splash->hide();
	}
}

void CtdSplashResume()
{
	if (splash && !finished) {
		splash->show();
		splash->raise();
		Flush();
	}
}

void CtdSplashFinish()
{
	finished = true;
	if (splash) {
		splash->hide();
		splash->deleteLater();
		splash = nullptr;
	}
}

#include "YouTubeHeartbeat.hpp"

#include <QColor>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QPushButton>
#include <QVariantAnimation>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
const QColor kGlowColor(0x3d, 0xdc, 0x84);
constexpr int kBeatMs = 1200;
/* Strength of the glow with Windows animations off */
constexpr double kSteadyStrength = 0.6;

bool AnimationsEnabled()
{
#ifdef _WIN32
	BOOL enabled = TRUE;
	if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
		return enabled != FALSE;
	}
#endif
	return true;
}
} // namespace

YouTubeHeartbeat::YouTubeHeartbeat(QPushButton *button_) : QObject(button_), button(button_)
{
	glow = new QGraphicsDropShadowEffect(button);
	glow->setOffset(0, 0);
	glow->setEnabled(false);
	SetStrength(0.0);
	button->setGraphicsEffect(glow);

	/* Lub-dub, then rest */
	beat = new QVariantAnimation(this);
	beat->setDuration(kBeatMs);
	beat->setLoopCount(-1);
	beat->setKeyValueAt(0.00, 0.0);
	beat->setKeyValueAt(0.08, 1.0);
	beat->setKeyValueAt(0.18, 0.25);
	beat->setKeyValueAt(0.28, 0.7);
	beat->setKeyValueAt(0.45, 0.0);
	beat->setKeyValueAt(1.00, 0.0);
	connect(beat, &QVariantAnimation::valueChanged, this,
		[this](const QVariant &value) { SetStrength(value.toDouble()); });

	button->installEventFilter(this);
	Update();
}

void YouTubeHeartbeat::SetWanted(bool wanted_)
{
	wanted = wanted_;
	Update();
}

void YouTubeHeartbeat::SetQuietWhileVisible(QWidget *other)
{
	if (quietWhile == other) {
		return;
	}
	if (quietWhile) {
		quietWhile->removeEventFilter(this);
	}
	quietWhile = other;
	if (other) {
		other->installEventFilter(this);
	}
	Update();
}

bool YouTubeHeartbeat::eventFilter(QObject *obj, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Show:
	case QEvent::Hide:
	case QEvent::EnabledChange:
		Update();
		break;
	default:
		break;
	}
	return QObject::eventFilter(obj, event);
}

void YouTubeHeartbeat::Update()
{
	const bool beating = wanted && button->isVisible() && button->isEnabled() &&
			     !(quietWhile && quietWhile->isVisible());
	if (!beating) {
		beat->stop();
		glow->setEnabled(false);
		return;
	}

	glow->setEnabled(true);
	if (!AnimationsEnabled()) {
		beat->stop();
		SetStrength(kSteadyStrength);
	} else if (beat->state() != QAbstractAnimation::Running) {
		beat->start();
	}
}

void YouTubeHeartbeat::SetStrength(double v)
{
	QColor color = kGlowColor;
	color.setAlphaF(v);
	glow->setColor(color);
	glow->setBlurRadius(4.0 + 18.0 * v);
}

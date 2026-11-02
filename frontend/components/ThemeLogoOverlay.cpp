#include "ThemeLogoOverlay.hpp"

#include <OBSApp.hpp>
#include <utility/OBSTheme.hpp>

#include <QEvent>
#include <QPainter>
#include <QTimer>

#include "moc_ThemeLogoOverlay.cpp"

namespace {
const char *const kLogoTheme = "com.obsproject.Themes.CliffgoatGold";
/* On top of the logo's own 65% alpha: faint enough to read through */
constexpr qreal kOverlayOpacity = 0.25;
} // namespace

ThemeLogoOverlay::ThemeLogoOverlay(QWidget *window) : QWidget(window)
{
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_NoSystemBackground);
	setFocusPolicy(Qt::NoFocus);
	setGeometry(window->rect());

	window->installEventFilter(this);
	connect(App(), &OBSApp::StyleChanged, this, &ThemeLogoOverlay::Refresh);
	Refresh();
}

void ThemeLogoOverlay::Refresh()
{
	const OBSTheme *theme = App()->GetTheme();
	bool wanted = theme && (theme->id.startsWith(kLogoTheme) || theme->dependencies.contains(kLogoTheme));

	logo = QPixmap();
	if (wanted && logo.load(QStringLiteral("theme:Cliffgoat/logo.png"))) {
		/* Drawn at its own size, as the stylesheet draws the background */
		qreal dpr = devicePixelRatioF();
		logo = logo.scaled(logo.size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		logo.setDevicePixelRatio(dpr);
	}

	setVisible(!logo.isNull());
	if (isVisible()) {
		raise();
	}
	update();
}

bool ThemeLogoOverlay::eventFilter(QObject *obj, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Resize:
		setGeometry(parentWidget()->rect());
		break;
	case QEvent::ChildAdded:
		/* Stay above docks added later */
		if (isVisible()) {
			QTimer::singleShot(0, this, [this]() { raise(); });
		}
		break;
	case QEvent::DevicePixelRatioChange:
		Refresh();
		break;
	default:
		break;
	}
	return QWidget::eventFilter(obj, event);
}

void ThemeLogoOverlay::paintEvent(QPaintEvent *)
{
	if (logo.isNull()) {
		return;
	}
	QSize size = logo.deviceIndependentSize().toSize();
	QPoint topLeft((width() - size.width()) / 2, (height() - size.height()) / 2);

	QPainter painter(this);
	painter.setOpacity(kOverlayOpacity);
	painter.drawPixmap(topLeft, logo);
}

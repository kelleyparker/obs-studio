#include "YouTubeTeleprompter.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <util/config-file.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace {
constexpr int kMarginPx = 48;
constexpr int kCountdownMs = 3000;
constexpr double kReadingLine = 1.0 / 3.0; /* the reading line, from the top of the window */
const QRegularExpression kCueRe(QStringLiteral("^\\[show:.*\\]$"), QRegularExpression::CaseInsensitiveOption);

config_t *UserConfig()
{
	return App() ? App()->GetUserConfig() : nullptr;
}

int ConfigInt(const char *name, int fallback)
{
	config_t *cfg = UserConfig();
	return cfg && config_has_user_value(cfg, "YouTubeWorkflow", name)
		       ? (int)config_get_int(cfg, "YouTubeWorkflow", name)
		       : fallback;
}

bool ConfigBool(const char *name, bool fallback)
{
	config_t *cfg = UserConfig();
	return cfg && config_has_user_value(cfg, "YouTubeWorkflow", name)
		       ? config_get_bool(cfg, "YouTubeWorkflow", name)
		       : fallback;
}
} // namespace

/* Draws the script, scrolled and optionally mirrored, with the reading line and the countdown */
class TeleprompterCanvas : public QWidget {
public:
	explicit TeleprompterCanvas(YouTubeTeleprompter *owner_) : QWidget(owner_), owner(owner_)
	{
		setAttribute(Qt::WA_OpaquePaintEvent);
		setFocusPolicy(Qt::NoFocus);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.fillRect(rect(), Qt::black);
		if (owner->mirrorCheck->isChecked()) {
			p.translate(width(), 0);
			p.scale(-1, 1);
		}
		const double lineY = owner->ReadingLineY();
		p.save();
		p.translate(kMarginPx, lineY - owner->scrollY);
		owner->doc->drawContents(&p, QRectF(0, owner->scrollY - lineY, owner->doc->textWidth(), height()));
		p.restore();

		/* Reading line: two small arrows at the edges, so they don't cover the words */
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x3d, 0xdc, 0x84));
		const int a = 12;
		p.drawPolygon(QPolygonF({QPointF(4, lineY - a), QPointF(4 + a, lineY), QPointF(4, lineY + a)}));
		p.drawPolygon(QPolygonF({QPointF(width() - 4, lineY - a), QPointF(width() - 4 - a, lineY),
					 QPointF(width() - 4, lineY + a)}));

		if (owner->countdownMs > 0) {
			p.resetTransform();
			p.fillRect(rect(), QColor(0, 0, 0, 160));
			QFont f = font();
			f.setPixelSize(std::max(48, height() / 4));
			f.setBold(true);
			p.setFont(f);
			p.setPen(QColor(0x3d, 0xdc, 0x84));
			p.drawText(rect(), Qt::AlignCenter, QString::number((owner->countdownMs + 999) / 1000));
		}
	}

	void wheelEvent(QWheelEvent *event) override
	{
		owner->Nudge(-event->angleDelta().y() / 120.0);
		event->accept();
	}

	void mousePressEvent(QMouseEvent *) override { owner->TogglePlay(); }

private:
	YouTubeTeleprompter *owner;
};

YouTubeTeleprompter *YouTubeTeleprompter::Get()
{
	static QPointer<YouTubeTeleprompter> instance;
	if (!instance) {
		/* Owned by the main window so it closes with OBS; Qt::Window keeps it a window of its own */
		instance = new YouTubeTeleprompter(OBSBasic::Get());
	}
	return instance;
}

YouTubeTeleprompter::YouTubeTeleprompter(QWidget *parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle("Teleprompter");
	setAttribute(Qt::WA_DeleteOnClose, false);
	setFocusPolicy(Qt::StrongFocus);
	resize(900, 600);
	setStyleSheet("YouTubeTeleprompter { background: black; }");

	doc = new QTextDocument(this);
	doc->setDocumentMargin(0);

	canvas = new TeleprompterCanvas(this);

	toolbar = new QWidget(this);
	QHBoxLayout *bar = new QHBoxLayout(toolbar);
	bar->setContentsMargins(8, 4, 8, 4);
	QPushButton *restartBtn = new QPushButton("↺ Start", toolbar);
	restartBtn->setToolTip("Back to the start (Home)");
	playBtn = new QPushButton(toolbar);
	playBtn->setToolTip("Play / pause (Space, or the \"YouTube: Teleprompter play/pause\" hotkey)");
	wpmSpin = new QSpinBox(toolbar);
	wpmSpin->setRange(60, 300);
	wpmSpin->setSingleStep(5);
	wpmSpin->setSuffix(" words/min");
	wpmSpin->setToolTip("Scrolling speed (+ / −)");
	fontSpin = new QSpinBox(toolbar);
	fontSpin->setRange(18, 140);
	fontSpin->setSuffix(" px");
	fontSpin->setToolTip("Text size (Ctrl + / Ctrl −)");
	mirrorCheck = new QCheckBox("Mirror", toolbar);
	mirrorCheck->setToolTip("Flip left to right, for teleprompter glass");
	onTopCheck = new QCheckBox("On top", toolbar);
	hideCaptureCheck = new QCheckBox("Hide from screen recordings", toolbar);
	hideCaptureCheck->setToolTip("Keeps this window out of Display Capture, Window Capture and screenshots "
				     "(Windows 10 2004 or later)");
	autoStartCheck = new QCheckBox("Start when I record", toolbar);
	autoStartCheck->setToolTip("Starts scrolling, after a 3-second countdown, when a recording starts");
	statusLabel = new QLabel(toolbar);
	bar->addWidget(restartBtn);
	bar->addWidget(playBtn);
	bar->addWidget(wpmSpin);
	bar->addWidget(fontSpin);
	bar->addWidget(mirrorCheck);
	bar->addWidget(onTopCheck);
	bar->addWidget(hideCaptureCheck);
	bar->addWidget(autoStartCheck);
	bar->addStretch();
	bar->addWidget(statusLabel);
	for (QWidget *w : toolbar->findChildren<QWidget *>()) {
		w->setFocusPolicy(Qt::NoFocus); /* keys belong to the teleprompter */
	}

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(canvas, 1);
	layout->addWidget(toolbar);

	/* Settings, kept between sessions */
	wpmSpin->setValue(ConfigInt("TeleprompterWpm", 150));
	fontSpin->setValue(ConfigInt("TeleprompterFontPx", 44));
	mirrorCheck->setChecked(ConfigBool("TeleprompterMirror", false));
	onTopCheck->setChecked(ConfigBool("TeleprompterOnTop", true));
	hideCaptureCheck->setChecked(ConfigBool("TeleprompterHideCapture", true));
	autoStartCheck->setChecked(ConfigBool("TeleprompterAutoStart", true));
	if (config_t *cfg = UserConfig()) {
		const char *geometry = config_get_string(cfg, "YouTubeWorkflow", "TeleprompterGeometry");
		if (geometry && *geometry) {
			restoreGeometry(QByteArray::fromBase64(geometry));
		}
	}
	ApplyOnTop();

	timer = new QTimer(this);
	timer->setTimerType(Qt::PreciseTimer);
	timer->setInterval(16);
	connect(timer, &QTimer::timeout, this, &YouTubeTeleprompter::Tick);

	connect(restartBtn, &QPushButton::clicked, this, &YouTubeTeleprompter::Restart);
	connect(playBtn, &QPushButton::clicked, this, &YouTubeTeleprompter::TogglePlay);
	connect(wpmSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { SaveSettings(); });
	connect(fontSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() {
		BuildDocument();
		SaveSettings();
	});
	connect(mirrorCheck, &QCheckBox::toggled, this, [this]() {
		canvas->update();
		SaveSettings();
	});
	connect(onTopCheck, &QCheckBox::toggled, this, [this]() {
		ApplyOnTop();
		SaveSettings();
	});
	connect(hideCaptureCheck, &QCheckBox::toggled, this, [this]() {
		ApplyCaptureExclusion();
		SaveSettings();
	});
	connect(autoStartCheck, &QCheckBox::toggled, this, [this]() { SaveSettings(); });

	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(mgr, &YouTubeWorkflowManager::RecordingStarted, this, [this]() {
		if (!autoStartCheck->isChecked() || !isVisible() || spokenWords == 0 || playing) {
			return;
		}
		Restart();
		countdownMs = kCountdownMs;
		Play();
	});
	connect(mgr, &YouTubeWorkflowManager::TeleprompterToggleRequested, this, [this]() {
		if (isVisible()) {
			TogglePlay();
		}
	});

	UpdatePlayButton();
	if (config_t *cfg = UserConfig()) {
		const char *last = config_get_string(cfg, "YouTubeWorkflow", "TeleprompterScript");
		if (last && *last && QFileInfo::exists(QString::fromUtf8(last))) {
			LoadFile(QString::fromUtf8(last));
		}
	}
}

bool YouTubeTeleprompter::LoadFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	LoadText(QString::fromUtf8(file.readAll()), path);
	return true;
}

void YouTubeTeleprompter::LoadText(const QString &text, const QString &path)
{
	Pause();
	markdown = text;
	scriptPath = path;
	if (!path.isEmpty()) {
		if (config_t *cfg = UserConfig()) {
			config_set_string(cfg, "YouTubeWorkflow", "TeleprompterScript", path.toUtf8().constData());
		}
		setWindowTitle(QString("Teleprompter — %1").arg(QFileInfo(path).completeBaseName()));
	}
	BuildDocument();
	Restart();
}

void YouTubeTeleprompter::BuildDocument()
{
	doc->clear();
	doc->setTextWidth(std::max(100, canvas->width() - 2 * kMarginPx));
	QFont base = font();
	base.setPixelSize(fontSpin->value());
	doc->setDefaultFont(base);

	QTextCursor cursor(doc);
	QTextBlockFormat block;
	block.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
	block.setBottomMargin(fontSpin->value() * 0.8);
	QTextCharFormat spoken;
	spoken.setForeground(QColor(0xf2, 0xf2, 0xf2));
	QTextCharFormat dim;
	dim.setForeground(QColor(0x80, 0x80, 0x88));

	spokenWords = 0;
	bool first = true;
	QString paragraph;
	auto flush = [&](const QString &text, const QTextCharFormat &fmt, double scale, bool italic = false) {
		if (text.trimmed().isEmpty()) {
			return;
		}
		if (!first) {
			cursor.insertBlock(block);
		} else {
			cursor.setBlockFormat(block);
			first = false;
		}
		QTextCharFormat f = fmt;
		QFont font = base;
		font.setPixelSize(std::max(12, (int)std::lround(fontSpin->value() * scale)));
		font.setItalic(italic);
		f.setFont(font);
		cursor.insertText(text.trimmed(), f);
	};
	auto flushParagraph = [&]() {
		flush(paragraph, spoken, 1.0);
		spokenWords += paragraph.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
		paragraph.clear();
	};

	for (const QString &raw : markdown.split('\n')) {
		const QString line = raw.trimmed();
		if (line.isEmpty()) {
			flushParagraph();
		} else if (line.startsWith('#')) {
			flushParagraph();
			QString heading = line;
			heading.remove(QRegularExpression("^#+\\s*"));
			flush(heading.toUpper(), dim, 0.55);
		} else if (kCueRe.match(line).hasMatch()) {
			flushParagraph();
			flush(line, dim, 0.6, true);
		} else {
			paragraph += (paragraph.isEmpty() ? "" : " ") + line;
		}
	}
	flushParagraph();
	if (spokenWords == 0) {
		flush("Open a script from the Script Writer (📝 Script… in the YouTube dock).", dim, 0.6, true);
	}
	statusLabel->setText(spokenWords ? QString("%1 words · about %2 min")
						   .arg(spokenWords)
						   .arg(spokenWords / (double)wpmSpin->value(), 0, 'f', 1)
					 : QString());
	canvas->update();
}

double YouTubeTeleprompter::PixelsPerWord() const
{
	return spokenWords > 0 ? doc->size().height() / spokenWords : 1.0;
}

double YouTubeTeleprompter::ReadingLineY() const
{
	return canvas->height() * kReadingLine;
}

void YouTubeTeleprompter::Play()
{
	if (spokenWords == 0) {
		return;
	}
	playing = true;
	clock.restart();
	timer->start();
	UpdatePlayButton();
}

void YouTubeTeleprompter::Pause()
{
	playing = false;
	countdownMs = 0;
	if (timer) {
		timer->stop();
	}
	UpdatePlayButton();
	if (canvas) {
		canvas->update();
	}
}

void YouTubeTeleprompter::TogglePlay()
{
	if (playing) {
		Pause();
	} else {
		Play();
	}
}

void YouTubeTeleprompter::Restart()
{
	scrollY = 0.0;
	canvas->update();
}

void YouTubeTeleprompter::Tick()
{
	const double dt = clock.restart() / 1000.0;
	if (countdownMs > 0) {
		countdownMs = std::max(0, countdownMs - (int)std::lround(dt * 1000.0));
		canvas->update();
		return;
	}
	scrollY += dt * wpmSpin->value() / 60.0 * PixelsPerWord();
	if (scrollY >= doc->size().height()) {
		scrollY = doc->size().height();
		Pause();
	}
	canvas->update();
}

void YouTubeTeleprompter::Nudge(double lines)
{
	scrollY = std::clamp(scrollY + lines * fontSpin->value() * 1.35, 0.0, doc->size().height());
	canvas->update();
}

void YouTubeTeleprompter::SetWpm(int wpm)
{
	wpmSpin->setValue(wpm);
}

void YouTubeTeleprompter::SetFontSize(int px)
{
	/* Keep the same place in the script when the text reflows */
	const double fraction = doc->size().height() > 0 ? scrollY / doc->size().height() : 0.0;
	fontSpin->setValue(px);
	scrollY = fraction * doc->size().height();
	canvas->update();
}

void YouTubeTeleprompter::keyPressEvent(QKeyEvent *event)
{
	const bool ctrl = event->modifiers() & Qt::ControlModifier;
	switch (event->key()) {
	case Qt::Key_Space:
		TogglePlay();
		break;
	case Qt::Key_Escape:
		Pause();
		break;
	case Qt::Key_Home:
		Restart();
		break;
	case Qt::Key_Up:
		Nudge(-1);
		break;
	case Qt::Key_Down:
		Nudge(1);
		break;
	case Qt::Key_PageUp:
		Nudge(-8);
		break;
	case Qt::Key_PageDown:
		Nudge(8);
		break;
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		ctrl ? SetFontSize(fontSpin->value() + 4) : SetWpm(wpmSpin->value() + 10);
		break;
	case Qt::Key_Minus:
		ctrl ? SetFontSize(fontSpin->value() - 4) : SetWpm(wpmSpin->value() - 10);
		break;
	default:
		QWidget::keyPressEvent(event);
		return;
	}
	event->accept();
}

void YouTubeTeleprompter::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	ApplyCaptureExclusion();
	BuildDocument();
}

void YouTubeTeleprompter::closeEvent(QCloseEvent *event)
{
	Pause();
	SaveSettings();
	event->accept();
}

void YouTubeTeleprompter::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!doc || !canvas || !statusLabel) {
		return;
	}
	const double fraction = doc->size().height() > 0 ? scrollY / doc->size().height() : 0.0;
	BuildDocument();
	scrollY = fraction * doc->size().height();
}

void YouTubeTeleprompter::ApplyCaptureExclusion()
{
#ifdef _WIN32
	if (!testAttribute(Qt::WA_WState_Created)) {
		return;
	}
	HWND hwnd = reinterpret_cast<HWND>(winId());
	SetWindowDisplayAffinity(hwnd, hideCaptureCheck->isChecked() ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
#endif
}

void YouTubeTeleprompter::ApplyOnTop()
{
	const bool wasVisible = isVisible();
	setWindowFlag(Qt::WindowStaysOnTopHint, onTopCheck->isChecked());
	if (wasVisible) {
		show(); /* changing window flags hides the window */
		ApplyCaptureExclusion();
	}
}

void YouTubeTeleprompter::UpdatePlayButton()
{
	playBtn->setText(playing ? "⏸ Pause" : "▶ Play");
}

void YouTubeTeleprompter::SaveSettings()
{
	config_t *cfg = UserConfig();
	if (!cfg) {
		return;
	}
	config_set_int(cfg, "YouTubeWorkflow", "TeleprompterWpm", wpmSpin->value());
	config_set_int(cfg, "YouTubeWorkflow", "TeleprompterFontPx", fontSpin->value());
	config_set_bool(cfg, "YouTubeWorkflow", "TeleprompterMirror", mirrorCheck->isChecked());
	config_set_bool(cfg, "YouTubeWorkflow", "TeleprompterOnTop", onTopCheck->isChecked());
	config_set_bool(cfg, "YouTubeWorkflow", "TeleprompterHideCapture", hideCaptureCheck->isChecked());
	config_set_bool(cfg, "YouTubeWorkflow", "TeleprompterAutoStart", autoStartCheck->isChecked());
	config_set_string(cfg, "YouTubeWorkflow", "TeleprompterGeometry", saveGeometry().toBase64().constData());
	config_save_safe(cfg, "tmp", nullptr);
}

#include "YouTubeVideoPreview.hpp"

#include <OBSApp.hpp>
#include <util/config-file.h>

#include <QAudioOutput>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>

namespace {
QString FormatClock(double seconds)
{
	int s = std::max(0, int(seconds));
	return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
			 : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

/* "MM:SS Title" (or H:MM:SS) chapter lines of a YouTube description */
QList<QPair<double, QString>> ParseChapters(const QString &description)
{
	static const QRegularExpression re(R"(^\s*(\d{1,2}(?::\d{2}){1,2})(?:\s*[-–—]\s*|\s+)(.*)$)");
	QList<QPair<double, QString>> chapters;
	for (const QString &line : description.split('\n')) {
		QRegularExpressionMatch m = re.match(line);
		if (!m.hasMatch()) {
			continue;
		}
		double seconds = 0;
		for (const QString &part : m.captured(1).split(':')) {
			seconds = seconds * 60 + part.toInt();
		}
		chapters.append({seconds, m.captured(2).trimmed()});
	}
	return chapters;
}
} // namespace

/* ------------------------------------------------------------------------ */
/* EditScrubBar                                                             */

EditScrubBar::EditScrubBar(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(34);
	setCursor(Qt::PointingHandCursor);
}

void EditScrubBar::SetDuration(double seconds)
{
	duration = seconds;
	update();
}

void EditScrubBar::SetPosition(double seconds)
{
	position = seconds;
	update();
}

void EditScrubBar::SetMarks(const QList<double> &joins_, const QList<double> &chapters_)
{
	joins = joins_;
	chapters = chapters_;
	update();
}

double EditScrubBar::TimeAt(int x) const
{
	int w = std::max(1, width() - 12);
	return std::clamp(double(x - 6) / w, 0.0, 1.0) * duration;
}

int EditScrubBar::XAt(double seconds) const
{
	return 6 + int((duration > 0 ? seconds / duration : 0.0) * (width() - 12));
}

void EditScrubBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QRect bar(6, height() / 2 - 5, width() - 12, 10);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(58, 120, 72));
	p.drawRoundedRect(bar, 4, 4);
	if (duration > 0) {
		QRect played = bar;
		played.setRight(XAt(position));
		p.setBrush(QColor(90, 180, 110));
		p.drawRoundedRect(played, 4, 4);
	}
	for (double c : chapters) {
		p.setPen(QPen(QColor(90, 160, 255), 2));
		p.drawLine(XAt(c), 2, XAt(c), height() / 2 - 6);
	}
	for (double j : joins) {
		p.setPen(QPen(QColor(235, 140, 40), 2));
		p.drawLine(XAt(j), height() / 2 - 8, XAt(j), height() / 2 + 8);
	}
	p.setPen(QPen(Qt::white, 2));
	p.drawLine(XAt(position), 2, XAt(position), height() - 2);
}

void EditScrubBar::mousePressEvent(QMouseEvent *event)
{
	emit SeekRequested(TimeAt(event->position().toPoint().x()));
}

void EditScrubBar::mouseMoveEvent(QMouseEvent *event)
{
	if (event->buttons() & Qt::LeftButton) {
		emit SeekRequested(TimeAt(event->position().toPoint().x()));
	}
}

/* ------------------------------------------------------------------------ */
/* YouTubeVideoPreview                                                      */

YouTubeVideoPreview::YouTubeVideoPreview(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("Watch the edit");
	setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	resize(1100, 720);

	player = new QMediaPlayer(this);
	audio = new QAudioOutput(this);
	player->setAudioOutput(audio);
	video = new QVideoWidget(this);
	video->setMinimumSize(480, 270);
	player->setVideoOutput(video);

	scrub = new EditScrubBar(this);
	playButton = new QPushButton("▶ Play", this);
	QPushButton *backButton = new QPushButton("⏪ 5s", this);
	QPushButton *forwardButton = new QPushButton("5s ⏩", this);
	QPushButton *prevJoinButton = new QPushButton("⏮ Prev cut", this);
	QPushButton *nextJoinButton = new QPushButton("Next cut ⏭", this);
	prevJoinButton->setToolTip("Jump to 2 s before the previous cut, to hear how it sounds ([)");
	nextJoinButton->setToolTip("Jump to 2 s before the next cut, to hear how it sounds (])");
	chapterCombo = new QComboBox(this);
	chapterCombo->setToolTip("Jump to a chapter");
	timeLabel = new QLabel(this);
	QPushButton *externalButton = new QPushButton("Open in default player", this);

	QHBoxLayout *transport = new QHBoxLayout();
	for (QWidget *w : {(QWidget *)prevJoinButton, (QWidget *)backButton, (QWidget *)playButton,
			   (QWidget *)forwardButton, (QWidget *)nextJoinButton}) {
		transport->addWidget(w);
	}
	transport->addSpacing(12);
	transport->addWidget(chapterCombo, 1);
	transport->addSpacing(12);
	transport->addWidget(timeLabel);
	transport->addWidget(externalButton);

	QLabel *legend = new QLabel("<span style='color:#eb8c28'>|</span> a cut &nbsp; "
				    "<span style='color:#5aa0ff'>|</span> a chapter &nbsp; "
				    "Space play/pause, ←/→ 5s, [ / ] previous/next cut.",
				    this);
	legend->setStyleSheet("color: #b8a8c8; font-size: 11px;");

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(video, 1);
	layout->addWidget(scrub);
	layout->addLayout(transport);
	layout->addWidget(legend);

	const char *geometry = config_get_string(App()->GetUserConfig(), "YouTubeWorkflow", "PreviewGeometry");
	if (geometry && *geometry) {
		restoreGeometry(QByteArray::fromBase64(geometry));
	}

	connect(playButton, &QPushButton::clicked, this, [this]() {
		if (player->playbackState() == QMediaPlayer::PlayingState) {
			player->pause();
		} else {
			player->play();
		}
	});
	connect(player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
		playButton->setText(state == QMediaPlayer::PlayingState ? "⏸ Pause" : "▶ Play");
	});
	connect(player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
		duration = ms / 1000.0;
		scrub->SetDuration(duration);
	});
	connect(player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
		double t = ms / 1000.0;
		scrub->SetPosition(t);
		timeLabel->setText(QString("%1 / %2").arg(FormatClock(t), FormatClock(duration)));
	});
	connect(player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
		timeLabel->setText(QString("Video error: %1").arg(message));
	});
	connect(scrub, &EditScrubBar::SeekRequested, this, &YouTubeVideoPreview::Seek);
	connect(backButton, &QPushButton::clicked, this, [this]() { Seek(Position() - 5.0); });
	connect(forwardButton, &QPushButton::clicked, this, [this]() { Seek(Position() + 5.0); });
	connect(prevJoinButton, &QPushButton::clicked, this, [this]() { StepJoin(-1); });
	connect(nextJoinButton, &QPushButton::clicked, this, [this]() { StepJoin(1); });
	connect(chapterCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
		QVariant t = chapterCombo->itemData(index);
		if (t.isValid()) {
			Seek(t.toDouble());
		}
	});
	connect(externalButton, &QPushButton::clicked, this, [this]() {
		player->pause();
		QDesktopServices::openUrl(QUrl::fromLocalFile(videoPath));
	});

	auto shortcut = [this](const QKeySequence &key, auto slot) {
		QShortcut *s = new QShortcut(key, this);
		connect(s, &QShortcut::activated, this, slot);
	};
	shortcut(Qt::Key_Space, [this]() { playButton->click(); });
	shortcut(Qt::Key_Left, [this]() { Seek(Position() - 5.0); });
	shortcut(Qt::Key_Right, [this]() { Seek(Position() + 5.0); });
	shortcut(Qt::Key_BracketLeft, [this]() { StepJoin(-1); });
	shortcut(Qt::Key_BracketRight, [this]() { StepJoin(1); });
}

YouTubeVideoPreview::~YouTubeVideoPreview()
{
	SaveGeometry();
}

void YouTubeVideoPreview::SaveGeometry()
{
	config_set_string(App()->GetUserConfig(), "YouTubeWorkflow", "PreviewGeometry",
			  saveGeometry().toBase64().constData());
}

void YouTubeVideoPreview::closeEvent(QCloseEvent *event)
{
	Unload();
	SaveGeometry();
	QDialog::closeEvent(event);
}

void YouTubeVideoPreview::Load(const QString &videoPath_, const QList<double> &joins_, const QString &description)
{
	bool same = videoPath_ == videoPath && player->source().isValid();
	videoPath = videoPath_;
	joins = joins_;
	std::sort(joins.begin(), joins.end());
	setWindowTitle(QString("Watch the edit — %1").arg(QFileInfo(videoPath).fileName()));

	QList<double> chapterTimes;
	chapterCombo->clear();
	chapterCombo->addItem("Chapters…");
	for (const auto &chapter : ParseChapters(description)) {
		chapterTimes.append(chapter.first);
		chapterCombo->addItem(QString("%1  %2").arg(FormatClock(chapter.first), chapter.second), chapter.first);
	}
	chapterCombo->setEnabled(!chapterTimes.isEmpty());
	scrub->SetMarks(joins, chapterTimes);

	if (!same) {
		player->setSource(QUrl::fromLocalFile(videoPath));
	}
}

void YouTubeVideoPreview::Unload()
{
	player->stop();
	player->setSource(QUrl());
}

double YouTubeVideoPreview::Position() const
{
	return player->position() / 1000.0;
}

void YouTubeVideoPreview::Seek(double seconds)
{
	player->setPosition(qint64(std::clamp(seconds, 0.0, std::max(0.0, duration)) * 1000.0));
}

void YouTubeVideoPreview::StepJoin(int direction)
{
	/* Land 2 s before a cut, so playing on lets you hear it */
	const double lead = 2.0;
	double here = Position() + lead;
	if (direction > 0) {
		for (double j : joins) {
			if (j > here + 0.25) {
				Seek(j - lead);
				return;
			}
		}
	} else {
		for (auto it = joins.crbegin(); it != joins.crend(); ++it) {
			if (*it < here - 0.25) {
				Seek(*it - lead);
				return;
			}
		}
	}
}

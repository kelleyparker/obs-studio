#include "YouTubeMusicPicker.hpp"
#include "YouTubeHeartbeat.hpp"
#include "YouTubeSettingsWidget.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QAbstractButton>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QAudioSink>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace {
const char *kPlaySong = "▶ Song";
const char *kPlayMix = "▶ With my voice";

/* The sample format of a 16-bit PCM WAV and where its samples start; false for anything else */
bool ReadWavHeader(QFile &file, QAudioFormat &format, qint64 &dataOffset)
{
	QByteArray riff = file.read(12);
	if (riff.size() < 12 || !riff.startsWith("RIFF") || riff.mid(8, 4) != "WAVE") {
		return false;
	}
	bool haveFormat = false;
	for (;;) {
		QByteArray header = file.read(8);
		if (header.size() < 8) {
			return false;
		}
		QByteArray id = header.left(4);
		quint32 size = qFromLittleEndian<quint32>(header.constData() + 4);
		if (id == "data") {
			dataOffset = file.pos();
			return haveFormat;
		}
		if (id == "fmt ") {
			QByteArray fmt = file.read(size);
			if (fmt.size() < 16) {
				return false;
			}
			quint16 tag = qFromLittleEndian<quint16>(fmt.constData());
			quint16 channels = qFromLittleEndian<quint16>(fmt.constData() + 2);
			quint32 rate = qFromLittleEndian<quint32>(fmt.constData() + 4);
			quint16 bits = qFromLittleEndian<quint16>(fmt.constData() + 14);
			/* 1 = PCM, 0xFFFE = WAVE_FORMAT_EXTENSIBLE */
			if ((tag != 1 && tag != 0xFFFE) || bits != 16 || !channels || !rate) {
				return false;
			}
			format.setSampleRate(int(rate));
			format.setChannelCount(channels);
			format.setSampleFormat(QAudioFormat::Int16);
			haveFormat = true;
		} else if (!file.seek(file.pos() + size)) {
			return false;
		}
		/* Chunks are padded to an even length */
		if ((size & 1) && !file.seek(file.pos() + 1)) {
			return false;
		}
	}
}

QJsonObject ReadJson(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

QString Clock(double seconds)
{
	int s = int(seconds + 0.5);
	return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

/* A 16-bit WAV's length in milliseconds, or 0 if it can't be read */
qint64 WavLengthMs(const QString &path)
{
	QFile file(path);
	QAudioFormat format;
	qint64 dataOffset = 0;
	if (!file.open(QIODevice::ReadOnly) || !ReadWavHeader(file, format, dataOffset) || !format.bytesPerFrame()) {
		return 0;
	}
	return (file.size() - dataOffset) / format.bytesPerFrame() * 1000 / format.sampleRate();
}

/* A seek bar that jumps straight to where it's clicked (not a page at a time), and lets the mouse wheel
 * scroll the dock instead of moving it */
class SeekSlider : public QSlider {
public:
	using QSlider::QSlider;

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			QStyleOptionSlider opt;
			initStyleOption(&opt);
			QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
			if (!handle.contains(event->position().toPoint())) {
				QRect groove =
					style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
				int x = int(event->position().x()) - groove.x() - handle.width() / 2;
				setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x,
									 groove.width() - handle.width()));
			}
		}
		/* The handle is under the mouse now, so this picks it up for dragging */
		QSlider::mousePressEvent(event);
	}

	void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
} // namespace

YouTubeMusicPicker::YouTubeMusicPicker(QWidget *parent) : QGroupBox("🎵 Pick the Music", parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	infoLabel = new QLabel(this);
	infoLabel->setWordWrap(true);
	infoLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	layout->addWidget(infoLabel);
	notesLabel = new QLabel(this);
	notesLabel->setWordWrap(true);
	notesLabel->setStyleSheet("color: #ffb070; font-size: 11px;");
	notesLabel->hide();
	layout->addWidget(notesLabel);

	listBox = new QWidget(this);
	listLayout = new QVBoxLayout(listBox);
	listLayout->setContentsMargins(0, 0, 0, 0);
	listLayout->setSpacing(4);
	layout->addWidget(listBox);
	group = new QButtonGroup(this);
	group->setExclusive(true);

	/* New songs from an edited style prompt */
	QGroupBox *regenBox = new QGroupBox("🎨 Make new songs", this);
	QVBoxLayout *regenLayout = new QVBoxLayout(regenBox);
	styleEdit = new QPlainTextEdit(regenBox);
	styleEdit->setPlaceholderText("Genre, mood, tempo and instruments, e.g. chill lo-fi hip hop, warm and calm, "
				      "soft Rhodes piano, mellow bass, brushed drums");
	styleEdit->setFixedHeight(54);
	styleEdit->setToolTip("The style prompt for the music. It starts as the selected song's; a tempo like "
			      "\"100 bpm\" in it is used too.");
	regenLayout->addWidget(styleEdit);
	/* Just for these songs: it starts at the Settings choice each time, and Settings (what Auto mode uses)
	 * stays as it is */
	QHBoxLayout *sourceRow = new QHBoxLayout();
	sourceCombo = new QComboBox(regenBox);
	YouTubeSettingsWidget::FillMusicSources(sourceCombo);
	sourceCombo->setToolTip("What makes the new songs. Lyria uses your Gemini key (about $0.08 per 2-minute song, "
				"done in about half a minute); any song it can't make is made on this PC with ACE-Step.\n"
				"This doesn't change the music source in Settings.");
	sourceRow->addWidget(new QLabel("Make with:", regenBox));
	sourceRow->addWidget(sourceCombo, 1);
	regenLayout->addLayout(sourceRow);
	regenButton = new QPushButton(regenBox);
	regenLayout->addWidget(regenButton);
	connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&YouTubeMusicPicker::UpdateEtaLabel);
	layout->addWidget(regenBox);

	busyRow = new QWidget(this);
	QHBoxLayout *busyLayout = new QHBoxLayout(busyRow);
	busyLayout->setContentsMargins(0, 0, 0, 0);
	busyBar = new QProgressBar(busyRow);
	busyBar->setRange(0, 100);
	busyBar->setFixedHeight(16);
	busyLabel = new QLabel(busyRow);
	busyLabel->setStyleSheet("color: #ffcc44;");
	QPushButton *stopButton = new QPushButton("⏹ Stop", busyRow);
	busyLayout->addWidget(busyLabel, 1);
	busyLayout->addWidget(busyBar, 1);
	busyLayout->addWidget(stopButton);
	busyRow->hide();
	layout->addWidget(busyRow);

	useButton = new QPushButton("✅ Use this music", this);
	useButton->setStyleSheet("font-weight: bold;");
	new YouTubeHeartbeat(useButton);
	layout->addWidget(useButton);

	player = new QMediaPlayer(this);
	audio = new QAudioOutput(this);
	player->setAudioOutput(audio);
	connect(player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
		if (status == QMediaPlayer::EndOfMedia) {
			ReachedEnd();
		} else if (status == QMediaPlayer::InvalidMedia) {
			StopPlayback();
		}
	});
	positionTimer = new QTimer(this);
	positionTimer->setInterval(100);
	connect(positionTimer, &QTimer::timeout, this, &YouTubeMusicPicker::ShowPosition);

	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(useButton, &QPushButton::clicked, this, &YouTubeMusicPicker::Finish);
	connect(regenButton, &QPushButton::clicked, this, &YouTubeMusicPicker::Regenerate);
	connect(stopButton, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);
	connect(mgr, &YouTubeWorkflowManager::MusicRegenStarted, this, [this]() { SetBusy(true); });
	connect(mgr, &YouTubeWorkflowManager::MusicRegenFinished, this, [this]() { SetBusy(false); });
	connect(mgr, &YouTubeWorkflowManager::MusicSourceChanged, this, [this](const QString &source) {
		sourceCombo->setCurrentIndex(std::max(0, sourceCombo->findData(source)));
	});
	connect(mgr, &YouTubeWorkflowManager::StepProgressUpdated, this,
		[this](const QString &step, int percent, const QString &message) {
			if (step == "music" && busyRow->isVisible()) {
				busyBar->setValue(percent);
				busyLabel->setText(message);
			}
		});

	hide();
}

void YouTubeMusicPicker::Load(const QString &musicPath_, const QString &statePath_)
{
	musicPath = musicPath_;
	statePath = statePath_;
	StopPlayback();

	for (QAbstractButton *b : group->buttons()) {
		group->removeButton(b);
	}
	while (QLayoutItem *item = listLayout->takeAt(0)) {
		if (item->widget()) {
			item->widget()->deleteLater();
		}
		delete item;
	}

	QJsonObject root = ReadJson(musicPath);
	if (root.isEmpty()) {
		emit YouTubeWorkflowManager::Instance()->LogMessageReceived(
			"error", QString("Could not read the music choices: %1").arg(musicPath));
		return;
	}
	QString chosen = root.value("chosen").toString();
	videoSeconds = root.value("duration").toDouble();
	regenCount = root.value("regen_count").toInt(2);
	QString by = root.value("brief_by").toString();
	QStringList makers;
	for (const QJsonValue &v : root.value("options").toArray()) {
		QJsonObject o = v.toObject();
		QString maker = o.value("made_by").toString("ACE-Step");
		if (!o.value("path").toString().isEmpty() && !makers.contains(maker)) {
			makers.append(maker);
		}
	}
	QString made = makers.isEmpty() || makers == QStringList{"ACE-Step"}
			       ? QString("Made on this PC with ACE-Step")
			       : QString("Made with %1").arg(makers.join(" and "));
	infoLabel->setText(QString("%1%2. Each song is fitted to the video and plays about 20 dB under your voice, "
				   "dipping further while you talk.")
				   .arg(made, by.isEmpty() ? QString() : QString(", styles by %1").arg(by)));
	QStringList notes;
	for (const QJsonValue &v : root.value("notes").toArray()) {
		notes.append("⚠ " + v.toString());
	}
	notesLabel->setText(notes.join("\n"));
	notesLabel->setVisible(!notes.isEmpty());

	QRadioButton *checked = nullptr;
	for (const QJsonValue &v : root.value("options").toArray()) {
		QJsonObject o = v.toObject();
		QString path = o.value("path").toString();
		if (path.isEmpty() || !QFileInfo::exists(path)) {
			continue;
		}
		QWidget *row = new QWidget(listBox);
		QVBoxLayout *rowColumn = new QVBoxLayout(row);
		rowColumn->setContentsMargins(0, 0, 0, 0);
		rowColumn->setSpacing(0);
		QHBoxLayout *rowLayout = new QHBoxLayout();
		rowColumn->addLayout(rowLayout);
		QRadioButton *radio = new QRadioButton(
			QString("%1 · %2").arg(o.value("label").toString(), o.value("name").toString()), row);
		QString style = o.value("style").toString();
		QString details = QString("%1 BPM, %2%3%4")
					  .arg(o.value("bpm").toInt())
					  .arg(o.value("key").toString())
					  .arg(o.value("instrumental").toBool(true) ? ", instrumental" : ", with vocals")
					  .arg(makers.size() > 1 ? " · " + o.value("made_by").toString("ACE-Step")
								 : QString());
		radio->setToolTip(QString("%1\n%2").arg(style, details));
		radio->setProperty("songId", o.value("id").toString());
		radio->setProperty("style", style);
		group->addButton(radio);
		QLabel *detailLabel = new QLabel(details, row);
		detailLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
		QPushButton *songButton = new QPushButton(kPlaySong, row);
		songButton->setProperty("idleText", kPlaySong);
		songButton->setToolTip("Hear the song on its own, at full level");
		QPushButton *mixButton = new QPushButton(kPlayMix, row);
		mixButton->setProperty("idleText", kPlayMix);
		mixButton->setToolTip("Hear it under your voice, as it will be in the video");
		QString preview = o.value("preview").toString();
		mixButton->setEnabled(QFileInfo::exists(preview));
		connect(songButton, &QPushButton::clicked, this, [this, songButton, path]() { Play(songButton, path); });
		connect(mixButton, &QPushButton::clicked, this, [this, mixButton, preview]() { Play(mixButton, preview); });
		connect(radio, &QRadioButton::toggled, this, [this, style](bool on) {
			if (on) {
				styleEdit->setPlainText(style);
			}
		});
		rowLayout->addWidget(radio);
		rowLayout->addWidget(detailLabel, 1);
		rowLayout->addWidget(songButton);
		rowLayout->addWidget(mixButton);

		/* Where the song is: both versions are fitted to the video, so they share one slider */
		QHBoxLayout *seekLayout = new QHBoxLayout();
		seekLayout->setContentsMargins(22, 0, 0, 0);
		SeekSlider *slider = new SeekSlider(Qt::Horizontal, row);
		qint64 lengthMs = WavLengthMs(path);
		slider->setRange(0, int(lengthMs > 0 ? lengthMs : qint64(videoSeconds * 1000.0)));
		slider->setSingleStep(1000);
		slider->setPageStep(10000);
		slider->setFocusPolicy(Qt::NoFocus);
		slider->setToolTip("Click or drag to jump to any point; ▶ starts from here");
		QLabel *timeLabel = new QLabel(row);
		timeLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
		timeLabel->setMinimumWidth(timeLabel->fontMetrics().horizontalAdvance("00:00 / 00:00") + 4);
		timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		auto showTime = [timeLabel, slider](int ms) {
			timeLabel->setText(QString("%1 / %2").arg(Clock(ms / 1000.0), Clock(slider->maximum() / 1000.0)));
		};
		showTime(0);
		connect(slider, &QSlider::valueChanged, timeLabel, showTime);
		connect(slider, &QSlider::sliderReleased, this, [this, slider]() {
			if (slider == playingSlider) {
				SeekTo(slider->value());
			}
		});
		songButton->setProperty("slider", QVariant::fromValue<QObject *>(slider));
		mixButton->setProperty("slider", QVariant::fromValue<QObject *>(slider));
		seekLayout->addWidget(slider, 1);
		seekLayout->addWidget(timeLabel);
		rowColumn->addLayout(seekLayout);
		listLayout->addWidget(row);
		if (o.value("id").toString() == chosen || !checked) {
			checked = radio;
		}
	}

	QRadioButton *none = new QRadioButton("No music", listBox);
	none->setProperty("songId", "none");
	group->addButton(none);
	listLayout->addWidget(none);
	if (chosen == "none" || !checked) {
		checked = none;
	}
	checked->setChecked(true);
	if (checked == none) {
		styleEdit->clear();
	}

	/* Each review starts at the Settings choice */
	{
		QSignalBlocker block(sourceCombo);
		QString source = YouTubeWorkflowManager::Instance()->GetConfig().musicSource;
		sourceCombo->setCurrentIndex(std::max(0, sourceCombo->findData(source)));
	}
	UpdateEtaLabel();
	SetBusy(YouTubeWorkflowManager::Instance()->IsRegeneratingMusic());
	show();
}

void YouTubeMusicPicker::Close()
{
	StopPlayback();
	hide();
}

void YouTubeMusicPicker::Play(QPushButton *button, const QString &file)
{
	bool same = playingButton == button;
	StopPlayback();
	if (same || !QFileInfo::exists(file)) {
		return;
	}
	playingButton = button;
	button->setText("⏹ Stop");
	/* From where the row's slider was left; from the start once it has reached the end */
	playingSlider = qobject_cast<QSlider *>(button->property("slider").value<QObject *>());
	qint64 startMs = playingSlider ? playingSlider->value() : 0;
	if (playingSlider && startMs >= playingSlider->maximum() - 250) {
		startMs = 0;
	}
	positionTimer->start();
	/* Inside OBS, Windows Media Foundation stalls on audio-only files after a moment, so WAVs go
	 * straight to the sound card; the media player is only for older M4A previews */
	if (file.endsWith(".wav", Qt::CaseInsensitive) && PlayWav(file, startMs)) {
		return;
	}
	player->setSource(QUrl::fromLocalFile(file));
	player->setPosition(startMs);
	player->play();
}

bool YouTubeMusicPicker::PlayWav(const QString &path, qint64 startMs)
{
	QFile *file = new QFile(path, this);
	QAudioFormat format;
	qint64 dataOffset = 0;
	QAudioDevice device = QMediaDevices::defaultAudioOutput();
	if (!file->open(QIODevice::ReadOnly) || !ReadWavHeader(*file, format, dataOffset) || device.isNull() ||
	    !device.isFormatSupported(format)) {
		delete file;
		return false;
	}
	wavFile = file;
	wavFormat = format;
	wavDataOffset = dataOffset;
	if (!SeekWav(startMs)) {
		wavFile = nullptr;
		delete file;
		return false;
	}
	StartSink();
	return true;
}

bool YouTubeMusicPicker::SeekWav(qint64 ms)
{
	qint64 frame = ms * wavFormat.sampleRate() / 1000;
	qint64 offset = std::min(wavDataOffset + frame * wavFormat.bytesPerFrame(), wavFile->size());
	playStartMs = ms;
	return wavFile->seek(offset);
}

void YouTubeMusicPicker::StartSink()
{
	sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), wavFormat, this);
	connect(sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
		/* Idle once the file is read and the sound card has played what it was given */
		if (state == QAudio::IdleState && wavFile && wavFile->atEnd()) {
			ReachedEnd();
		}
	});
	sink->start(wavFile);
}

void YouTubeMusicPicker::SeekTo(qint64 ms)
{
	if (sink && wavFile) {
		/* A sink can't skip, so the file moves and a new sink starts there */
		StopSink();
		if (SeekWav(ms)) {
			StartSink();
		}
	} else if (player->playbackState() == QMediaPlayer::PlayingState) {
		player->setPosition(ms);
	}
}

void YouTubeMusicPicker::ShowPosition()
{
	if (!playingSlider || playingSlider->isSliderDown()) {
		return;
	}
	if (sink) {
		playingSlider->setValue(int(playStartMs + sink->processedUSecs() / 1000));
	} else if (player->playbackState() == QMediaPlayer::PlayingState) {
		playingSlider->setValue(int(player->position()));
	}
}

void YouTubeMusicPicker::ReachedEnd()
{
	QSlider *slider = playingSlider;
	StopPlayback();
	if (slider) {
		slider->setValue(0);
	}
}

void YouTubeMusicPicker::StopSink()
{
	if (sink) {
		sink->disconnect(this);
		sink->stop();
		sink->deleteLater();
		sink = nullptr;
	}
}

void YouTubeMusicPicker::StopPlayback()
{
	/* The slider stays where it stopped, so ▶ picks up from there */
	positionTimer->stop();
	ShowPosition();
	playingSlider = nullptr;
	StopSink();
	/* Let go of the file, so the pipeline can write over it */
	if (wavFile) {
		wavFile->close();
		wavFile->deleteLater();
		wavFile = nullptr;
	}
	player->stop();
	player->setSource(QUrl());
	if (playingButton) {
		playingButton->setText(playingButton->property("idleText").toString());
		playingButton = nullptr;
	}
}

void YouTubeMusicPicker::Regenerate()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QString style = styleEdit->toPlainText().trimmed();
	if (style.isEmpty()) {
		emit mgr->LogMessageReceived("warning", "Describe the music you want first (genre, mood, instruments).");
		return;
	}
	StopPlayback();
	QJsonObject options;
	options["style"] = style;
	if (QAbstractButton *checked = group->checkedButton()) {
		options["base_id"] = checked->property("songId").toString();
	}
	options["music_source"] = sourceCombo->currentData().toString();
	mgr->RegenerateMusic(options);
}

void YouTubeMusicPicker::Finish()
{
	QAbstractButton *checked = group->checkedButton();
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (!checked) {
		emit mgr->LogMessageReceived("warning", "Pick a song (or No music) first.");
		return;
	}
	QJsonObject root = ReadJson(musicPath);
	root["chosen"] = checked->property("songId").toString();
	QSaveFile out(musicPath);
	if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(root).toJson()) < 0 || !out.commit()) {
		emit mgr->LogMessageReceived("error", QString("Could not save %1").arg(musicPath));
		return;
	}
	Close();
	mgr->ResumePipeline(statePath);
}

void YouTubeMusicPicker::SetBusy(bool busy)
{
	listBox->setEnabled(!busy);
	styleEdit->setEnabled(!busy);
	regenButton->setEnabled(!busy);
	useButton->setEnabled(!busy);
	busyRow->setVisible(busy);
	if (busy) {
		StopPlayback();
		busyBar->setValue(0);
		busyLabel->setText("Starting...");
	}
}

void YouTubeMusicPicker::UpdateEtaLabel()
{
	/* How long the music source took last time (written by the pipeline) */
	QJsonObject t = ReadJson(QDir::homePath() + "/OBSYouTubeMusicGen/timings.json");
	if (sourceCombo->currentData().toString() == "lyria") {
		/* The pipeline's sections: up to 240 s each, joined with 6 s crossfades, 10 s past the end */
		double target = videoSeconds + 10.0;
		int parts = target <= 240.0 ? 1 : int(std::ceil((target - 6.0) / 234.0));
		int songs = regenCount * parts;
		double seconds = std::ceil(songs / 3.0) * t.value("lyria_s").toDouble(30.0);
		regenButton->setText(videoSeconds > 0 ? QString("🎵 Make %1 new songs with Lyria (~%2, ~$%3)")
								.arg(regenCount)
								.arg(Clock(seconds))
								.arg(songs * 0.08, 0, 'f', 2)
						      : QString("🎵 Make %1 new songs with Lyria").arg(regenCount));
		regenButton->setToolTip("Make new songs with this style with Google's Lyria, on your Gemini key "
					"(about $0.08 per 2-minute song); the songs above stay");
		return;
	}
	double load = t.value("load_s").toDouble(90.0);
	double rate = t.value("rate").toDouble(1.0);
	double seconds = load + regenCount * (videoSeconds + 10.0) * rate + 10.0 * regenCount;
	regenButton->setText(videoSeconds > 0 ? QString("🎵 Make %1 new songs (~%2)").arg(regenCount).arg(Clock(seconds))
					      : QString("🎵 Make %1 new songs").arg(regenCount));
	regenButton->setToolTip("Make new songs with this style on this PC (ACE-Step); the songs above stay");
}

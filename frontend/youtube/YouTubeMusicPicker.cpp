#include "YouTubeMusicPicker.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QAbstractButton>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QAudioSink>
#include <QButtonGroup>
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
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QUrl>
#include <QVBoxLayout>
#include <QtEndian>

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
	styleEdit->setToolTip("The style prompt for ACE-Step. It starts as the selected song's; a tempo like "
			      "\"100 bpm\" in it is used too.");
	regenLayout->addWidget(styleEdit);
	regenButton = new QPushButton(regenBox);
	regenButton->setToolTip("Make new songs with this style on this PC (ACE-Step); the songs above stay");
	regenLayout->addWidget(regenButton);
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
	layout->addWidget(useButton);

	player = new QMediaPlayer(this);
	audio = new QAudioOutput(this);
	player->setAudioOutput(audio);
	connect(player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
		if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) {
			StopPlayback();
		}
	});

	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(useButton, &QPushButton::clicked, this, &YouTubeMusicPicker::Finish);
	connect(regenButton, &QPushButton::clicked, this, &YouTubeMusicPicker::Regenerate);
	connect(stopButton, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);
	connect(mgr, &YouTubeWorkflowManager::MusicRegenStarted, this, [this]() { SetBusy(true); });
	connect(mgr, &YouTubeWorkflowManager::MusicRegenFinished, this, [this]() { SetBusy(false); });
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
	infoLabel->setText(QString("Made on this PC with ACE-Step%1. Each song is fitted to the video and plays about "
				   "20 dB under your voice, dipping further while you talk.")
				   .arg(by.isEmpty() ? QString() : QString(", styles by %1").arg(by)));
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
		QHBoxLayout *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		QRadioButton *radio = new QRadioButton(
			QString("%1 · %2").arg(o.value("label").toString(), o.value("name").toString()), row);
		QString style = o.value("style").toString();
		QString details = QString("%1 BPM, %2%3")
					  .arg(o.value("bpm").toInt())
					  .arg(o.value("key").toString())
					  .arg(o.value("instrumental").toBool(true) ? ", instrumental" : ", with vocals");
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
	/* Inside OBS, Windows Media Foundation stalls on audio-only files after a moment, so WAVs go
	 * straight to the sound card; the media player is only for older M4A previews */
	if (file.endsWith(".wav", Qt::CaseInsensitive) && PlayWav(file)) {
		return;
	}
	player->setSource(QUrl::fromLocalFile(file));
	player->play();
}

bool YouTubeMusicPicker::PlayWav(const QString &path)
{
	QFile *file = new QFile(path, this);
	QAudioFormat format;
	qint64 dataOffset = 0;
	QAudioDevice device = QMediaDevices::defaultAudioOutput();
	if (!file->open(QIODevice::ReadOnly) || !ReadWavHeader(*file, format, dataOffset) || device.isNull() ||
	    !device.isFormatSupported(format) || !file->seek(dataOffset)) {
		delete file;
		return false;
	}
	wavFile = file;
	sink = new QAudioSink(device, format, this);
	connect(sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
		/* Idle once the file is read and the sound card has played what it was given */
		if (state == QAudio::IdleState && wavFile && wavFile->atEnd()) {
			StopPlayback();
		}
	});
	sink->start(wavFile);
	return true;
}

void YouTubeMusicPicker::StopPlayback()
{
	if (sink) {
		sink->disconnect(this);
		sink->stop();
		sink->deleteLater();
		sink = nullptr;
	}
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
	/* How long ACE-Step took last time on this PC (written by the pipeline) */
	QJsonObject t = ReadJson(QDir::homePath() + "/OBSYouTubeMusicGen/timings.json");
	double load = t.value("load_s").toDouble(90.0);
	double rate = t.value("rate").toDouble(1.0);
	double seconds = load + regenCount * (videoSeconds + 10.0) * rate + 10.0 * regenCount;
	regenButton->setText(videoSeconds > 0 ? QString("🎵 Make %1 new songs (~%2)").arg(regenCount).arg(Clock(seconds))
					      : QString("🎵 Make %1 new songs").arg(regenCount));
}

#pragma once

#include <QAudioFormat>
#include <QGroupBox>
#include <QString>

class QAudioOutput;
class QAudioSink;
class QButtonGroup;
class QFile;
class QLabel;
class QMediaPlayer;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;
class QVBoxLayout;
class QWidget;

/* Step mode: choose the background music ({stem}_yt_music.json) made with ACE-Step on this PC or with
 * Google's Lyria. Each song can be heard on its own or under your voice, from any point on its seek bar;
 * new ones can be made from an edited style prompt. Shown in the YouTube dock while the pipeline waits at
 * the music gate. */
class YouTubeMusicPicker : public QGroupBox {
	Q_OBJECT

public:
	explicit YouTubeMusicPicker(QWidget *parent = nullptr);

	void Load(const QString &musicPath, const QString &statePath);
	/* Stops playback and hides (the review is over) */
	void Close();

private:
	QLabel *infoLabel;
	QLabel *notesLabel;
	QVBoxLayout *listLayout;
	QWidget *listBox;
	QButtonGroup *group;
	QPlainTextEdit *styleEdit;
	QPushButton *regenButton;
	QPushButton *useButton;
	QWidget *busyRow;
	QProgressBar *busyBar;
	QLabel *busyLabel;

	QMediaPlayer *player;
	QAudioOutput *audio;
	/* WAVs play through these instead of the media player */
	QAudioSink *sink = nullptr;
	QFile *wavFile = nullptr;
	QAudioFormat wavFormat;
	qint64 wavDataOffset = 0;
	/* Where in the song the current sink started */
	qint64 playStartMs = 0;
	QPushButton *playingButton = nullptr;
	/* The playing row's seek bar, moved along by the timer */
	QSlider *playingSlider = nullptr;
	QTimer *positionTimer = nullptr;

	QString musicPath;
	QString statePath;
	double videoSeconds = 0.0;
	int regenCount = 2;

	void Play(QPushButton *button, const QString &file);
	bool PlayWav(const QString &path, qint64 startMs);
	bool SeekWav(qint64 ms);
	void StartSink();
	void StopSink();
	/* Jumps the playing song to ms */
	void SeekTo(qint64 ms);
	void ShowPosition();
	/* The song played to its end: stop and put its seek bar back to the start */
	void ReachedEnd();
	void StopPlayback();
	void Finish();
	void Regenerate();
	void SetBusy(bool busy);
	void UpdateEtaLabel();
};

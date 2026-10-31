#pragma once

#include <QGroupBox>
#include <QString>

class QAudioOutput;
class QButtonGroup;
class QLabel;
class QMediaPlayer;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

/* Step mode: choose the background music ({stem}_yt_music.json) made with ACE-Step on this PC.
 * Each song can be heard on its own or under your voice; new ones can be made from an edited style
 * prompt. Shown in the YouTube dock while the pipeline waits at the music gate. */
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
	QPushButton *playingButton = nullptr;

	QString musicPath;
	QString statePath;
	double videoSeconds = 0.0;
	int regenCount = 2;

	void Play(QPushButton *button, const QString &file);
	void StopPlayback();
	void Finish();
	void Regenerate();
	void SetBusy(bool busy);
	void UpdateEtaLabel();
};

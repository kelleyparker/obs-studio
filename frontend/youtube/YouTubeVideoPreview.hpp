#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QAudioOutput;
class QComboBox;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QVideoWidget;

/* Scrub bar for the rendered edit, with a tick at every cut join (orange) and chapter (blue) */
class EditScrubBar : public QWidget {
	Q_OBJECT

public:
	explicit EditScrubBar(QWidget *parent = nullptr);

	void SetDuration(double seconds);
	void SetPosition(double seconds);
	void SetMarks(const QList<double> &joins, const QList<double> &chapters);

signals:
	void SeekRequested(double seconds);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;

private:
	double duration = 0.0;
	double position = 0.0;
	QList<double> joins;
	QList<double> chapters;

	double TimeAt(int x) const;
	int XAt(double seconds) const;
};

/* "Watch Edit": plays the rendered video before it is exported or uploaded */
class YouTubeVideoPreview : public QDialog {
	Q_OBJECT

public:
	explicit YouTubeVideoPreview(QWidget *parent = nullptr);
	~YouTubeVideoPreview() override;

	void Load(const QString &videoPath, const QList<double> &joins, const QString &description);
	/* Releases the file so the pipeline can render over it */
	void Unload();

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	QMediaPlayer *player;
	QAudioOutput *audio;
	QVideoWidget *video;
	EditScrubBar *scrub;
	QPushButton *playButton;
	QLabel *timeLabel;
	QComboBox *chapterCombo;
	QString videoPath;
	QList<double> joins;
	double duration = 0.0;

	double Position() const;
	void Seek(double seconds);
	void StepJoin(int direction);
	void SaveGeometry();
};

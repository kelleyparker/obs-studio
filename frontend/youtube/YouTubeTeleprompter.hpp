#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTextDocument;
class QTimer;

/* A teleprompter for the scripts the Script Writer makes: large text on black that scrolls at a set number
 * of words per minute past a reading line. Headings, the title and [show: ...] cues are dimmed because they
 * aren't read aloud. It can mirror for teleprompter glass, stay on top, keep out of screen recordings, and
 * start scrolling (after a countdown) when a recording starts. One window for the whole app. */
class YouTubeTeleprompter : public QWidget {
	Q_OBJECT

public:
	static YouTubeTeleprompter *Get();

	/* Shows a saved script (.md); false if it can't be read */
	bool LoadFile(const QString &path);
	void LoadText(const QString &markdown, const QString &path = QString());
	void Play();
	void Pause();
	void TogglePlay();
	/* Back to the first line at the reading line */
	void Restart();

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	explicit YouTubeTeleprompter(QWidget *parent = nullptr);

	friend class TeleprompterCanvas;

	void BuildDocument();
	void Tick();
	void Nudge(double lines);
	void SetWpm(int wpm);
	void SetFontSize(int px);
	void ApplyCaptureExclusion();
	void ApplyOnTop();
	void UpdatePlayButton();
	void SaveSettings();
	/* Pixels of scrolling per spoken word, so the speed can be set in words per minute */
	double PixelsPerWord() const;
	double ReadingLineY() const;

	QWidget *canvas = nullptr;
	QWidget *toolbar = nullptr;
	QPushButton *playBtn = nullptr;
	QSpinBox *wpmSpin = nullptr;
	QSpinBox *fontSpin = nullptr;
	QCheckBox *mirrorCheck = nullptr;
	QCheckBox *onTopCheck = nullptr;
	QCheckBox *hideCaptureCheck = nullptr;
	QCheckBox *autoStartCheck = nullptr;
	QLabel *statusLabel = nullptr;

	QTextDocument *doc = nullptr;
	QTimer *timer = nullptr;
	QElapsedTimer clock;
	QString markdown;
	QString scriptPath;
	int spokenWords = 0;
	double scrollY = 0.0; /* document y at the reading line */
	bool playing = false;
	int countdownMs = 0;
};

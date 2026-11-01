#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class YouTubeHeartbeat;

/* Writes the spoken script for the next video before it's recorded: Claude (Opus 5.5 at high effort by
 * default, from Settings) suggests topics or writes and revises the script, which streams in, is saved
 * to %USERPROFILE%\OBSYouTubeScripts and opens in the teleprompter. Only text is sent to Claude. */
class YouTubeScriptWriter : public QDialog {
	Q_OBJECT

public:
	explicit YouTubeScriptWriter(QWidget *parent = nullptr);

	/* "Claude Opus 5.5" etc. for the settings and the buttons */
	static void FillModels(QComboBox *combo);
	static void FillEfforts(QComboBox *combo);
	/* Rough US$ for one script of this many minutes, from typical token use at this effort */
	static double EstimateCost(const QString &model, const QString &effort, double minutes);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void SuggestTopics();
	void Write(bool revise);
	bool Save();
	void OpenTeleprompter();
	void OpenSaved(const QString &path);
	void RefreshSaved();
	void ShowIdeas(const QJsonArray &ideas, const QString &cost);
	void SetBusy(bool busy, const QString &what = QString());
	void UpdateButtons();
	void UpdateStatus(const QString &extra = QString());
	QJsonArray TitleDirs() const;

	QLineEdit *topicEdit = nullptr;
	QPlainTextEdit *notesEdit = nullptr;
	QSpinBox *minutesSpin = nullptr;
	QComboBox *toneCombo = nullptr;
	QComboBox *modelCombo = nullptr;
	QComboBox *effortCombo = nullptr;
	QPushButton *ideasBtn = nullptr;
	QListWidget *ideasList = nullptr;
	QPushButton *writeBtn = nullptr;
	QLineEdit *feedbackEdit = nullptr;
	QPushButton *reviseBtn = nullptr;
	QListWidget *savedList = nullptr;
	QPlainTextEdit *scriptEdit = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *saveBtn = nullptr;
	QPushButton *promptBtn = nullptr;
	QPushButton *stopBtn = nullptr;
	YouTubeHeartbeat *writeHeart = nullptr;
	YouTubeHeartbeat *promptHeart = nullptr;

	QString currentPath;
	QString lastCost;
	bool busy = false;
	bool streaming = false;
};

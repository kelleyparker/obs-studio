#pragma once

#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;

/* A plain text notepad. Untitled notes save themselves to the config
 * folder, so nothing is lost if OBS closes. Opened files only save when
 * asked to. */
class NotepadDock : public QWidget {
	Q_OBJECT

public:
	NotepadDock(QWidget *parent = nullptr);
	~NotepadDock();

private:
	QPlainTextEdit *edit;
	QLabel *status;
	QPushButton *wrapButton;
	QTimer autoSaveTimer;
	QString currentPath; /* empty while the note is untitled */
	bool loading = false;

	/* AI Generate: sample text from the local Ollama, never a cloud model */
	QPushButton *generateButton;
	QNetworkAccessManager *network;
	QPointer<QNetworkReply> reply;
	QByteArray pendingLine;
	QString generateModel;
	QString generatePrompt;
	QString generateStatus;
	QString generateError;
	bool generating = false;
	bool ollamaStarted = false;
	int generateId = 0;

	static QString ScratchPath();

	void New();
	void Open();
	bool Save();
	bool SaveAs();
	void ChooseFont();
	void SetWordWrap(bool wrap);

	void ToggleGenerate();
	void StartGenerate();
	void CheckOllama(int triesLeft);
	bool StartOllama();
	void SendGenerate();
	void ReadGenerated(bool flush);
	void FinishGenerate(const QString &error);

	bool HasUnsavedChanges() const;
	bool MaybeSave();
	bool LoadFile(const QString &path);
	bool WriteFile(const QString &path);
	void SetDocument(const QString &path, const QString &text);
	void SaveScratch();
	void UpdateStatus();
};

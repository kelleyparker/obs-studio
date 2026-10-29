#pragma once

#include <QString>
#include <QTimer>
#include <QWidget>

class QAction;
class QLabel;
class QPlainTextEdit;

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
	QAction *wrapAction;
	QTimer autoSaveTimer;
	QString currentPath; /* empty while the note is untitled */
	bool loading = false;

	static QString ScratchPath();

	void New();
	void Open();
	bool Save();
	bool SaveAs();
	void ChooseFont();
	void SetWordWrap(bool wrap);

	bool HasUnsavedChanges() const;
	bool MaybeSave();
	bool LoadFile(const QString &path);
	bool WriteFile(const QString &path);
	void SetDocument(const QString &path, const QString &text);
	void SaveScratch();
	void UpdateStatus();
};

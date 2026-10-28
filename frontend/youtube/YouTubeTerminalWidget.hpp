#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QWinEventNotifier;

/* Hosts a real Windows console (PowerShell under conhost) inside the dock.
 * Windows Terminal cannot be reparented into another app, so the classic
 * console host is embedded instead: colors, line editing and full-screen
 * console apps behave exactly as they do in a standalone console window.
 * The shell starts the first time the widget is shown, and everything it
 * launched is killed when OBS closes (the processes live in a job object). */
class YouTubeTerminalWidget : public QWidget {
	Q_OBJECT

public:
	explicit YouTubeTerminalWidget(QWidget *parent = nullptr);
	~YouTubeTerminalWidget() override;

	QSize sizeHint() const override;

protected:
	void showEvent(QShowEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void StartShell();
	void StopShell();
	void AttachConsoleWindow();
	void FitConsoleWindow();
	void ShowPlaceholder(const QString &text, bool canRestart);

	QWidget *host = nullptr;
	QWidget *placeholder = nullptr;
	QLabel *placeholderLabel = nullptr;
	QPushButton *restartBtn = nullptr;
	bool started = false;

#ifdef _WIN32
	void *job = nullptr;
	void *process = nullptr;
	void *consoleHwnd = nullptr;
	QWinEventNotifier *exitNotifier = nullptr;
	int attachAttempts = 0;
#endif
};

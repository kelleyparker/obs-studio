#include "YouTubeTerminalWidget.hpp"

#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
#include <QWinEventNotifier>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace {

/* The console belongs to another process, so clicking it doesn't move keyboard
 * focus to it. The click still reaches its parent as WM_PARENTNOTIFY, which is
 * where focus gets handed over. */
class ConsoleHostWidget : public QWidget {
public:
	using QWidget::QWidget;
	std::function<void()> onChildClicked;

protected:
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override
	{
		const MSG *msg = static_cast<const MSG *>(message);
		if (msg->message == WM_PARENTNOTIFY && onChildClicked) {
			const UINT what = LOWORD(msg->wParam);
			if (what == WM_LBUTTONDOWN || what == WM_RBUTTONDOWN || what == WM_MBUTTONDOWN)
				onChildClicked();
		}
		return QWidget::nativeEvent(eventType, message, result);
	}
};

struct ConsoleSearch {
	std::vector<DWORD> pids;
	HWND hwnd = nullptr;
};

BOOL CALLBACK FindConsoleWindowProc(HWND hwnd, LPARAM lParam)
{
	auto *search = reinterpret_cast<ConsoleSearch *>(lParam);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (std::find(search->pids.begin(), search->pids.end(), pid) == search->pids.end())
		return TRUE;

	wchar_t cls[64] = {};
	if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"ConsoleWindowClass") == 0) {
		search->hwnd = hwnd;
		return FALSE;
	}
	return TRUE;
}

/* A console window reports the PID of its first client (the shell) rather
 * than conhost's, so match it against every process in the job. */
HWND FindJobConsoleWindow(HANDLE job)
{
	constexpr DWORD maxPids = 64;
	std::vector<BYTE> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + maxPids * sizeof(ULONG_PTR));
	auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(buf.data());
	if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, (DWORD)buf.size(), nullptr) &&
	    GetLastError() != ERROR_MORE_DATA)
		return nullptr;

	ConsoleSearch search;
	for (DWORD i = 0; i < list->NumberOfProcessIdsInList; i++)
		search.pids.push_back((DWORD)list->ProcessIdList[i]);
	EnumWindows(FindConsoleWindowProc, reinterpret_cast<LPARAM>(&search));
	return search.hwnd;
}

QString FindShell()
{
	const QString pwsh = QStandardPaths::findExecutable("pwsh");
	return pwsh.isEmpty() ? QStringLiteral("powershell.exe") : pwsh;
}

} // namespace
#endif

YouTubeTerminalWidget::YouTubeTerminalWidget(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// Native window the console is reparented into
#ifdef _WIN32
	ConsoleHostWidget *consoleHost = new ConsoleHostWidget(this);
	consoleHost->onChildClicked = [this]() {
		if (consoleHwnd)
			SetFocus(reinterpret_cast<HWND>(consoleHwnd));
	};
	host = consoleHost;
#else
	host = new QWidget(this);
#endif
	host->setAttribute(Qt::WA_NativeWindow);
	host->setAttribute(Qt::WA_StyledBackground);
	host->setStyleSheet("background-color: #0c0c0c;");
	host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	host->installEventFilter(this);
	layout->addWidget(host);

	placeholder = new QWidget(this);
	QVBoxLayout *placeholderLayout = new QVBoxLayout(placeholder);
	placeholderLabel = new QLabel(placeholder);
	placeholderLabel->setAlignment(Qt::AlignCenter);
	placeholderLabel->setWordWrap(true);
	placeholderLabel->setStyleSheet("color: #9a88ab;");
	restartBtn = new QPushButton("🔄 Restart Terminal", placeholder);
	connect(restartBtn, &QPushButton::clicked, this, &YouTubeTerminalWidget::StartShell);
	placeholderLayout->addStretch();
	placeholderLayout->addWidget(placeholderLabel);
	placeholderLayout->addWidget(restartBtn, 0, Qt::AlignHCenter);
	placeholderLayout->addStretch();
	layout->addWidget(placeholder);
	placeholder->hide();

#ifndef _WIN32
	started = true;
	ShowPlaceholder("The embedded terminal is only available on Windows.", false);
#endif
}

YouTubeTerminalWidget::~YouTubeTerminalWidget()
{
	StopShell();
}

QSize YouTubeTerminalWidget::sizeHint() const
{
	return QSize(400, 280);
}

void YouTubeTerminalWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (!started) {
		// Start on first show so a hidden dock never spawns a shell
		started = true;
		QTimer::singleShot(0, this, &YouTubeTerminalWidget::StartShell);
	}
}

bool YouTubeTerminalWidget::eventFilter(QObject *watched, QEvent *event)
{
#ifdef _WIN32
	if (watched == host && consoleHwnd) {
		if (event->type() == QEvent::Resize) {
			FitConsoleWindow();
		} else if (event->type() == QEvent::WinIdChange) {
			SetParent(reinterpret_cast<HWND>(consoleHwnd), reinterpret_cast<HWND>(host->winId()));
			FitConsoleWindow();
		}
	}
#endif
	return QWidget::eventFilter(watched, event);
}

void YouTubeTerminalWidget::StartShell()
{
#ifdef _WIN32
	StopShell();

	HANDLE newJob = CreateJobObjectW(nullptr, nullptr);
	if (!newJob) {
		ShowPlaceholder(QString("Could not create the terminal job (error %1).").arg(GetLastError()), true);
		return;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject(newJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

	/* Launching conhost explicitly keeps the console out of Windows Terminal
	 * when that is the default terminal app, so the window can be embedded. */
	wchar_t sysDir[MAX_PATH] = {};
	GetSystemDirectoryW(sysDir, MAX_PATH);
	const QString conhost = QDir(QString::fromWCharArray(sysDir)).filePath("conhost.exe");
	std::wstring cmdLine = QString("\"%1\" \"%2\" -NoLogo")
				       .arg(QDir::toNativeSeparators(conhost), QDir::toNativeSeparators(FindShell()))
				       .toStdWString();
	const std::wstring cwd = QDir::toNativeSeparators(QDir::homePath()).toStdWString();

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(),
			    &si, &pi)) {
		const DWORD err = GetLastError();
		CloseHandle(newJob);
		ShowPlaceholder(QString("Could not start the terminal (error %1).").arg(err), true);
		return;
	}
	AssignProcessToJobObject(newJob, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	job = newJob;
	process = pi.hProcess;
	exitNotifier = new QWinEventNotifier(pi.hProcess, this);
	connect(exitNotifier, &QWinEventNotifier::activated, this, [this]() {
		StopShell();
		ShowPlaceholder("The shell exited.", true);
	});

	placeholder->hide();
	host->show();
	attachAttempts = 0;
	AttachConsoleWindow();
#endif
}

void YouTubeTerminalWidget::StopShell()
{
#ifdef _WIN32
	if (exitNotifier) {
		// May be called from the notifier's own signal, so don't delete it directly
		exitNotifier->setEnabled(false);
		exitNotifier->deleteLater();
		exitNotifier = nullptr;
	}
	consoleHwnd = nullptr;
	if (process) {
		TerminateProcess(reinterpret_cast<HANDLE>(process), 0);
		CloseHandle(reinterpret_cast<HANDLE>(process));
		process = nullptr;
	}
	if (job) {
		// KILL_ON_JOB_CLOSE ends the shell and anything it launched
		CloseHandle(reinterpret_cast<HANDLE>(job));
		job = nullptr;
	}
#endif
}

void YouTubeTerminalWidget::AttachConsoleWindow()
{
#ifdef _WIN32
	if (!job || consoleHwnd)
		return;

	HWND hwnd = FindJobConsoleWindow(reinterpret_cast<HANDLE>(job));
	if (!hwnd) {
		if (++attachAttempts < 50) {
			QTimer::singleShot(100, this, &YouTubeTerminalWidget::AttachConsoleWindow);
		} else {
			StopShell();
			ShowPlaceholder("The console window never appeared.", true);
		}
		return;
	}

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
	style |= WS_CHILD;
	SetWindowLongPtrW(hwnd, GWL_STYLE, style);
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	exStyle &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_NOPARENTNOTIFY);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
	SetParent(hwnd, reinterpret_cast<HWND>(host->winId()));

	consoleHwnd = hwnd;
	FitConsoleWindow();
	ShowWindow(hwnd, SW_SHOW);
#endif
}

void YouTubeTerminalWidget::FitConsoleWindow()
{
#ifdef _WIN32
	if (!consoleHwnd)
		return;

	RECT rc = {};
	GetClientRect(reinterpret_cast<HWND>(host->winId()), &rc);
	SetWindowPos(reinterpret_cast<HWND>(consoleHwnd), nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
		     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void YouTubeTerminalWidget::ShowPlaceholder(const QString &text, bool canRestart)
{
	host->hide();
	placeholderLabel->setText(text);
	restartBtn->setVisible(canRestart);
	placeholder->show();
}

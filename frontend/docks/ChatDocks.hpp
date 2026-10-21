#pragma once

#include <utility/StreamPlatforms.hpp>

#include <QDockWidget>
#include <QObject>
#include <QPointer>

#include <map>
#include <string>

class BrowserDock;
class MultistreamOutput;
class OBSBasic;
class QCefWidget;
class QLabel;
class QPushButton;
class QStackedWidget;
class StreamInfoFetcher;

/* A dock with the pop-out chat of the main stream's channel and of every
 * multistream destination with a Twitch, YouTube or Kick channel. The docks
 * are always there; they are shown and hidden from the Docks menu. They use
 * the browser docks' cookies, so a sign-in is kept. */
class MultistreamChatDocks : public QObject {
	Q_OBJECT

public:
	MultistreamChatDocks(OBSBasic *main, MultistreamOutput *multistream, StreamInfoFetcher *streamInfo);
	~MultistreamChatDocks();

	void SetFeatures(QDockWidget::DockWidgetFeatures features);
	/* Puts every chat dock back on the right, hidden (Docks > Reset UI) */
	void ResetLayout();
	/* Removes every chat dock; for closing OBS */
	void Clear();

private:
	struct Dock {
		StreamPlatform platform = StreamPlatform::None;
		std::string channel;
		QString name;
		std::string chatUrl;

		BrowserDock *dock = nullptr;
		QStackedWidget *stack = nullptr;
		QLabel *waiting = nullptr;
		QCefWidget *browser = nullptr; /* owned by the dock */
	};

	OBSBasic *main;
	QPointer<MultistreamOutput> multistream;
	QPointer<StreamInfoFetcher> streamInfo;
	std::map<std::string, Dock> docks;

	void Update();
	void CreateDock(const std::string &id, Dock &dock);
	void RemoveDock(Dock &dock);
	void UpdateChat(const std::string &id);
	QString CurrentChatUrl(const std::string &id, const Dock &dock) const;
};

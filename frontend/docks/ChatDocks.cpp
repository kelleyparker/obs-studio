#include "ChatDocks.hpp"

#include <docks/BrowserDock.hpp>
#include <utility/MultistreamOutput.hpp>
#include <utility/StreamInfo.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

using namespace std;

static QString DockObjectName(const string &id)
{
	return QStringLiteral("multistreamChat_") + QString::fromStdString(id);
}

MultistreamChatDocks::MultistreamChatDocks(OBSBasic *main_, MultistreamOutput *multistream_,
					   StreamInfoFetcher *streamInfo_)
	: main(main_),
	  multistream(multistream_),
	  streamInfo(streamInfo_)
{
	if (multistream) {
		connect(multistream, &MultistreamOutput::DestinationsChanged, this, &MultistreamChatDocks::Update);
		/* The main stream's platform may have changed in Settings > Stream */
		connect(multistream, &MultistreamOutput::PrimaryStateChanged, this, &MultistreamChatDocks::Update);
	}
	if (streamInfo) {
		connect(streamInfo, &StreamInfoFetcher::InfoChanged, this,
			[this](const QString &id) { UpdateChat(id.toStdString()); });
	}
	Update();
}

MultistreamChatDocks::~MultistreamChatDocks()
{
	Clear();
}

void MultistreamChatDocks::Clear()
{
	for (auto &[id, dock] : docks) {
		RemoveDock(dock);
	}
	docks.clear();
}

void MultistreamChatDocks::SetFeatures(QDockWidget::DockWidgetFeatures features)
{
	for (auto &[id, dock] : docks) {
		dock.dock->setFeatures(features);
	}
}

void MultistreamChatDocks::ResetLayout()
{
	for (auto &[id, dock] : docks) {
		dock.dock->setFloating(false);
		main->addDockWidget(Qt::RightDockWidgetArea, dock.dock);
		dock.dock->setVisible(false);
	}
}

void MultistreamChatDocks::Update()
{
	map<string, Dock> updated;

	for (const StreamChannel &channel : GetStreamChannels(multistream)) {
		/* A chat needs the channel */
		if (channel.channel.empty()) {
			continue;
		}

		auto old = docks.find(channel.id);
		if (old != docks.end() && old->second.platform == channel.platform &&
		    old->second.channel == channel.channel) {
			Dock &dock = old->second;
			if (dock.name != channel.name) {
				dock.name = channel.name;
				QString title = QTStr("Multistream.Chat.Title").arg(dock.name);
				dock.dock->setTitle(title);
				dock.dock->setWindowTitle(title);
			}
			updated.emplace(channel.id, std::move(dock));
			docks.erase(old);
			continue;
		}

		Dock dock;
		dock.platform = channel.platform;
		dock.channel = channel.channel;
		dock.name = channel.name;
		updated.emplace(channel.id, std::move(dock));
	}

	/* Docks of removed or changed channels */
	for (auto &[id, dock] : docks) {
		RemoveDock(dock);
	}

	docks = std::move(updated);
	for (auto &[id, dock] : docks) {
		if (!dock.dock) {
			CreateDock(id, dock);
		}
		UpdateChat(id);
	}
}

void MultistreamChatDocks::CreateDock(const string &id, Dock &dock)
{
	QString title = QTStr("Multistream.Chat.Title").arg(dock.name);

	dock.dock = new BrowserDock(title);
	dock.dock->setObjectName(DockObjectName(id));
	dock.dock->setWindowTitle(title);
	dock.dock->resize(380, 600);
	dock.dock->setMinimumSize(80, 80);
	dock.dock->setAllowedAreas(Qt::AllDockWidgetAreas);

	QWidget *container = new QWidget(dock.dock);

	/* Header: the dashboard link, opening the chat in the web browser, and
	 * reloading it */
	QPushButton *dashboard = new QPushButton(QTStr("Multistream.Dashboard"), container);
	dashboard->setToolTip(QTStr("Multistream.Dashboard.Tooltip").arg(StreamPlatformName(dock.platform)));
	QPushButton *popOut = new QPushButton(QTStr("Multistream.Chat.PopOut"), container);
	popOut->setToolTip(QTStr("Multistream.Chat.PopOut.Tooltip"));
	QPushButton *reload = new QPushButton(QTStr("Multistream.Chat.Reload"), container);

	QHBoxLayout *header = new QHBoxLayout();
	header->setContentsMargins(4, 4, 4, 4);
	header->addWidget(dashboard);
	header->addWidget(popOut);
	header->addStretch();
	header->addWidget(reload);

	dock.stack = new QStackedWidget(container);
	dock.waiting = new QLabel(dock.stack);
	dock.waiting->setAlignment(Qt::AlignCenter);
	dock.waiting->setWordWrap(true);
	dock.waiting->setMargin(16);
	dock.waiting->setProperty("class", "text-muted");
	dock.stack->addWidget(dock.waiting);

	QVBoxLayout *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addLayout(header);
	layout->addWidget(dock.stack, 1);
	dock.dock->setWidget(container);

	connect(dashboard, &QPushButton::clicked, this, [this, id]() {
		auto it = docks.find(id);
		if (it == docks.end()) {
			return;
		}
		string videoId = streamInfo ? streamInfo->YouTubeVideoId(id) : string();
		QDesktopServices::openUrl(QUrl(StreamDashboardUrl(it->second.platform, it->second.channel, videoId)));
	});
	connect(popOut, &QPushButton::clicked, this, [this, id]() {
		auto it = docks.find(id);
		if (it == docks.end()) {
			return;
		}
		QString url = CurrentChatUrl(id, it->second);
		if (!url.isEmpty()) {
			QDesktopServices::openUrl(QUrl(url));
		}
	});
	connect(reload, &QPushButton::clicked, this, [this, id]() {
		auto it = docks.find(id);
		if (it != docks.end() && it->second.browser) {
			it->second.browser->reloadPage();
		}
	});

	/* Hidden until it is shown from the Docks menu, unless the saved
	 * window layout has it */
	dock.dock->setVisible(false);
	main->AddDockWidget(dock.dock, Qt::RightDockWidgetArea, true, true);
}

void MultistreamChatDocks::RemoveDock(Dock &dock)
{
	if (!dock.dock) {
		return;
	}
	main->removeDockWidget(dock.dock);
	/* Also removes its entry from the Docks menu */
	delete dock.dock;
	dock.dock = nullptr;
	dock.browser = nullptr;
}

QString MultistreamChatDocks::CurrentChatUrl(const string &id, const Dock &dock) const
{
	string videoId;
	if (dock.platform == StreamPlatform::YouTube && streamInfo) {
		videoId = streamInfo->YouTubeVideoId(id);
	}
	return StreamChatUrl(dock.platform, dock.channel, videoId);
}

void MultistreamChatDocks::UpdateChat(const string &id)
{
	auto it = docks.find(id);
	if (it == docks.end() || !it->second.dock) {
		return;
	}
	Dock &dock = it->second;

	QString url = CurrentChatUrl(id, dock);
	if (url.isEmpty()) {
		/* YouTube's chat belongs to the live video, which isn't known
		 * until the channel is live */
		dock.waiting->setText(QTStr("Multistream.Chat.WaitingYouTube").arg(dock.name));
		dock.stack->setCurrentWidget(dock.waiting);
		return;
	}

	string urlString = url.toStdString();
	if (!dock.browser) {
		static int panelVersion = obs_browser_qcef_version();

		dock.browser = cef->create_widget(dock.stack, urlString, nullptr);
		if (!dock.browser) {
			dock.waiting->setText(QTStr("Multistream.Chat.BrowserFailed"));
			dock.stack->setCurrentWidget(dock.waiting);
			return;
		}
		/* For signing in, which opens a pop-up */
		if (panelVersion >= 1) {
			dock.browser->allowAllPopups(true);
		}
		dock.dock->cefWidget.reset(dock.browser);
		dock.stack->addWidget(dock.browser);
	} else if (urlString != dock.chatUrl) {
		dock.browser->setURL(urlString);
	}
	dock.chatUrl = urlString;
	dock.stack->setCurrentWidget(dock.browser);
}

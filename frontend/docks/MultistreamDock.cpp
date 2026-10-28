#include "MultistreamDock.hpp"

#include <utility/StreamInfo.hpp>
#include <utility/StreamPlatforms.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

using namespace std;

static QString StateText(MultistreamState state)
{
	switch (state) {
	case MultistreamState::Connecting:
		return QTStr("Multistream.Status.Connecting");
	case MultistreamState::Live:
		return QTStr("Multistream.Status.Live");
	case MultistreamState::Reconnecting:
		return QTStr("Multistream.Status.Reconnecting");
	case MultistreamState::Stopping:
		return QTStr("Multistream.Status.Stopping");
	case MultistreamState::Error:
		return QTStr("Multistream.Status.Error");
	case MultistreamState::Offline:
		break;
	}
	return QTStr("Multistream.Status.Offline");
}

static const char *StateClass(MultistreamState state)
{
	switch (state) {
	case MultistreamState::Live:
		return "text-success";
	case MultistreamState::Connecting:
	case MultistreamState::Reconnecting:
	case MultistreamState::Stopping:
		return "text-warning";
	case MultistreamState::Error:
		return "text-danger";
	case MultistreamState::Offline:
		break;
	}
	return "text-muted";
}

static void SetLabelClass(QLabel *label, const char *cls)
{
	label->setProperty("class", cls);
	label->style()->unpolish(label);
	label->style()->polish(label);
}

static bool FindChannel(const MultistreamOutput *multistream, const string &id, StreamChannel &result)
{
	for (StreamChannel &channel : GetStreamChannels(multistream)) {
		if (channel.id == id) {
			result = std::move(channel);
			return true;
		}
	}
	return false;
}

static QPushButton *CreateDashboardButton(QWidget *parent)
{
	return new QPushButton(QTStr("Multistream.Dashboard"), parent);
}

/* "Title · Category" */
static QString DetailsText(const StreamInfo &info)
{
	QStringList parts;
	if (!info.title.isEmpty()) {
		parts << info.title;
	}
	if (!info.category.isEmpty()) {
		parts << info.category;
	}
	return parts.join(QString(" %1 ").arg(QChar(0x00B7)));
}

static void SetDetails(QLabel *label, const StreamInfo &info)
{
	QString text = DetailsText(info);
	/* Long titles are cut off; the tooltip shows all of it */
	label->setText(label->fontMetrics().elidedText(text, Qt::ElideRight, 480));
	label->setToolTip(text);
	label->setVisible(!text.isEmpty());
}

static QString ViewersText(const StreamInfo &info)
{
	if (!info.live || info.viewers < 0) {
		return QString(QChar(0x2014));
	}
	return QTStr("Multistream.Viewers").arg(QLocale().toString((qlonglong)info.viewers));
}

MultistreamDock::MultistreamDock(MultistreamOutput *multistream_, StreamInfoFetcher *streamInfo_, QWidget *parent)
	: QWidget(parent),
	  multistream(multistream_),
	  streamInfo(streamInfo_)
{
	primaryStatus = new QLabel(this);
	primaryStatus->setWordWrap(true);
	primaryViewers = new QLabel(this);
	primaryViewers->setToolTip(QTStr("Multistream.Viewers.Tooltip"));
	primaryDashboard = CreateDashboardButton(this);
	primaryDetails = new QLabel(this);
	primaryDetails->setProperty("class", "text-muted");
	primaryDetails->setVisible(false);

	QHBoxLayout *primaryLayout = new QHBoxLayout();
	primaryLayout->addWidget(primaryStatus, 1);
	primaryLayout->addWidget(primaryViewers);
	primaryLayout->addWidget(primaryDashboard);

	connect(primaryDashboard, &QPushButton::clicked, this, [this]() { OpenDashboard(kPrimaryStreamChannelId); });

	QWidget *gridWidget = new QWidget(this);
	grid = new QGridLayout(gridWidget);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(0, 1);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(gridWidget);

	emptyLabel = new QLabel(QTStr("Multistream.Dock.Empty"), this);
	emptyLabel->setWordWrap(true);

	startAll = new QPushButton(QTStr("Multistream.Dock.StartAll"), this);
	stopAll = new QPushButton(QTStr("Multistream.Dock.StopAll"), this);
	QPushButton *settings = new QPushButton(QTStr("Settings"), this);

	totalViewers = new QLabel(this);
	totalViewers->setVisible(false);

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(startAll);
	buttons->addWidget(stopAll);
	buttons->addStretch();
	buttons->addWidget(totalViewers);
	buttons->addWidget(settings);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(primaryLayout);
	layout->addWidget(primaryDetails);
	layout->addWidget(emptyLabel);
	layout->addWidget(scroll, 1);
	layout->addLayout(buttons);

	connect(settings, &QPushButton::clicked, this, &MultistreamDock::OpenSettings);
	connect(startAll, &QPushButton::clicked, this, [this]() {
		if (!multistream) {
			return;
		}
		for (const auto &dest : multistream->Destinations()) {
			if (dest.enabled && !multistream->IsActive(dest.id)) {
				multistream->Start(dest.id);
			}
		}
	});
	connect(stopAll, &QPushButton::clicked, this, [this]() {
		if (multistream) {
			multistream->StopAll();
		}
	});

	connect(multistream, &MultistreamOutput::DestinationsChanged, this, &MultistreamDock::Rebuild);
	connect(multistream, &MultistreamOutput::StatusChanged, this,
		[this](const QString &id) { UpdateRow(id.toStdString()); });
	/* The main stream's platform may have changed in Settings > Stream */
	connect(multistream, &MultistreamOutput::PrimaryStateChanged, this, &MultistreamDock::Rebuild);

	if (streamInfo) {
		connect(streamInfo, &StreamInfoFetcher::InfoChanged, this,
			[this](const QString &id) { UpdateInfo(id.toStdString()); });
	}

	statsTimer.setInterval(1000);
	connect(&statsTimer, &QTimer::timeout, this, &MultistreamDock::UpdateStats);

	Rebuild();
}

void MultistreamDock::OpenDashboard(const string &id)
{
	StreamChannel channel;
	if (!FindChannel(multistream, id, channel)) {
		return;
	}
	string videoId = streamInfo ? streamInfo->YouTubeVideoId(id) : string();
	QString url = StreamDashboardUrl(channel.platform, channel.channel, videoId);
	if (!url.isEmpty()) {
		QDesktopServices::openUrl(QUrl(url));
	}
}

void MultistreamDock::UpdateInfo(const string &id)
{
	StreamInfo info = streamInfo ? streamInfo->Info(id) : StreamInfo();

	if (id == kPrimaryStreamChannelId) {
		primaryViewers->setText(ViewersText(info));
		SetDetails(primaryDetails, info);
	} else if (auto it = rows.find(id); it != rows.end()) {
		it->second.viewers->setText(ViewersText(info));
		SetDetails(it->second.details, info);
	}

	/* The total of every channel with a known count */
	int64_t total = 0;
	int counted = 0;
	if (streamInfo && multistream) {
		for (const StreamChannel &channel : GetStreamChannels(multistream)) {
			StreamInfo channelInfo = streamInfo->Info(channel.id);
			if (channelInfo.live && channelInfo.viewers >= 0) {
				total += channelInfo.viewers;
				counted++;
			}
		}
	}
	totalViewers->setText(counted > 1 ? QTStr("Multistream.Viewers.Total").arg(QLocale().toString((qlonglong)total))
					  : QString());
	totalViewers->setVisible(counted > 1);
}

void MultistreamDock::Rebuild()
{
	for (auto &[id, row] : rows) {
		delete row.name;
		delete row.status;
		delete row.stats;
		delete row.viewers;
		delete row.dashboard;
		delete row.toggle;
		delete row.details;
	}
	rows.clear();

	if (!multistream) {
		return;
	}

	int line = 0;
	for (const auto &dest : multistream->Destinations()) {
		Row row;
		QString displayName = QString::fromStdString(dest.name);
		CanvasSlotId canvasId;
		if (CanvasSlotId::FromKey(dest.canvas, canvasId)) {
			displayName += " " + QTStr("Multistream.Canvas.Tag").arg(canvasId.DisplayName());
		}
		row.name = new QLabel(displayName);
		row.status = new QLabel();
		row.stats = new QLabel();
		row.viewers = new QLabel();
		row.viewers->setToolTip(QTStr("Multistream.Viewers.Tooltip"));
		row.dashboard = CreateDashboardButton(nullptr);
		row.toggle = new QPushButton();
		row.details = new QLabel();
		row.details->setProperty("class", "text-muted");
		row.details->setVisible(false);

		if (!dest.enabled) {
			row.name->setToolTip(QTStr("Multistream.Dock.DisabledTooltip"));
			SetLabelClass(row.name, "text-muted");
		}

		/* Dashboard links, chat and stream info are for Twitch, YouTube
		 * and Kick only */
		StreamPlatform platform = StreamPlatformFromId(dest.platform);
		bool supported = platform != StreamPlatform::None;
		row.viewers->setVisible(supported);
		row.dashboard->setVisible(supported);
		row.dashboard->setToolTip(QTStr("Multistream.Dashboard.Tooltip").arg(StreamPlatformName(platform)));

		grid->addWidget(row.name, line, 0);
		grid->addWidget(row.status, line, 1);
		grid->addWidget(row.stats, line, 2);
		grid->addWidget(row.viewers, line, 3);
		grid->addWidget(row.dashboard, line, 4);
		grid->addWidget(row.toggle, line, 5);
		grid->addWidget(row.details, line + 1, 0, 1, 6);

		string id = dest.id;
		connect(row.dashboard, &QPushButton::clicked, this, [this, id]() { OpenDashboard(id); });
		connect(row.toggle, &QPushButton::clicked, this, [this, id]() {
			if (!multistream) {
				return;
			}
			if (multistream->IsActive(id)) {
				/* A second click while stopping (e.g. during a stream
				 * delay) stops immediately, like the main stop button */
				multistream->Stop(id, multistream->IsStopping(id));
			} else {
				multistream->Start(id);
			}
		});

		rows.emplace(id, row);
		UpdateRow(id);
		UpdateInfo(id);
		line += 2;
	}

	emptyLabel->setVisible(rows.empty());

	StreamPlatform primary = ResolvePrimaryStreamPlatform(multistream);
	primaryViewers->setVisible(primary != StreamPlatform::None);
	primaryDashboard->setVisible(primary != StreamPlatform::None);
	primaryDashboard->setToolTip(QTStr("Multistream.Dashboard.Tooltip").arg(StreamPlatformName(primary)));
	UpdateInfo(kPrimaryStreamChannelId);
	UpdatePrimary();
}

void MultistreamDock::UpdatePrimary()
{
	if (!multistream) {
		return;
	}

	bool live = multistream->PrimaryLive();
	primaryStatus->setText(
		QTStr("Multistream.Dock.Primary")
			.arg(live ? QTStr("Multistream.Status.Live") : QTStr("Multistream.Status.Offline")));
	SetLabelClass(primaryStatus, live ? "text-success" : "text-muted");

	bool anyActive = multistream->Active();
	startAll->setEnabled(live && !rows.empty());
	stopAll->setEnabled(anyActive);

	/* Only poll output statistics while something is streaming */
	if (anyActive && !statsTimer.isActive()) {
		statsTimer.start();
	} else if (!anyActive && statsTimer.isActive()) {
		statsTimer.stop();
	}
}

void MultistreamDock::UpdateRow(const string &id)
{
	auto it = rows.find(id);
	if (it == rows.end() || !multistream) {
		return;
	}

	Row &row = it->second;
	MultistreamOutput::Status status = multistream->GetStatus(id);
	bool active = multistream->IsActive(id);

	row.status->setText(StateText(status.state));
	row.status->setToolTip(status.lastError);
	SetLabelClass(row.status, StateClass(status.state));

	if (!active) {
		row.toggle->setText(QTStr("Multistream.Dock.Start"));
	} else if (multistream->IsStopping(id)) {
		row.toggle->setText(QTStr("Multistream.Dock.ForceStop"));
	} else {
		row.toggle->setText(QTStr("Multistream.Dock.Stop"));
	}
	row.toggle->setEnabled(active || multistream->PrimaryLive());

	if (!active) {
		row.stats->clear();
		row.lastBytes = 0;
		row.lastTime = 0;
	}

	UpdatePrimary();
}

void MultistreamDock::UpdateStats()
{
	if (!multistream) {
		return;
	}

	uint64_t now = os_gettime_ns();

	for (auto &[id, row] : rows) {
		if (!multistream->IsActive(id)) {
			continue;
		}

		MultistreamOutput::Status status = multistream->GetStatus(id);

		QString text;
		if (row.lastTime && now > row.lastTime && status.totalBytes >= row.lastBytes) {
			double seconds = double(now - row.lastTime) / 1000000000.0;
			double kbps = double(status.totalBytes - row.lastBytes) * 8.0 / 1000.0 / seconds;
			text = QString("%1 kbps").arg(kbps, 0, 'f', 0);
		}

		if (status.totalFrames > 0) {
			double dropped = 100.0 * status.framesDropped / status.totalFrames;
			text += QString(text.isEmpty() ? "%1% %2" : "  %1% %2")
					.arg(dropped, 0, 'f', 1)
					.arg(QTStr("Multistream.Dock.Dropped"));
		}

		row.stats->setText(text);
		row.lastBytes = status.totalBytes;
		row.lastTime = now;
	}
}

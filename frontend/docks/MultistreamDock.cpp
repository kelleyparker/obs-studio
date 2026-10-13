#include "MultistreamDock.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
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

MultistreamDock::MultistreamDock(MultistreamOutput *multistream_, QWidget *parent)
	: QWidget(parent),
	  multistream(multistream_)
{
	primaryStatus = new QLabel(this);
	primaryStatus->setWordWrap(true);

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

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(startAll);
	buttons->addWidget(stopAll);
	buttons->addStretch();
	buttons->addWidget(settings);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(primaryStatus);
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
	connect(multistream, &MultistreamOutput::PrimaryStateChanged, this, [this]() {
		UpdatePrimary();
		for (const auto &[id, row] : rows) {
			UpdateRow(id);
		}
	});

	connect(&statsTimer, &QTimer::timeout, this, &MultistreamDock::UpdateStats);
	statsTimer.start(1000);

	Rebuild();
}

void MultistreamDock::Rebuild()
{
	for (auto &[id, row] : rows) {
		delete row.name;
		delete row.status;
		delete row.stats;
		delete row.toggle;
	}
	rows.clear();

	if (!multistream) {
		return;
	}

	int line = 0;
	for (const auto &dest : multistream->Destinations()) {
		Row row;
		row.name = new QLabel(QString::fromStdString(dest.name));
		row.status = new QLabel();
		row.stats = new QLabel();
		row.toggle = new QPushButton();

		if (!dest.enabled) {
			row.name->setToolTip(QTStr("Multistream.Dock.DisabledTooltip"));
			SetLabelClass(row.name, "text-muted");
		}

		grid->addWidget(row.name, line, 0);
		grid->addWidget(row.status, line, 1);
		grid->addWidget(row.stats, line, 2);
		grid->addWidget(row.toggle, line, 3);

		string id = dest.id;
		connect(row.toggle, &QPushButton::clicked, this, [this, id]() {
			if (!multistream) {
				return;
			}
			if (multistream->IsActive(id)) {
				multistream->Stop(id);
			} else {
				multistream->Start(id);
			}
		});

		rows.emplace(id, row);
		UpdateRow(id);
		line++;
	}

	emptyLabel->setVisible(rows.empty());
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

	row.toggle->setText(active ? QTStr("Multistream.Dock.Stop") : QTStr("Multistream.Dock.Start"));
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

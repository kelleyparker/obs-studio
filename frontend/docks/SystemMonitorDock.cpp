#include "SystemMonitorDock.hpp"

#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSet>
#include <QThread>
#include <QVBoxLayout>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
/* Meter colors by percent used */
constexpr double kBusyPercent = 60.0;     /* yellow from here */
constexpr double kCriticalPercent = 85.0; /* red from here */
constexpr int kHistory = 60;              /* samples (one a second) drawn behind the bar */

QColor LevelColor(double percent)
{
	if (percent >= kCriticalPercent) {
		return QColor(0xda, 0x36, 0x33);
	}
	if (percent >= kBusyPercent) {
		return QColor(0xd2, 0x99, 0x22);
	}
	return QColor(0x2e, 0xa0, 0x43);
}

int GroupRank(const QString &group)
{
	static const QStringList groups = {"cpu", "memory", "gpu", "disk", "drive", "network"};
	int rank = int(groups.indexOf(group));
	return rank < 0 ? int(groups.size()) : rank;
}

QString Size(double bytes)
{
	double gb = bytes / (1024.0 * 1024.0 * 1024.0);
	return gb >= 1024.0 ? QString("%1 TB").arg(gb / 1024.0, 0, 'f', 2) : QString("%1 GB").arg(gb, 0, 'f', 0);
}
} // namespace

UsageMeter::UsageMeter(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize UsageMeter::sizeHint() const
{
	return QSize(200, 20);
}

void UsageMeter::AddValue(double percent)
{
	history.append(percent);
	while (history.size() > kHistory) {
		history.removeFirst();
	}
	update();
}

void UsageMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	double current = history.isEmpty() ? 0.0 : history.last();

	QPainterPath shape;
	shape.addRoundedRect(area, 3, 3);
	p.setClipPath(shape);
	p.fillPath(shape, QColor(127, 127, 127, 50));

	QColor color = LevelColor(current);
	color.setAlpha(210);
	p.fillRect(QRectF(area.left(), area.top(), area.width() * current / 100.0, area.height()), color);

	/* The last minute, right-aligned so the newest second sits at the end */
	if (history.size() > 1) {
		QColor line = palette().color(QPalette::WindowText);
		line.setAlpha(140);
		p.setPen(QPen(line, 1.2));
		double step = area.width() / (kHistory - 1);
		double x0 = area.right() - step * (history.size() - 1);
		QPolygonF points;
		for (qsizetype i = 0; i < history.size(); i++) {
			double y = area.bottom() - 2.0 - (area.height() - 4.0) * history[i] / 100.0;
			points.append(QPointF(x0 + step * i, y));
		}
		p.drawPolyline(points);
	}

	p.setClipping(false);
	p.setPen(palette().color(QPalette::WindowText));
	p.drawText(rect().adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight,
		   QString("%1%").arg(int(current + 0.5)));
}

SystemMonitorDock::SystemMonitorDock(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outer->addWidget(scroll);

	QWidget *content = new QWidget(scroll);
	QVBoxLayout *contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(8, 8, 8, 8);
	rowsLayout = new QVBoxLayout();
	rowsLayout->setSpacing(8);
	contentLayout->addLayout(rowsLayout);
	waitingLabel = new QLabel("Measuring…", content);
	waitingLabel->setStyleSheet("color: #b8a8c8;");
	contentLayout->addWidget(waitingLabel);
	QLabel *legend = new QLabel(QString("Green under %1%, yellow under %2%, red from %2%. Disks show how busy they "
					    "are (active time); the line behind each bar is the last minute.")
					    .arg(kBusyPercent)
					    .arg(kCriticalPercent),
				    content);
	legend->setWordWrap(true);
	legend->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	contentLayout->addWidget(legend);
	contentLayout->addStretch();
	scroll->setWidget(content);

	qRegisterMetaType<QList<UsageReading>>("QList<UsageReading>");
	thread = new QThread(this);
	thread->setObjectName("System Monitor");
	sampler = new SystemUsageSampler();
	sampler->moveToThread(thread);
	connect(sampler, &SystemUsageSampler::Sampled, this, &SystemMonitorDock::Show);
	thread->start(QThread::LowPriority);
}

SystemMonitorDock::~SystemMonitorDock()
{
	QMetaObject::invokeMethod(sampler, &SystemUsageSampler::Stop, Qt::BlockingQueuedConnection);
	thread->quit();
	thread->wait();
	delete sampler;
}

void SystemMonitorDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	QMetaObject::invokeMethod(sampler, &SystemUsageSampler::Start, Qt::QueuedConnection);
}

void SystemMonitorDock::hideEvent(QHideEvent *event)
{
	/* Closed or tabbed away: nothing is sampled while nobody is looking */
	QMetaObject::invokeMethod(sampler, &SystemUsageSampler::Stop, Qt::QueuedConnection);
	QWidget::hideEvent(event);
}

bool SystemMonitorDock::RecordingDrive(UsageReading &reading) const
{
	OBSBasic *main = OBSBasic::Get();
	const char *path = main ? main->GetCurrentOutputPath() : nullptr;
	if (!path || !*path) {
		return false;
	}
	QString folder = QDir::toNativeSeparators(QString::fromUtf8(path));
	ULARGE_INTEGER free, total;
	if (!GetDiskFreeSpaceExW(reinterpret_cast<const wchar_t *>(folder.utf16()), &free, &total, nullptr) ||
	    !total.QuadPart) {
		return false;
	}
	QString drive = folder.left(2);
	reading = {"drive", "drive", QString("Recording drive (%1)").arg(drive),
		   double(total.QuadPart - free.QuadPart) * 100.0 / double(total.QuadPart),
		   QString("%1 free of %2").arg(Size(double(free.QuadPart)), Size(double(total.QuadPart)))};
	return true;
}

SystemMonitorDock::Row &SystemMonitorDock::RowFor(const UsageReading &reading)
{
	auto it = rows.find(reading.key);
	if (it != rows.end()) {
		return *it;
	}
	Row row;
	row.group = reading.group;
	row.widget = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(row.widget);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);
	QHBoxLayout *header = new QHBoxLayout();
	row.name = new QLabel(row.widget);
	row.name->setStyleSheet("font-weight: bold;");
	row.detail = new QLabel(row.widget);
	row.detail->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	/* Long details give way to the meter instead of widening the dock (the tooltip has all of it) */
	row.detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	row.detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	row.name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	header->addWidget(row.name, 3);
	header->addWidget(row.detail, 2);
	row.meter = new UsageMeter(row.widget);
	layout->addLayout(header);
	layout->addWidget(row.meter);

	/* After the rows of its own group and the groups before it */
	int at = 0;
	for (const QString &key : order) {
		if (GroupRank(rows[key].group) <= GroupRank(reading.group)) {
			at++;
		}
	}
	order.insert(at, reading.key);
	rowsLayout->insertWidget(at, row.widget);
	return *rows.insert(reading.key, row);
}

void SystemMonitorDock::Show(QList<UsageReading> readings)
{
	UsageReading drive;
	if (RecordingDrive(drive)) {
		readings.append(drive);
	}

	QSet<QString> seen;
	for (const UsageReading &reading : readings) {
		Row &row = RowFor(reading);
		row.name->setText(reading.label);
		row.detail->setText(reading.detail);
		row.widget->setToolTip(
			QString("%1: %2%\n%3").arg(reading.label).arg(int(reading.percent + 0.5)).arg(reading.detail));
		row.meter->AddValue(reading.percent);
		seen.insert(reading.key);
	}
	/* A network adapter that disconnected, or a GPU that went away */
	for (qsizetype i = order.size() - 1; i >= 0; i--) {
		if (!seen.contains(order[i])) {
			rows.take(order[i]).widget->deleteLater();
			order.removeAt(i);
		}
	}
	waitingLabel->setVisible(rows.isEmpty());
}

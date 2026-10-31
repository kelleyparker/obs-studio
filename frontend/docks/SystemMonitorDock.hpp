#pragma once

#include <utility/SystemUsage-windows.hpp>

#include <QList>
#include <QMap>
#include <QPointer>
#include <QWidget>

class QLabel;
class QThread;
class QVBoxLayout;

/* A bar for one reading, green, yellow or red by how busy it is, over a faint line of its last minute */
class UsageMeter : public QWidget {
	Q_OBJECT

public:
	explicit UsageMeter(QWidget *parent = nullptr);

	void AddValue(double percent);
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QList<double> history;
};

/* Task Manager at a glance: CPU, memory, each GPU and its VRAM, each disk, the recording drive and the
 * network, sampled once a second while the dock is showing */
class SystemMonitorDock : public QWidget {
	Q_OBJECT

public:
	explicit SystemMonitorDock(QWidget *parent = nullptr);
	~SystemMonitorDock();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	struct Row {
		QWidget *widget = nullptr;
		QLabel *name = nullptr;
		QLabel *detail = nullptr;
		UsageMeter *meter = nullptr;
		QString group;
	};

	QThread *thread;
	SystemUsageSampler *sampler;
	QVBoxLayout *rowsLayout;
	QLabel *waitingLabel;
	QMap<QString, Row> rows;
	QList<QString> order; /* keys in the order they're shown */

	void Show(QList<UsageReading> readings);
	bool RecordingDrive(UsageReading &reading) const;
	Row &RowFor(const UsageReading &reading);
};

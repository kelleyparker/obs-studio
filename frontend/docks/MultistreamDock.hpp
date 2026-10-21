#pragma once

#include <utility/MultistreamOutput.hpp>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <map>
#include <string>

class QGridLayout;
class QLabel;
class QPushButton;
class StreamInfoFetcher;

class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	MultistreamDock(MultistreamOutput *multistream, StreamInfoFetcher *streamInfo, QWidget *parent = nullptr);

signals:
	void OpenSettings();

private:
	struct Row {
		QLabel *name;
		QLabel *status;
		QLabel *stats;
		QLabel *viewers;
		QPushButton *dashboard;
		QPushButton *toggle;
		QLabel *details; /* title and category, on a line of its own */
		uint64_t lastBytes = 0;
		uint64_t lastTime = 0;
	};

	QPointer<MultistreamOutput> multistream;
	QPointer<StreamInfoFetcher> streamInfo;
	QGridLayout *grid;
	QLabel *primaryStatus;
	QLabel *primaryViewers;
	QPushButton *primaryDashboard;
	QLabel *primaryDetails;
	QLabel *totalViewers;
	QLabel *emptyLabel;
	QPushButton *startAll;
	QPushButton *stopAll;
	QTimer statsTimer;
	std::map<std::string, Row> rows;

	void Rebuild();
	void UpdateRow(const std::string &id);
	void UpdateStats();
	void UpdatePrimary();
	void UpdateInfo(const std::string &id);
	void OpenDashboard(const std::string &id);
};

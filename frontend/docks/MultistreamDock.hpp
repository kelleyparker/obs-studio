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

class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	MultistreamDock(MultistreamOutput *multistream, QWidget *parent = nullptr);

signals:
	void OpenSettings();

private:
	struct Row {
		QLabel *name;
		QLabel *status;
		QLabel *stats;
		QPushButton *toggle;
		uint64_t lastBytes = 0;
		uint64_t lastTime = 0;
	};

	QPointer<MultistreamOutput> multistream;
	QGridLayout *grid;
	QLabel *primaryStatus;
	QLabel *emptyLabel;
	QPushButton *startAll;
	QPushButton *stopAll;
	QTimer statsTimer;
	std::map<std::string, Row> rows;

	void Rebuild();
	void UpdateRow(const std::string &id);
	void UpdateStats();
	void UpdatePrimary();
};

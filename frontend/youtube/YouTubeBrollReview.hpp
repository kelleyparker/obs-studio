#pragma once

#include <QGroupBox>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

/* Lists the B-roll moments the pipeline proposed so they can be accepted,
 * rejected or nudged before the final render. The choices are written back
 * into the plan JSON, then the paused pipeline is resumed. */
class YouTubeBrollReview : public QGroupBox {
	Q_OBJECT

public:
	explicit YouTubeBrollReview(QWidget *parent = nullptr);

	void LoadPlan(const QString &planPath, const QString &statePath);

private:
	struct InsertRow {
		QString id;
		QCheckBox *accept = nullptr;
		QDoubleSpinBox *startShift = nullptr;
		QDoubleSpinBox *endShift = nullptr;
	};

	QLabel *summaryLabel = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	QComboBox *styleCombo = nullptr;
	QPushButton *renderBtn = nullptr;
	QPushButton *skipBtn = nullptr;
	QList<InsertRow> rows;

	QString planPath;
	QString statePath;

	void ClearRows();
	bool SavePlan(bool acceptNone);
	void Finish(bool acceptNone);
};

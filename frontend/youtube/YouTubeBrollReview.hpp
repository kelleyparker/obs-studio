#pragma once

#include <QGroupBox>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QVBoxLayout;

/* Lists the B-roll spots the pipeline proposed so they can be accepted,
 * rejected or nudged before the final render: cuts to the screen recording,
 * and AI pictures of what is said. A dial picks how many pictures, each
 * picture has its own prompt and source, and "Make images" makes them while
 * the job keeps waiting. The choices are written back into the plan JSON,
 * then the paused pipeline is resumed. */
class YouTubeBrollReview : public QGroupBox {
	Q_OBJECT

public:
	explicit YouTubeBrollReview(QWidget *parent = nullptr);

	void LoadPlan(const QString &planPath, const QString &statePath);

private:
	struct InsertRow {
		QString id;
		bool picture = false;
		bool hasImage = false;
		double confidence = 0.0;
		double editedStart = 0.0;
		double editedEnd = 0.0;
		QCheckBox *accept = nullptr;
		QDoubleSpinBox *startShift = nullptr;
		QDoubleSpinBox *endShift = nullptr;
		QLineEdit *prompt = nullptr;
		QComboBox *source = nullptr;
		QPushButton *makeBtn = nullptr;
	};

	QLabel *summaryLabel = nullptr;
	QWidget *amountRow = nullptr;
	QSlider *amountSlider = nullptr;
	QLabel *amountName = nullptr;
	QLabel *costLabel = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	QComboBox *styleCombo = nullptr;
	QPushButton *makeAllBtn = nullptr;
	QPushButton *renderBtn = nullptr;
	QPushButton *skipBtn = nullptr;
	QLabel *busyLabel = nullptr;
	QList<InsertRow> rows;

	QString planPath;
	QString statePath;
	double editedTotal = 0.0;
	double maxCoverage = 0.4;
	bool busy = false;

	void ClearRows();
	/* Ticks the pictures the dial picks, as pick_broll_spots in youtube_pipeline.py */
	void PickSpots(int amount);
	/* Ticked pictures with no image yet */
	QList<const InsertRow *> PicturesToMake() const;
	void UpdateSummary();
	void SetBusy(bool busy);
	bool SavePlan(bool acceptNone);
	void MakeImages(const QList<const InsertRow *> &which);
	void Finish(bool acceptNone);
};

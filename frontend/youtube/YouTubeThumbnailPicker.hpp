#pragma once

#include <QGroupBox>
#include <QJsonObject>
#include <QString>

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QWidget;

/* Step mode: choose one of the thumbnail candidates ({stem}_yt_thumbs.json), or tweak the AI images
 * (text, background idea, things to leave out, style, your photo) and make new ones first.
 * Shown in the YouTube dock while the pipeline waits at the thumbnail gate. */
class YouTubeThumbnailPicker : public QGroupBox {
	Q_OBJECT

public:
	explicit YouTubeThumbnailPicker(QWidget *parent = nullptr);

	void LoadCandidates(const QString &thumbsPath, const QString &statePath);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QLabel *infoLabel;
	QLabel *notesLabel;
	QGridLayout *grid;
	QButtonGroup *group;
	QPushButton *useButton;
	QString thumbsPath;
	QString statePath;

	/* Tweaks */
	QWidget *tweakBox;
	QLineEdit *hookEdit;
	QPlainTextEdit *promptEdit;
	QLineEdit *avoidEdit;
	QComboBox *styleCombo;
	QComboBox *personCombo;
	QComboBox *sideCombo;
	QPushButton *attachButton;
	QLabel *photoPreview;
	QString photoPath;
	QPushButton *generateButton;
	QPushButton *refineButton;
	QSlider *strengthSlider;
	QPushButton *recomposeButton;

	/* While new images are being made */
	QWidget *busyRow;
	QProgressBar *busyBar;
	QLabel *busyLabel;

	void Finish();
	void Run(const QString &action);
	QJsonObject Options() const;
	void AttachPhoto();
	void SetPhoto(const QString &path);
	void SetBusy(bool busy);
	void UpdateEtaLabels();
};

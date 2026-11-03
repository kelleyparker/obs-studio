#pragma once

#include <QGroupBox>
#include <QJsonObject>
#include <QString>

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QWidget;

/* Step mode: choose one of the thumbnail candidates ({stem}_yt_thumbs.json), or make new AI images first:
 * pick the image model and write the prompt, attach reference images, and set the text, things to leave out,
 * style and your photo. Shown in the YouTube dock while the pipeline waits at the thumbnail gate. */
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
	/* The image model for this job's next images (starts at the dock's default) */
	QComboBox *modelCombo;
	QLineEdit *hookEdit;
	QPlainTextEdit *promptEdit;
	QPushButton *aiPromptButton;
	QString aiPrompt;
	/* Up to 4 reference images, shown as tiles that remove themselves when clicked */
	QWidget *refsBox;
	QHBoxLayout *refsTiles;
	QPushButton *addRefsButton;
	QSlider *refStrengthSlider;
	QLabel *refsNote;
	QStringList references;
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

	/* The last request, run again with the download allowed when local reference images need IP-Adapter */
	QString lastAction;
	QJsonObject lastOptions;
	QString downloadAction;
	double downloadGb = 0;

	/* While new images are being made */
	QWidget *busyRow;
	QProgressBar *busyBar;
	QLabel *busyLabel;

	void Finish();
	void Run(const QString &action);
	QJsonObject Options() const;
	void AttachPhoto();
	void AddReferences();
	void ShowReferences();
	/* Enables the references for models that can use them and says where they're sent */
	void UpdateModelState();
	QString Source() const;
	void AskToDownload();
	void SetPhoto(const QString &path);
	void SetBusy(bool busy);
	void UpdateEtaLabels();
};

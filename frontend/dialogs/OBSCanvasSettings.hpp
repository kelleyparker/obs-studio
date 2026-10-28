#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class ExtraCanvas;

/* Settings > Vertical Canvas: video, recording and replay buffer settings of
 * the vertical canvas */
class OBSCanvasSettings : public QDialog {
	Q_OBJECT

public:
	OBSCanvasSettings(ExtraCanvas *vertical, bool outputsActive, QWidget *parent);

private:
	ExtraCanvas *vertical;
	bool outputsActive;

	QCheckBox *enabled;
	QComboBox *baseResolution;
	QComboBox *outputResolution;
	QComboBox *scaleType;

	QLineEdit *recPath;
	QComboBox *recFormat;
	QComboBox *encoder;
	QSpinBox *videoBitrate;
	QSpinBox *audioTrack;
	QLineEdit *fileSuffix;
	QSpinBox *replayTime;
	QSpinBox *replaySize;

	void UpdateOutputResolutions();
	void accept() override;
};

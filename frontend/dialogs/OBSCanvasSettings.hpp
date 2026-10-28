#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class ExtraCanvas;

/* Tools > Canvases > <canvas>: video, recording and replay buffer settings of
 * one extra canvas */
class OBSCanvasSettings : public QDialog {
	Q_OBJECT

public:
	OBSCanvasSettings(ExtraCanvas *canvas, bool outputsActive, QWidget *parent);

private:
	ExtraCanvas *canvas;
	bool outputsActive;

	QCheckBox *enabled;
	QCheckBox *recordWithMain;
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

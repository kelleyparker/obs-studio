#include "OBSVerticalSettings.hpp"

#include <utility/VerticalCanvas.hpp>
#include <utility/VerticalOutput.hpp>
#include <utility/audio-encoders.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace std;

static QString ResolutionText(uint32_t cx, uint32_t cy)
{
	return QString("%1x%2").arg(cx).arg(cy);
}

static bool ParseResolution(const QString &text, uint32_t &cx, uint32_t &cy)
{
	static const QRegularExpression re("^\\s*(\\d+)\\s*[xX]\\s*(\\d+)\\s*$");
	QRegularExpressionMatch match = re.match(text);
	if (!match.hasMatch()) {
		return false;
	}

	cx = match.captured(1).toUInt();
	cy = match.captured(2).toUInt();
	return cx >= 32 && cy >= 32 && cx <= 16384 && cy <= 16384;
}

OBSVerticalSettings::OBSVerticalSettings(VerticalCanvas *vertical_, bool outputsActive_, QWidget *parent)
	: QDialog(parent),
	  vertical(vertical_),
	  outputsActive(outputsActive_)
{
	setWindowTitle(QTStr("Vertical.Settings.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	const VerticalVideoSettings &video = vertical->VideoSettings();
	VerticalOutputSettings output = VerticalOutputSettings::Load();

	enabled = new QCheckBox(QTStr("Vertical.Settings.Enable"), this);
	enabled->setChecked(vertical->Enabled());

	QLabel *help = new QLabel(QTStr("Vertical.Settings.Help"), this);
	help->setWordWrap(true);

	/* Video */
	QGroupBox *videoBox = new QGroupBox(QTStr("Basic.Settings.Video"), this);
	QFormLayout *videoForm = new QFormLayout(videoBox);

	baseResolution = new QComboBox(videoBox);
	baseResolution->setEditable(true);
	for (const char *preset : {"1080x1920", "720x1280", "1440x2560", "2160x3840", "1080x1350", "1080x1080"}) {
		baseResolution->addItem(preset);
	}
	baseResolution->setCurrentText(ResolutionText(video.baseWidth, video.baseHeight));
	videoForm->addRow(QTStr("Basic.Settings.Video.BaseResolution"), baseResolution);

	outputResolution = new QComboBox(videoBox);
	outputResolution->setEditable(true);
	videoForm->addRow(QTStr("Basic.Settings.Video.ScaledResolution"), outputResolution);
	UpdateOutputResolutions();
	outputResolution->setCurrentText(ResolutionText(video.outputWidth, video.outputHeight));
	connect(baseResolution, &QComboBox::currentTextChanged, this, &OBSVerticalSettings::UpdateOutputResolutions);

	scaleType = new QComboBox(videoBox);
	scaleType->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Bilinear"), int(OBS_SCALE_BILINEAR));
	scaleType->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Area"), int(OBS_SCALE_AREA));
	scaleType->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Bicubic"), int(OBS_SCALE_BICUBIC));
	scaleType->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Lanczos"), int(OBS_SCALE_LANCZOS));
	scaleType->setCurrentIndex(std::max(0, scaleType->findData(int(video.scaleType))));
	videoForm->addRow(QTStr("Basic.Settings.Video.DownscaleFilter"), scaleType);

	QLabel *fpsNote = new QLabel(QTStr("Vertical.Settings.FPSNote"), videoBox);
	fpsNote->setWordWrap(true);
	videoForm->addRow(fpsNote);

	/* Recording */
	QGroupBox *recBox = new QGroupBox(QTStr("Basic.Settings.Output.Adv.Recording"), this);
	QFormLayout *recForm = new QFormLayout(recBox);

	recPath = new QLineEdit(QString::fromStdString(output.recPath), recBox);
	QPushButton *browse = new QPushButton(QTStr("Browse"), recBox);
	connect(browse, &QPushButton::clicked, this, [this]() {
		QString dir = SelectDirectory(this, QTStr("Basic.Settings.Output.SelectDirectory"), recPath->text());
		if (!dir.isEmpty()) {
			recPath->setText(dir);
		}
	});
	QHBoxLayout *pathLayout = new QHBoxLayout();
	pathLayout->addWidget(recPath, 1);
	pathLayout->addWidget(browse);
	recForm->addRow(QTStr("Basic.Settings.Output.Simple.SavePath"), pathLayout);

	recFormat = new QComboBox(recBox);
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.hMP4"), "hybrid_mp4");
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.MKV"), "mkv");
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.MP4"), "mp4");
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.fMP4"), "fragmented_mp4");
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.MOV"), "mov");
	recFormat->addItem(QTStr("Basic.Settings.Output.Format.TS"), "mpegts");
	recFormat->setCurrentIndex(std::max(0, recFormat->findData(QString::fromStdString(output.recFormat))));
	recForm->addRow(QTStr("Basic.Settings.Output.Format"), recFormat);

	encoder = new QComboBox(recBox);
	encoder->addItem(QTStr("Vertical.Settings.EncoderAuto"), QString());
	for (const auto &item : GetVerticalVideoEncoders()) {
		encoder->addItem(QString::fromStdString(item.second), QString::fromStdString(item.first));
	}
	encoder->setCurrentIndex(std::max(0, encoder->findData(QString::fromStdString(output.encoder))));
	recForm->addRow(QTStr("Basic.Settings.Output.Encoder.Video"), encoder);

	videoBitrate = new QSpinBox(recBox);
	videoBitrate->setRange(200, 200000);
	videoBitrate->setSingleStep(500);
	videoBitrate->setSuffix(" Kbps");
	videoBitrate->setValue(output.videoBitrate);
	recForm->addRow(QTStr("Basic.Settings.Output.VideoBitrate"), videoBitrate);

	audioBitrate = new QComboBox(recBox);
	for (const auto &item : GetSimpleAACEncoderBitrateMap()) {
		audioBitrate->addItem(QString::number(item.first), item.first);
	}
	int closestAudio = FindClosestAvailableSimpleAACBitrate(output.audioBitrate);
	audioBitrate->setCurrentIndex(std::max(0, audioBitrate->findData(closestAudio)));
	recForm->addRow(QTStr("Basic.Settings.Output.AudioBitrate"), audioBitrate);

	audioTrack = new QSpinBox(recBox);
	audioTrack->setRange(1, MAX_AUDIO_MIXES);
	audioTrack->setValue(output.audioTrack);
	recForm->addRow(QTStr("Vertical.Settings.AudioTrack"), audioTrack);

	fileSuffix = new QLineEdit(QString::fromStdString(output.fileSuffix), recBox);
	recForm->addRow(QTStr("Vertical.Settings.FileSuffix"), fileSuffix);

	QLabel *audioNote = new QLabel(QTStr("Vertical.Settings.AudioNote"), recBox);
	audioNote->setWordWrap(true);
	recForm->addRow(audioNote);

	/* Replay buffer */
	QGroupBox *replayBox = new QGroupBox(QTStr("ReplayBuffer"), this);
	QFormLayout *replayForm = new QFormLayout(replayBox);

	replayTime = new QSpinBox(replayBox);
	replayTime->setRange(5, 21600);
	replayTime->setSuffix(" s");
	replayTime->setValue(output.replayTimeSec);
	replayForm->addRow(QTStr("Basic.Settings.Output.ReplayBuffer.SecondsMax"), replayTime);

	replaySize = new QSpinBox(replayBox);
	replaySize->setRange(0, 1024 * 1024);
	replaySize->setSuffix(" MB");
	replaySize->setValue(output.replaySizeMB);
	replayForm->addRow(QTStr("Basic.Settings.Output.ReplayBuffer.MegabytesMax"), replaySize);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &OBSVerticalSettings::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(enabled);
	layout->addWidget(help);

	if (outputsActive) {
		QLabel *activeNote = new QLabel(QTStr("Vertical.Settings.ActiveNote"), this);
		activeNote->setWordWrap(true);
		activeNote->setProperty("class", "text-warning");
		layout->addWidget(activeNote);

		enabled->setEnabled(false);
		videoBox->setEnabled(false);
		recBox->setEnabled(false);
		replayBox->setEnabled(false);
	}

	layout->addWidget(videoBox);
	layout->addWidget(recBox);
	layout->addWidget(replayBox);
	layout->addWidget(buttons);

	resize(560, sizeHint().height());
}

void OBSVerticalSettings::UpdateOutputResolutions()
{
	uint32_t cx, cy;
	if (!ParseResolution(baseResolution->currentText(), cx, cy)) {
		return;
	}

	QString current = outputResolution->currentText();
	outputResolution->clear();

	/* The same downscale ratios as Settings > Video */
	for (double ratio : {1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0}) {
		uint32_t outCX = uint32_t(double(cx) / ratio) & ~1u;
		uint32_t outCY = uint32_t(double(cy) / ratio) & ~1u;
		if (outCX >= 32 && outCY >= 32) {
			outputResolution->addItem(ResolutionText(outCX, outCY));
		}
	}

	uint32_t curCX, curCY;
	if (ParseResolution(current, curCX, curCY) && curCX <= cx && curCY <= cy) {
		outputResolution->setCurrentText(current);
	} else {
		outputResolution->setCurrentIndex(0);
	}
}

void OBSVerticalSettings::accept()
{
	if (outputsActive) {
		QDialog::accept();
		return;
	}

	VerticalVideoSettings video;
	if (!ParseResolution(baseResolution->currentText(), video.baseWidth, video.baseHeight) ||
	    !ParseResolution(outputResolution->currentText(), video.outputWidth, video.outputHeight)) {
		OBSMessageBox::warning(this, QTStr("Vertical.Settings.Title"),
				       QTStr("Vertical.Settings.BadResolution"));
		return;
	}
	video.scaleType = obs_scale_type(scaleType->currentData().toInt());

	VerticalOutputSettings output;
	output.recPath = recPath->text().toStdString();
	output.recFormat = recFormat->currentData().toString().toStdString();
	output.encoder = encoder->currentData().toString().toStdString();
	output.videoBitrate = videoBitrate->value();
	output.audioBitrate = audioBitrate->currentData().toInt();
	output.audioTrack = audioTrack->value();
	output.fileSuffix = fileSuffix->text().trimmed().toStdString();
	output.replayTimeSec = replayTime->value();
	output.replaySizeMB = replaySize->value();
	output.Save();

	vertical->SetVideoSettings(video);
	vertical->SetEnabled(enabled->isChecked());

	QDialog::accept();
}

#include "YouTubeBrollRecorder.hpp"

#include <OBSApp.hpp>
#include <utility/VerticalOutput.hpp>
#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <util/config-file.h>
#include <util/platform.h>

#include <QDateTime>
#include <QDir>

YouTubeBrollRecorder::YouTubeBrollRecorder(QObject *parent) : QObject(parent) {}

YouTubeBrollRecorder::~YouTubeBrollRecorder()
{
	if (recordOutput && obs_output_active(recordOutput)) {
		obs_output_force_stop(recordOutput);
	}
	Release();
}

bool YouTubeBrollRecorder::Start(const QString &sourceName, QString &error)
{
	if (Active()) {
		return true;
	}

	OBSSourceAutoRelease source = obs_get_source_by_name(sourceName.toUtf8().constData());
	if (!source) {
		error = QString("B-roll source '%1' was not found.").arg(sourceName);
		return false;
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "Video is not initialized.";
		return false;
	}

	const char *recDir = OBSBasic::Get()->GetCurrentOutputPath();
	if (!recDir || !*recDir || !QDir(QString::fromUtf8(recDir)).exists()) {
		error = "The recording folder does not exist.";
		return false;
	}
	QString path = QDir(QString::fromUtf8(recDir))
			       .filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") +
					 "_broll_screen.mp4");

	view = obs_view_create();
	obs_view_set_source(view, 0, source);
	viewVideo = obs_view_add2(view, &ovi);
	if (!viewVideo) {
		error = "Could not create a video mix for the B-roll source.";
		Release();
		return false;
	}

	std::string encoderId = GetDefaultVerticalVideoEncoder();
	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", 8000);
	obs_data_set_int(videoSettings, "keyint_sec", 2);
	if (encoderId == "obs_x264") {
		obs_data_set_string(videoSettings, "preset", "veryfast");
	}
	videoEncoder = obs_video_encoder_create(encoderId.c_str(), "youtube_broll_video", videoSettings, nullptr);
	if (!videoEncoder) {
		error = QString("Could not create the '%1' encoder for B-roll.").arg(QString::fromStdString(encoderId));
		Release();
		return false;
	}
	obs_encoder_set_video(videoEncoder, viewVideo);

	int audioBitrate = FindClosestAvailableSimpleAACBitrate(128);
	const char *audioId = GetSimpleAACEncoderForBitrate(audioBitrate);
	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", audioBitrate);
	audioEncoder = audioId ? obs_audio_encoder_create(audioId, "youtube_broll_audio", audioSettings, 0, nullptr)
			       : nullptr;
	if (!audioEncoder) {
		error = "Could not create the B-roll audio encoder.";
		Release();
		return false;
	}
	obs_encoder_set_audio(audioEncoder, obs_get_audio());

	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", path.toUtf8().constData());
	recordOutput = obs_output_create("mp4_output", "youtube_broll_recording", outputSettings, nullptr);
	if (!recordOutput) {
		error = "Could not create the B-roll recording output.";
		Release();
		return false;
	}
	obs_output_set_video_encoder(recordOutput, videoEncoder);
	obs_output_set_audio_encoder(recordOutput, audioEncoder, 0);

	signal_handler_t *handler = obs_output_get_signal_handler(recordOutput);
	recordSignals.emplace_back(handler, "start", RecordStart, this);
	recordSignals.emplace_back(handler, "stop", RecordStop, this);

	startTimeNs = 0;
	if (!obs_output_start(recordOutput)) {
		const char *lastError = obs_output_get_last_error(recordOutput);
		error = lastError ? QString::fromUtf8(lastError) : QString("The B-roll recording failed to start.");
		Release();
		return false;
	}

	lastRecordingPath = path;
	blog(LOG_INFO, "YouTube B-roll: recording source '%s' to '%s' with %s", sourceName.toUtf8().constData(),
	     path.toUtf8().constData(), encoderId.c_str());
	return true;
}

void YouTubeBrollRecorder::Stop()
{
	if (recordOutput && obs_output_active(recordOutput)) {
		obs_output_stop(recordOutput);
	} else if (recordOutput) {
		/* Never got going: nothing will signal "stop" */
		Release();
		emit Stopped(OBS_OUTPUT_SUCCESS);
	}
}

void YouTubeBrollRecorder::Release()
{
	recordSignals.clear();
	recordOutput = nullptr;
	videoEncoder = nullptr;
	audioEncoder = nullptr;
	if (view) {
		obs_view_remove(view);
		obs_view_set_source(view, 0, nullptr);
		view = nullptr;
	}
	viewVideo = nullptr;
}

void YouTubeBrollRecorder::OnStarted(uint64_t timeNs)
{
	startTimeNs = timeNs;
	emit Started();
}

void YouTubeBrollRecorder::OnStopped(int code)
{
	blog(LOG_INFO, "YouTube B-roll: recording stopped (code %d)", code);
	Release();
	emit Stopped(code);
}

void YouTubeBrollRecorder::RecordStart(void *data, calldata_t *)
{
	uint64_t now = os_gettime_ns();
	auto *self = static_cast<YouTubeBrollRecorder *>(data);
	QMetaObject::invokeMethod(self, [self, now]() { self->OnStarted(now); }, Qt::QueuedConnection);
}

void YouTubeBrollRecorder::RecordStop(void *data, calldata_t *params)
{
	auto *self = static_cast<YouTubeBrollRecorder *>(data);
	int code = (int)calldata_int(params, "code");
	QMetaObject::invokeMethod(self, [self, code]() { self->OnStopped(code); }, Qt::QueuedConnection);
}

#include "CanvasOutput.hpp"
#include "ExtraCanvas.hpp"

#include <OBSApp.hpp>
#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <util/config-file.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>

using namespace std;

CanvasOutputSettings CanvasOutputSettings::Load(const CanvasSlotId &slot)
{
	config_t *config = OBSBasic::Get()->Config();
	const string section = slot.ConfigSection();
	const char *Section = section.c_str();

	config_set_default_string(config, Section, "RecFilePath", config_get_string(config, "AdvOut", "RecFilePath"));
	config_set_default_string(config, Section, "RecFormat", "hybrid_mp4");
	config_set_default_string(config, Section, "RecEncoder", "");
	config_set_default_int(config, Section, "VideoBitrate", 8000);
	config_set_default_int(config, Section, "AudioTrack", 1);
	config_set_default_string(config, Section, "FileSuffix", slot.DefaultSuffix().c_str());
	config_set_default_int(config, Section, "RBTime", 30);
	config_set_default_int(config, Section, "RBSize", 512);

	CanvasOutputSettings settings;
	settings.recPath = config_get_string(config, Section, "RecFilePath");
	settings.recFormat = config_get_string(config, Section, "RecFormat");
	settings.encoder = config_get_string(config, Section, "RecEncoder");
	settings.videoBitrate = std::clamp((int)config_get_int(config, Section, "VideoBitrate"), 200, 200000);
	settings.audioBitrate = kFixedAudioBitrateKbps;
	settings.audioTrack = std::clamp((int)config_get_int(config, Section, "AudioTrack"), 1, MAX_AUDIO_MIXES);
	settings.fileSuffix = config_get_string(config, Section, "FileSuffix");
	settings.replayTimeSec = std::clamp((int)config_get_int(config, Section, "RBTime"), 5, 21600);
	settings.replaySizeMB = std::clamp((int)config_get_int(config, Section, "RBSize"), 0, 1024 * 1024);
	return settings;
}

void CanvasOutputSettings::Save(const CanvasSlotId &slot) const
{
	config_t *config = OBSBasic::Get()->Config();
	const string section = slot.ConfigSection();
	const char *Section = section.c_str();

	config_set_string(config, Section, "RecFilePath", recPath.c_str());
	config_set_string(config, Section, "RecFormat", recFormat.c_str());
	config_set_string(config, Section, "RecEncoder", encoder.c_str());
	config_set_int(config, Section, "VideoBitrate", videoBitrate);
	config_set_int(config, Section, "AudioTrack", audioTrack);
	config_set_string(config, Section, "FileSuffix", fileSuffix.c_str());
	config_set_int(config, Section, "RBTime", replayTimeSec);
	config_set_int(config, Section, "RBSize", replaySizeMB);
	config_save_safe(config, "tmp", nullptr);
}

vector<pair<string, string>> GetCanvasVideoEncoders()
{
	vector<pair<string, string>> encoders;
	const char *id;

	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) {
			continue;
		}

		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) {
			continue;
		}

		const char *codec = obs_get_encoder_codec(id);
		if (!codec || (strcmp(codec, "h264") != 0 && strcmp(codec, "hevc") != 0 && strcmp(codec, "av1") != 0)) {
			continue;
		}

		const char *name = obs_encoder_get_display_name(id);
		encoders.emplace_back(id, name ? name : id);
	}

	return encoders;
}

string GetDefaultCanvasVideoEncoder()
{
	/* Prefer hardware H.264 so extra encodes don't add CPU load:
	 * NVIDIA, Intel or AMD on Windows, Apple VideoToolbox on macOS */
	static const char *preferred[] = {"obs_nvenc_h264_tex", "obs_qsv11_v2", "h264_texture_amf",
					  "com.apple.videotoolbox.videoencoder.ave.avc", "obs_x264"};

	vector<pair<string, string>> available = GetCanvasVideoEncoders();
	for (const char *id : preferred) {
		for (const auto &encoder : available) {
			if (encoder.first == id) {
				return id;
			}
		}
	}

	return available.empty() ? string() : available.front().first;
}

static string ResolveEncoder(const CanvasSlotId &slot, const string &selected)
{
	if (!selected.empty()) {
		for (const auto &encoder : GetCanvasVideoEncoders()) {
			if (encoder.first == selected) {
				return selected;
			}
		}
		blog(LOG_WARNING, "%s: encoder '%s' is not available, using the default", slot.CanvasName().c_str(),
		     selected.c_str());
	}
	return GetDefaultCanvasVideoEncoder();
}

CanvasOutput::CanvasOutput(ExtraCanvas *canvas_, QObject *parent)
	: QObject(parent),
	  canvas(canvas_),
	  slot(canvas_->Slot())
{
	/* Outputs can't keep running once the canvas or its video mix is gone */
	connect(canvas_, &ExtraCanvas::CanvasClearing, this, [this]() { StopAll(true); });
}

CanvasOutput::~CanvasOutput()
{
	StopAll(true);
}

bool CanvasOutput::AcquireEncoders(const CanvasOutputSettings &settings, QString &error)
{
	/* Encoders are shared by the recording and the replay buffer. While
	 * either is running, its encoders can't be changed. */
	if (videoEncoder && audioEncoder) {
		return true;
	}

	video_t *video = canvas ? canvas->GetVideo() : nullptr;
	if (!video) {
		error = QTStr("Canvas.Error.NotEnabled").arg(slot.DisplayName());
		return false;
	}

	string encoderId = ResolveEncoder(slot, settings.encoder);
	if (encoderId.empty()) {
		error = QTStr("Canvas.Error.NoEncoder");
		return false;
	}

	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", settings.videoBitrate);
	obs_data_set_int(videoSettings, "keyint_sec", 2);
	if (encoderId == "obs_x264") {
		obs_data_set_string(videoSettings, "preset", "veryfast");
	}

	const string prefix = slot.OutputPrefix();
	videoEncoder = obs_video_encoder_create(encoderId.c_str(), (prefix + "_video").c_str(), videoSettings, nullptr);
	if (!videoEncoder) {
		error = QTStr("Canvas.Error.EncoderFailed").arg(QString::fromStdString(encoderId));
		return false;
	}
	obs_encoder_set_video(videoEncoder, video);

	int audioBitrate = FindClosestAvailableSimpleAACBitrate(settings.audioBitrate);
	const char *audioId = GetSimpleAACEncoderForBitrate(audioBitrate);
	if (!audioId) {
		videoEncoder = nullptr;
		error = QTStr("Canvas.Error.NoEncoder");
		return false;
	}

	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", audioBitrate);
	audioEncoder = obs_audio_encoder_create(audioId, (prefix + "_audio").c_str(), audioSettings,
						settings.audioTrack - 1, nullptr);
	if (!audioEncoder) {
		videoEncoder = nullptr;
		error = QTStr("Canvas.Error.EncoderFailed").arg(QString::fromUtf8(audioId));
		return false;
	}
	obs_encoder_set_audio(audioEncoder, obs_get_audio());

	blog(LOG_INFO, "%s: using video encoder '%s' at %d kbps, audio track %d", slot.CanvasName().c_str(),
	     encoderId.c_str(), settings.videoBitrate, settings.audioTrack);
	return true;
}

void CanvasOutput::ReleaseIdle()
{
	if (!RecordingActive()) {
		recordSignals.clear();
		recordOutput = nullptr;
	}
	if (!ReplayBufferActive()) {
		replaySignals.clear();
		replayOutput = nullptr;
	}
	if (!recordOutput && !replayOutput) {
		videoEncoder = nullptr;
		audioEncoder = nullptr;
	}
}

static void AddMuxerSettings(obs_data_t *settings, const string &format)
{
	/* Same as the main recording: fragmented MP4/MOV needs movflags */
	if (format.rfind("fragmented", 0) == 0) {
		obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");
	}
}

bool CanvasOutput::StartRecording(QString &error)
{
	if (RecordingActive()) {
		return true;
	}

	ReleaseIdle();

	CanvasOutputSettings settings = CanvasOutputSettings::Load(slot);
	config_t *config = OBSBasic::Get()->Config();
	const char *filenameFormat = config_get_string(config, "Output", "FilenameFormatting");
	bool overwrite = config_get_bool(config, "Output", "OverwriteIfExists");
	bool noSpace = config_get_bool(config, "AdvOut", "RecFileNameWithoutSpace");

	string format = GetFormatString(filenameFormat, nullptr, settings.fileSuffix.c_str());
	string path = GetOutputFilename(settings.recPath.c_str(), settings.recFormat.c_str(), noSpace, overwrite,
					format.c_str());
	if (path.empty()) {
		/* GetOutputFilename() has already told the user */
		error.clear();
		return false;
	}

	if (!AcquireEncoders(settings, error)) {
		return false;
	}

	const char *muxer = "ffmpeg_muxer";
	if (settings.recFormat == "hybrid_mp4") {
		muxer = "mp4_output";
	} else if (settings.recFormat == "hybrid_mov") {
		muxer = "mov_output";
	}

	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", path.c_str());
	AddMuxerSettings(outputSettings, settings.recFormat);

	recordOutput = obs_output_create(muxer, (slot.OutputPrefix() + "_recording").c_str(), outputSettings, nullptr);
	if (!recordOutput) {
		ReleaseIdle();
		error = QTStr("Output.StartFailedGeneric");
		return false;
	}

	obs_output_set_video_encoder(recordOutput, videoEncoder);
	obs_output_set_audio_encoder(recordOutput, audioEncoder, 0);

	signal_handler_t *handler = obs_output_get_signal_handler(recordOutput);
	recordSignals.emplace_back(handler, "start", RecordStart, this);
	recordSignals.emplace_back(handler, "stop", RecordStop, this);

	if (!obs_output_start(recordOutput)) {
		const char *lastError = obs_output_get_last_error(recordOutput);
		error = lastError ? QString::fromUtf8(lastError) : QTStr("Output.StartFailedGeneric");
		recordSignals.clear();
		recordOutput = nullptr;
		ReleaseIdle();
		return false;
	}

	lastRecordingPath = QString::fromStdString(path);
	blog(LOG_INFO, "%s: recording to '%s'", slot.CanvasName().c_str(), path.c_str());
	return true;
}

void CanvasOutput::StopRecording(bool force)
{
	if (!recordOutput || !obs_output_active(recordOutput)) {
		return;
	}
	if (force) {
		obs_output_force_stop(recordOutput);
	} else {
		obs_output_stop(recordOutput);
	}
}

bool CanvasOutput::RecordingActive() const
{
	return recordOutput && obs_output_active(recordOutput);
}

uint64_t CanvasOutput::RecordingSeconds() const
{
	if (!RecordingActive() || !recordStartTime) {
		return 0;
	}
	return (os_gettime_ns() - recordStartTime) / 1000000000ULL;
}

bool CanvasOutput::StartReplayBuffer(QString &error)
{
	if (ReplayBufferActive()) {
		return true;
	}

	ReleaseIdle();

	CanvasOutputSettings settings = CanvasOutputSettings::Load(slot);
	config_t *config = OBSBasic::Get()->Config();
	const char *filenameFormat = config_get_string(config, "Output", "FilenameFormatting");
	bool noSpace = config_get_bool(config, "AdvOut", "RecFileNameWithoutSpace");

	if (settings.recFormat == "hybrid_mp4" || settings.recFormat == "hybrid_mov") {
		/* The replay buffer is always written by FFmpeg */
		settings.recFormat = settings.recFormat == "hybrid_mp4" ? "mp4" : "mov";
	}

	os_dir_t *dir = settings.recPath.empty() ? nullptr : os_opendir(settings.recPath.c_str());
	if (!dir) {
		error = QTStr("Output.BadPath.Text");
		return false;
	}
	os_closedir(dir);

	if (!AcquireEncoders(settings, error)) {
		return false;
	}

	string format = GetFormatString(filenameFormat, "Replay", settings.fileSuffix.c_str());
	string ext = GetFormatExt(settings.recFormat.c_str());

	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "directory", settings.recPath.c_str());
	obs_data_set_string(outputSettings, "format", format.c_str());
	obs_data_set_string(outputSettings, "extension", ext.c_str());
	obs_data_set_bool(outputSettings, "allow_spaces", !noSpace);
	obs_data_set_int(outputSettings, "max_time_sec", settings.replayTimeSec);
	obs_data_set_int(outputSettings, "max_size_mb", settings.replaySizeMB);
	AddMuxerSettings(outputSettings, settings.recFormat);

	replayOutput = obs_output_create("replay_buffer", (slot.OutputPrefix() + "_replay_buffer").c_str(),
					 outputSettings, nullptr);
	if (!replayOutput) {
		ReleaseIdle();
		error = QTStr("Output.StartFailedGeneric");
		return false;
	}

	obs_output_set_video_encoder(replayOutput, videoEncoder);
	obs_output_set_audio_encoder(replayOutput, audioEncoder, 0);

	signal_handler_t *handler = obs_output_get_signal_handler(replayOutput);
	replaySignals.emplace_back(handler, "start", ReplayStart, this);
	replaySignals.emplace_back(handler, "stop", ReplayStop, this);
	replaySignals.emplace_back(handler, "saved", ReplaySavedSignal, this);

	if (!obs_output_start(replayOutput)) {
		const char *lastError = obs_output_get_last_error(replayOutput);
		error = lastError ? QString::fromUtf8(lastError) : QTStr("Output.StartFailedGeneric");
		replaySignals.clear();
		replayOutput = nullptr;
		ReleaseIdle();
		return false;
	}

	return true;
}

void CanvasOutput::StopReplayBuffer(bool force)
{
	if (!replayOutput || !obs_output_active(replayOutput)) {
		return;
	}
	if (force) {
		obs_output_force_stop(replayOutput);
	} else {
		obs_output_stop(replayOutput);
	}
}

bool CanvasOutput::ReplayBufferActive() const
{
	return replayOutput && obs_output_active(replayOutput);
}

void CanvasOutput::SaveReplay()
{
	if (!ReplayBufferActive()) {
		return;
	}

	calldata_t cd = {};
	proc_handler_t *ph = obs_output_get_proc_handler(replayOutput);
	proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
}

void CanvasOutput::StopAll(bool force)
{
	StopRecording(force);
	StopReplayBuffer(force);
}

void CanvasOutput::OnRecordingStarted()
{
	recordStartTime = os_gettime_ns();
	emit RecordingStarted();
}

void CanvasOutput::OnRecordingStopped(int code, const QString &error)
{
	recordStartTime = 0;
	blog(LOG_INFO, "%s: recording stopped (code %d)", slot.CanvasName().c_str(), code);
	ReleaseIdle();
	emit RecordingStopped(code, error);
}

void CanvasOutput::OnReplayBufferStarted()
{
	emit ReplayBufferStarted();
}

void CanvasOutput::OnReplayBufferStopped(int code, const QString &error)
{
	blog(LOG_INFO, "%s: replay buffer stopped (code %d)", slot.CanvasName().c_str(), code);
	ReleaseIdle();
	emit ReplayBufferStopped(code, error);
}

void CanvasOutput::OnReplaySaved()
{
	if (!replayOutput) {
		return;
	}

	calldata_t cd = {};
	proc_handler_t *ph = obs_output_get_proc_handler(replayOutput);
	proc_handler_call(ph, "get_last_replay", &cd);
	QString path = QString::fromUtf8(calldata_string(&cd, "path"));
	calldata_free(&cd);

	blog(LOG_INFO, "%s: replay saved to '%s'", slot.CanvasName().c_str(), path.toUtf8().constData());
	emit ReplaySaved(path);
}

static QString LastError(calldata_t *params)
{
	obs_output_t *output = static_cast<obs_output_t *>(calldata_ptr(params, "output"));
	const char *error = output ? obs_output_get_last_error(output) : nullptr;
	return error ? QString::fromUtf8(error) : QString();
}

void CanvasOutput::RecordStart(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<CanvasOutput *>(data), &CanvasOutput::OnRecordingStarted,
				  Qt::QueuedConnection);
}

void CanvasOutput::RecordStop(void *data, calldata_t *params)
{
	CanvasOutput *self = static_cast<CanvasOutput *>(data);
	int code = (int)calldata_int(params, "code");
	QString error = LastError(params);
	QMetaObject::invokeMethod(
		self, [self, code, error]() { self->OnRecordingStopped(code, error); }, Qt::QueuedConnection);
}

void CanvasOutput::ReplayStart(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<CanvasOutput *>(data), &CanvasOutput::OnReplayBufferStarted,
				  Qt::QueuedConnection);
}

void CanvasOutput::ReplayStop(void *data, calldata_t *params)
{
	CanvasOutput *self = static_cast<CanvasOutput *>(data);
	int code = (int)calldata_int(params, "code");
	QString error = LastError(params);
	QMetaObject::invokeMethod(
		self, [self, code, error]() { self->OnReplayBufferStopped(code, error); }, Qt::QueuedConnection);
}

void CanvasOutput::ReplaySavedSignal(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<CanvasOutput *>(data), &CanvasOutput::OnReplaySaved,
				  Qt::QueuedConnection);
}

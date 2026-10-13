#include "MultistreamOutput.hpp"

#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QUuid>

#include <util/config-file.h>

using namespace std;

constexpr string_view MultistreamFileName = "multistream.json";
constexpr int MultistreamKeyframeIntervalSec = 2;

static const char *ModeToString(MultistreamMode mode)
{
	return mode == MultistreamMode::Separate ? "separate" : "shared";
}

static MultistreamMode ModeFromString(const char *mode)
{
	return strcmp(mode, "separate") == 0 ? MultistreamMode::Separate : MultistreamMode::Shared;
}

const vector<MultistreamPlatformPreset> &GetMultistreamPlatformPresets()
{
	static const vector<MultistreamPlatformPreset> presets = {
		{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "https://studio.youtube.com/", 6000},
		{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "https://dashboard.twitch.tv/settings/stream", 6000},
		{"kick", "Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app",
		 "https://dashboard.kick.com/channel/stream", 8000},
		{"custom", "Custom RTMP", "", "", 6000},
	};
	return presets;
}

const MultistreamPlatformPreset *FindMultistreamPlatformPreset(const string &id)
{
	for (const auto &preset : GetMultistreamPlatformPresets()) {
		if (id == preset.id) {
			return &preset;
		}
	}
	return nullptr;
}

static string HostFromUrl(const string &url)
{
	size_t start = url.find("://");
	start = start == string::npos ? 0 : start + 3;
	size_t end = url.find_first_of(":/", start);
	return url.substr(start, end == string::npos ? string::npos : end - start);
}

MultistreamOutput::MultistreamOutput(QObject *parent) : QObject(parent) {}

MultistreamOutput::~MultistreamOutput()
{
	StopAll(true);
	running.clear();
}

string MultistreamOutput::GenerateId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

void MultistreamOutput::Load(const filesystem::path &profilePath)
{
	configPath = profilePath / filesystem::u8path(MultistreamFileName);

	enabled = false;
	mode = MultistreamMode::Shared;
	destinations.clear();

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(configPath.u8string().c_str(), "bak");
	if (data) {
		obs_data_set_default_string(data, "mode", "shared");
		enabled = obs_data_get_bool(data, "enabled");
		mode = ModeFromString(obs_data_get_string(data, "mode"));

		OBSDataArrayAutoRelease array = obs_data_get_array(data, "destinations");
		size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			obs_data_set_default_bool(item, "enabled", true);
			obs_data_set_default_int(item, "video_bitrate", 6000);
			obs_data_set_default_int(item, "audio_bitrate", 160);
			obs_data_set_default_string(item, "platform", "custom");

			MultistreamDestination dest;
			dest.id = obs_data_get_string(item, "id");
			dest.name = obs_data_get_string(item, "name");
			dest.platform = obs_data_get_string(item, "platform");
			dest.server = obs_data_get_string(item, "server");
			dest.key = obs_data_get_string(item, "key");
			dest.enabled = obs_data_get_bool(item, "enabled");
			dest.videoBitrate = (int)obs_data_get_int(item, "video_bitrate");
			dest.audioBitrate = (int)obs_data_get_int(item, "audio_bitrate");
			dest.encoder = obs_data_get_string(item, "encoder");
			dest.width = (uint32_t)obs_data_get_int(item, "width");
			dest.height = (uint32_t)obs_data_get_int(item, "height");

			if (dest.id.empty()) {
				dest.id = GenerateId();
			}
			destinations.push_back(std::move(dest));
		}
	}

	states.clear();
	emit DestinationsChanged();
}

bool MultistreamOutput::Save() const
{
	if (configPath.empty()) {
		return false;
	}

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "mode", ModeToString(mode));

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const auto &dest : destinations) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", dest.id.c_str());
		obs_data_set_string(item, "name", dest.name.c_str());
		obs_data_set_string(item, "platform", dest.platform.c_str());
		obs_data_set_string(item, "server", dest.server.c_str());
		obs_data_set_string(item, "key", dest.key.c_str());
		obs_data_set_bool(item, "enabled", dest.enabled);
		obs_data_set_int(item, "video_bitrate", dest.videoBitrate);
		obs_data_set_int(item, "audio_bitrate", dest.audioBitrate);
		obs_data_set_string(item, "encoder", dest.encoder.c_str());
		obs_data_set_int(item, "width", dest.width);
		obs_data_set_int(item, "height", dest.height);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(data, "destinations", array);

	if (!obs_data_save_json_safe(data, configPath.u8string().c_str(), "tmp", "bak")) {
		blog(LOG_WARNING, "[Multistream] Failed to save multistream settings");
		return false;
	}
	return true;
}

void MultistreamOutput::SetDestinations(vector<MultistreamDestination> newDestinations)
{
	destinations = std::move(newDestinations);

	for (auto it = states.begin(); it != states.end();) {
		if (!FindDestination(it->first) && !IsActive(it->first)) {
			it = states.erase(it);
		} else {
			++it;
		}
	}

	emit DestinationsChanged();
}

const MultistreamDestination *MultistreamOutput::FindDestination(const string &id) const
{
	for (const auto &dest : destinations) {
		if (dest.id == id) {
			return &dest;
		}
	}
	return nullptr;
}

void MultistreamOutput::PrimaryStarted(obs_output_t *primary, bool sharedEncodersAllowed)
{
	primaryOutput = obs_output_get_weak_output(primary);
	primarySharedEncoders = sharedEncodersAllowed;
	emit PrimaryStateChanged(true);

	if (!enabled) {
		return;
	}

	for (const auto &dest : destinations) {
		if (dest.enabled) {
			Start(dest.id);
		}
	}
}

void MultistreamOutput::PrimaryStopped(bool force)
{
	StopAll(force);
	primaryOutput = nullptr;
	emit PrimaryStateChanged(false);
}

obs_encoder_t *MultistreamOutput::CreateAudioEncoder(const string &name, int bitrate, size_t mixer)
{
	bitrate = FindClosestAvailableSimpleAACBitrate(bitrate);
	const char *id = GetSimpleAACEncoderForBitrate(bitrate);
	if (!id) {
		blog(LOG_WARNING, "[Multistream] No AAC encoder available");
		return nullptr;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", bitrate);

	obs_encoder_t *encoder = obs_audio_encoder_create(id, name.c_str(), settings, mixer, nullptr);
	if (encoder) {
		obs_encoder_set_audio(encoder, obs_get_audio());
	}
	return encoder;
}

obs_encoder_t *MultistreamOutput::GetSharedAudioEncoder(obs_output_t *primary)
{
	obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);

	/* RTMP destinations need AAC; reuse the main stream's encoder if it
	 * already is AAC, otherwise (e.g. Opus for WHIP) encode AAC once and
	 * share it between all destinations. */
	if (primaryAudio && strcmp(obs_encoder_get_codec(primaryAudio), "aac") == 0) {
		return primaryAudio;
	}

	if (!sharedAudioEncoder) {
		size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;
		int bitrate = 160;
		if (primaryAudio) {
			OBSDataAutoRelease settings = obs_encoder_get_settings(primaryAudio);
			int primaryBitrate = (int)obs_data_get_int(settings, "bitrate");
			if (primaryBitrate > 0) {
				bitrate = primaryBitrate;
			}
		}
		sharedAudioEncoder = CreateAudioEncoder("multistream_shared_aac", bitrate, mixer);
	}

	return sharedAudioEncoder;
}

obs_encoder_t *MultistreamOutput::CreateVideoEncoder(const MultistreamDestination &dest, obs_output_t *primary)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	bool primaryIsH264 = primaryVideo && strcmp(obs_encoder_get_codec(primaryVideo), "h264") == 0;

	string encoderId = dest.encoder;
	if (encoderId.empty()) {
		encoderId = primaryIsH264 ? obs_encoder_get_id(primaryVideo) : "obs_x264";
	}

	OBSDataAutoRelease settings = obs_data_create();

	/* Start from the main stream's settings (preset, profile, tuning, ...)
	 * when the destination uses the same encoder type. */
	if (primaryVideo && encoderId == obs_encoder_get_id(primaryVideo)) {
		OBSDataAutoRelease primarySettings = obs_encoder_get_settings(primaryVideo);
		obs_data_apply(settings, primarySettings);
	}

	obs_data_set_int(settings, "bitrate", dest.videoBitrate);
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "keyint_sec", MultistreamKeyframeIntervalSec);

	string name = "multistream_video_" + dest.name;
	obs_encoder_t *encoder = obs_video_encoder_create(encoderId.c_str(), name.c_str(), settings, nullptr);
	if (!encoder) {
		blog(LOG_WARNING, "[Multistream] Failed to create video encoder '%s' for '%s'", encoderId.c_str(),
		     dest.name.c_str());
		return nullptr;
	}

	if (dest.width && dest.height) {
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi) || ovi.output_width != dest.width || ovi.output_height != dest.height) {
			obs_encoder_set_scaled_size(encoder, dest.width, dest.height);
		}
	}

	obs_encoder_set_video(encoder, obs_get_video());
	return encoder;
}

bool MultistreamOutput::Start(const string &id)
{
	const MultistreamDestination *dest = FindDestination(id);
	if (!dest || IsActive(id)) {
		return false;
	}

	OBSOutputAutoRelease primary = obs_weak_output_get_output(primaryOutput);
	if (!primary) {
		blog(LOG_WARNING, "[Multistream] Cannot start '%s': the main stream is not live", dest->name.c_str());
		return false;
	}

	if (dest->server.empty() || dest->key.empty()) {
		SetState(id, MultistreamState::Error, QTStr("Multistream.Error.MissingServerOrKey"));
		return false;
	}

	auto run = make_unique<Running>();
	run->context = make_unique<SignalContext>(SignalContext{this, id});

	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", dest->server.c_str());
	obs_data_set_string(serviceSettings, "key", dest->key.c_str());

	string serviceName = "multistream_service_" + dest->name;
	run->service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);

	string outputName = "multistream_" + dest->name;
	run->output = obs_output_create("rtmp_output", outputName.c_str(), nullptr, nullptr);
	if (!run->service || !run->output) {
		SetState(id, MultistreamState::Error, QTStr("Multistream.Error.CreateFailed"));
		return false;
	}

	bool useShared = mode == MultistreamMode::Shared && primarySharedEncoders &&
			 obs_output_get_video_encoder(primary) != nullptr;

	obs_encoder_t *videoEncoder = nullptr;
	obs_encoder_t *audioEncoder = nullptr;

	if (useShared) {
		videoEncoder = obs_output_get_video_encoder(primary);
		audioEncoder = GetSharedAudioEncoder(primary);

		if (strcmp(obs_encoder_get_codec(videoEncoder), "h264") != 0) {
			blog(LOG_WARNING,
			     "[Multistream] '%s' is using the shared %s encoder; some platforms only accept H.264",
			     dest->name.c_str(), obs_encoder_get_codec(videoEncoder));
		}
	} else {
		run->videoEncoder = CreateVideoEncoder(*dest, primary);

		obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);
		size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;
		string audioName = "multistream_audio_" + dest->name;
		run->audioEncoder = CreateAudioEncoder(audioName, dest->audioBitrate, mixer);

		videoEncoder = run->videoEncoder;
		audioEncoder = run->audioEncoder;
	}

	if (!videoEncoder || !audioEncoder) {
		SetState(id, MultistreamState::Error, QTStr("Multistream.Error.CreateFailed"));
		return false;
	}

	obs_output_set_video_encoder(run->output, videoEncoder);
	obs_output_set_audio_encoder(run->output, audioEncoder, 0);
	obs_output_set_service(run->output, run->service);

	/* Use the same network settings as the main stream */
	config_t *config = OBSBasic::Get()->Config();
	bool reconnect = config_get_bool(config, "Output", "Reconnect");
	int retryDelay = (int)config_get_uint(config, "Output", "RetryDelay");
	int maxRetries = (int)config_get_uint(config, "Output", "MaxRetries");
	bool useDelay = config_get_bool(config, "Output", "DelayEnable");
	int delaySec = (int)config_get_int(config, "Output", "DelaySec");
	bool preserveDelay = config_get_bool(config, "Output", "DelayPreserve");

	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "bind_ip", config_get_string(config, "Output", "BindIP"));
	obs_data_set_string(outputSettings, "ip_family", config_get_string(config, "Output", "IPFamily"));
#ifdef _WIN32
	obs_data_set_bool(outputSettings, "new_socket_loop_enabled",
			  config_get_bool(config, "Output", "NewSocketLoopEnable"));
	obs_data_set_bool(outputSettings, "low_latency_mode_enabled",
			  config_get_bool(config, "Output", "LowLatencyEnable"));
#endif
	obs_data_set_bool(outputSettings, "dyn_bitrate", config_get_bool(config, "Output", "DynamicBitrate"));
	obs_output_update(run->output, outputSettings);

	obs_output_set_delay(run->output, useDelay ? delaySec : 0, preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);
	obs_output_set_reconnect_settings(run->output, reconnect ? maxRetries : 0, retryDelay);

	signal_handler_t *handler = obs_output_get_signal_handler(run->output);
	run->startSignal.Connect(handler, "start", OBSOutputStart, run->context.get());
	run->stopSignal.Connect(handler, "stop", OBSOutputStop, run->context.get());
	run->reconnectSignal.Connect(handler, "reconnect", OBSOutputReconnect, run->context.get());
	run->reconnectSuccessSignal.Connect(handler, "reconnect_success", OBSOutputReconnectSuccess,
					    run->context.get());

	blog(LOG_INFO, "[Multistream] Starting '%s' (%s) -> %s using %s encoders", dest->name.c_str(),
	     dest->platform.c_str(), HostFromUrl(dest->server).c_str(), useShared ? "shared" : "separate");

	SetState(id, MultistreamState::Connecting);

	obs_output_t *output = run->output;
	running[id] = std::move(run);

	if (!obs_output_start(output)) {
		const char *error = obs_output_get_last_error(output);
		QString message = error && *error ? QString::fromUtf8(error) : QTStr("Output.StartFailedGeneric");
		blog(LOG_WARNING, "[Multistream] '%s' failed to start: %s", dest->name.c_str(), QT_TO_UTF8(message));
		running.erase(id);
		SetState(id, MultistreamState::Error, message);
		return false;
	}

	return true;
}

void MultistreamOutput::Stop(const string &id, bool force)
{
	auto it = running.find(id);
	if (it == running.end()) {
		return;
	}

	/* A second graceful stop would cut a stream delay short */
	if (!force && it->second->stopRequested) {
		return;
	}
	it->second->stopRequested = true;

	obs_output_t *output = it->second->output;
	if (force) {
		obs_output_force_stop(output);
	} else {
		obs_output_stop(output);
	}
}

void MultistreamOutput::StopAll(bool force)
{
	vector<string> ids;
	for (const auto &[id, run] : running) {
		ids.push_back(id);
	}
	for (const auto &id : ids) {
		Stop(id, force);
	}
}

bool MultistreamOutput::Active() const
{
	return !running.empty();
}

bool MultistreamOutput::IsActive(const string &id) const
{
	return running.find(id) != running.end();
}

MultistreamOutput::Status MultistreamOutput::GetStatus(const string &id) const
{
	Status status;

	auto stateIt = states.find(id);
	if (stateIt != states.end()) {
		status.state = stateIt->second.state;
		status.lastError = stateIt->second.lastError;
	}

	auto runIt = running.find(id);
	if (runIt != running.end()) {
		obs_output_t *output = runIt->second->output;
		status.totalBytes = obs_output_get_total_bytes(output);
		status.framesDropped = obs_output_get_frames_dropped(output);
		status.totalFrames = obs_output_get_total_frames(output);
	}

	return status;
}

void MultistreamOutput::SetState(const string &id, MultistreamState state, const QString &lastError)
{
	StateInfo &info = states[id];
	info.state = state;
	info.lastError = lastError;
	emit StatusChanged(QString::fromStdString(id));
}

void MultistreamOutput::OnStarted(const string &id)
{
	if (IsActive(id)) {
		SetState(id, MultistreamState::Live);
	}
}

void MultistreamOutput::OnStopped(const string &id, int code, const QString &lastError)
{
	auto it = running.find(id);
	if (it == running.end()) {
		return;
	}

	running.erase(it);

	if (running.empty()) {
		sharedAudioEncoder = nullptr;
	}

	const MultistreamDestination *dest = FindDestination(id);
	const char *name = dest ? dest->name.c_str() : id.c_str();

	if (code == OBS_OUTPUT_SUCCESS) {
		blog(LOG_INFO, "[Multistream] '%s' stopped", name);
		SetState(id, MultistreamState::Offline);
		return;
	}

	QString message = lastError;
	if (message.isEmpty()) {
		switch (code) {
		case OBS_OUTPUT_BAD_PATH:
			message = QTStr("Output.ConnectFail.BadPath");
			break;
		case OBS_OUTPUT_CONNECT_FAILED:
			message = QTStr("Output.ConnectFail.ConnectFailed");
			break;
		case OBS_OUTPUT_INVALID_STREAM:
			message = QTStr("Output.ConnectFail.InvalidStream");
			break;
		case OBS_OUTPUT_DISCONNECTED:
			message = QTStr("Output.ConnectFail.Disconnected");
			break;
		default:
			message = QTStr("Output.ConnectFail.Error");
			break;
		}
	}

	blog(LOG_WARNING, "[Multistream] '%s' stopped with error %d: %s", name, code, QT_TO_UTF8(message));
	SetState(id, MultistreamState::Error, message);
}

void MultistreamOutput::OnReconnect(const string &id)
{
	if (IsActive(id)) {
		SetState(id, MultistreamState::Reconnecting);
	}
}

void MultistreamOutput::OnReconnectSuccess(const string &id)
{
	if (IsActive(id)) {
		SetState(id, MultistreamState::Live);
	}
}

/* libobs signal callbacks may run on output threads or synchronously inside
 * obs_output_stop()/obs_output_force_stop(); always queue to the UI thread so
 * the output is never released while libobs is still using it. */

void MultistreamOutput::OBSOutputStart(void *data, calldata_t *)
{
	auto ctx = static_cast<SignalContext *>(data);
	QMetaObject::invokeMethod(
		ctx->self, [self = ctx->self, id = ctx->id]() { self->OnStarted(id); }, Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputStop(void *data, calldata_t *params)
{
	auto ctx = static_cast<SignalContext *>(data);
	int code = (int)calldata_int(params, "code");
	QString lastError = QString::fromUtf8(calldata_string(params, "last_error"));
	QMetaObject::invokeMethod(
		ctx->self,
		[self = ctx->self, id = ctx->id, code, lastError]() { self->OnStopped(id, code, lastError); },
		Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputReconnect(void *data, calldata_t *)
{
	auto ctx = static_cast<SignalContext *>(data);
	QMetaObject::invokeMethod(
		ctx->self, [self = ctx->self, id = ctx->id]() { self->OnReconnect(id); }, Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputReconnectSuccess(void *data, calldata_t *)
{
	auto ctx = static_cast<SignalContext *>(data);
	QMetaObject::invokeMethod(
		ctx->self, [self = ctx->self, id = ctx->id]() { self->OnReconnectSuccess(id); }, Qt::QueuedConnection);
}

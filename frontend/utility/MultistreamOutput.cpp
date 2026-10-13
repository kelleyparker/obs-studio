#include "MultistreamOutput.hpp"

#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QUuid>

#include <util/config-file.h>

#include <cstring>

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
		 "https://kick.com/dashboard/settings/stream", 8000},
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

static bool IsCodec(obs_encoder_t *encoder, const char *codec)
{
	return encoder && strcmp(obs_encoder_get_codec(encoder), codec) == 0;
}

static int GetEncoderBitrate(obs_encoder_t *encoder)
{
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	return (int)obs_data_get_int(settings, "bitrate");
}

/* Platforms that only reliably accept H.264 video over RTMP */
static bool PlatformRequiresH264(const string &platform)
{
	return platform == "kick" || platform == "twitch";
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

void MultistreamOutput::ApplyPrimaryEncoderSettings(obs_data_t *videoSettings) const
{
	if (!enabled || mode != MultistreamMode::Shared) {
		return;
	}

	bool hasDestinations = false;
	for (const auto &dest : destinations) {
		hasDestinations = hasDestinations || dest.enabled;
	}
	if (!hasDestinations) {
		return;
	}

	/* Twitch, YouTube and Kick all require a keyframe at least every two
	 * seconds. It also lets destinations that join the already running
	 * encoder start right away, as they have to wait for a keyframe. */
	int keyint = (int)obs_data_get_int(videoSettings, "keyint_sec");
	if (keyint <= 0 || keyint > MultistreamKeyframeIntervalSec) {
		obs_data_set_int(videoSettings, "keyint_sec", MultistreamKeyframeIntervalSec);
		blog(LOG_INFO, "[Multistream] Using a %d s keyframe interval for the shared stream",
		     MultistreamKeyframeIntervalSec);
	}
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

obs_encoder_t *MultistreamOutput::CreateVideoEncoder(const MultistreamDestination &dest, const string &encoderId,
						     obs_output_t *primary)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);

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
			/* Rescale on the GPU instead of on the CPU */
			obs_encoder_set_scaled_size(encoder, dest.width, dest.height);
			obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
		}
	}

	obs_encoder_set_video(encoder, obs_get_video());
	return encoder;
}

bool MultistreamOutput::AcquireSharedEncoders(Running &run, obs_output_t *primary)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);
	if (!primaryVideo) {
		return false;
	}

	run.videoEncoder = obs_encoder_get_ref(primaryVideo);

	/* RTMP needs AAC: reuse the main stream's audio encoder if it is AAC,
	 * otherwise (e.g. Opus for WHIP) encode AAC once for every destination.
	 * The video encoder is already running, so libobs won't pair them. */
	if (IsCodec(primaryAudio, "aac")) {
		run.audioEncoder = obs_encoder_get_ref(primaryAudio);
		return true;
	}

	run.audioEncoder = obs_weak_encoder_get_encoder(sharedModeAudioEncoder);
	if (!run.audioEncoder) {
		size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;
		int bitrate = primaryAudio ? GetEncoderBitrate(primaryAudio) : 0;
		run.audioEncoder = CreateAudioEncoder("multistream_shared_aac", bitrate > 0 ? bitrate : 160, mixer);
		if (run.audioEncoder) {
			sharedModeAudioEncoder = obs_encoder_get_weak_encoder(run.audioEncoder);
		}
	}

	return !!run.audioEncoder;
}

bool MultistreamOutput::AcquireSeparateEncoders(Running &run, const MultistreamDestination &dest, obs_output_t *primary,
						bool dynamicBitrate)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);

	string encoderId = dest.encoder;
	if (!encoderId.empty() && !obs_get_encoder_codec(encoderId.c_str())) {
		/* e.g. a profile from a machine with a different GPU */
		blog(LOG_WARNING, "[Multistream] Encoder '%s' for '%s' is not available, using the default encoder",
		     encoderId.c_str(), dest.name.c_str());
		encoderId.clear();
	}
	if (encoderId.empty()) {
		encoderId = IsCodec(primaryVideo, "h264") ? obs_encoder_get_id(primaryVideo) : "obs_x264";
	}

	int audioBitrate = FindClosestAvailableSimpleAACBitrate(dest.audioBitrate);
	size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;

	/* Destinations with identical settings share their encoders instead of
	 * encoding the same thing twice. With dynamic bitrate every destination
	 * needs its own encoders, as the bitrate is adjusted per connection. */
	string key = encoderId + "|" + to_string(dest.videoBitrate) + "|" + to_string(dest.width) + "x" +
		     to_string(dest.height) + "|" + to_string(audioBitrate) + "|" + to_string(mixer);
	if (dynamicBitrate) {
		key += "|" + dest.id;
	}

	EncoderPair &cached = separateModeEncoders[key];
	run.videoEncoder = obs_weak_encoder_get_encoder(cached.video);
	run.audioEncoder = obs_weak_encoder_get_encoder(cached.audio);
	if (run.videoEncoder && run.audioEncoder) {
		return true;
	}

	run.videoEncoder = CreateVideoEncoder(dest, encoderId, primary);

	/* The main stream's audio encoder is already running, so reusing it
	 * doesn't pair it with the new video encoder. */
	if (IsCodec(primaryAudio, "aac") && obs_encoder_active(primaryAudio) &&
	    GetEncoderBitrate(primaryAudio) == audioBitrate) {
		run.audioEncoder = obs_encoder_get_ref(primaryAudio);
	} else {
		run.audioEncoder = CreateAudioEncoder("multistream_audio_" + dest.name, audioBitrate, mixer);
	}

	if (!run.videoEncoder || !run.audioEncoder) {
		return false;
	}

	cached.video = obs_encoder_get_weak_encoder(run.videoEncoder);
	cached.audio = obs_encoder_get_weak_encoder(run.audioEncoder);
	return true;
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

	config_t *config = OBSBasic::Get()->Config();
	bool dynamicBitrate = config_get_bool(config, "Output", "DynamicBitrate");

	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	bool useShared = mode == MultistreamMode::Shared && primarySharedEncoders && primaryVideo;
	if (useShared && !IsCodec(primaryVideo, "h264")) {
		if (PlatformRequiresH264(dest->platform)) {
			blog(LOG_INFO,
			     "[Multistream] '%s' needs H.264 but the main stream uses %s; using a separate encoder",
			     dest->name.c_str(), obs_encoder_get_codec(primaryVideo));
			useShared = false;
		} else {
			blog(LOG_WARNING, "[Multistream] '%s' receives %s video; make sure the platform supports it",
			     dest->name.c_str(), obs_encoder_get_codec(primaryVideo));
		}
	}

	auto run = make_unique<Running>();
	run->context = make_unique<SignalContext>(SignalContext{this, id, ++nextSerial});

	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", dest->server.c_str());
	obs_data_set_string(serviceSettings, "key", dest->key.c_str());

	string serviceName = "multistream_service_" + dest->name;
	run->service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);

	string outputName = "multistream_" + dest->name;
	run->output = obs_output_create("rtmp_output", outputName.c_str(), nullptr, nullptr);

	bool encodersAcquired = useShared ? AcquireSharedEncoders(*run, primary)
					  : AcquireSeparateEncoders(*run, *dest, primary, dynamicBitrate);

	if (!run->service || !run->output || !encodersAcquired) {
		SetState(id, MultistreamState::Error, QTStr("Multistream.Error.CreateFailed"));
		return false;
	}

	obs_output_set_video_encoder(run->output, run->videoEncoder);
	obs_output_set_audio_encoder(run->output, run->audioEncoder, 0);
	obs_output_set_service(run->output, run->service);

	/* Use the same network settings as the main stream */
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
	/* A congested destination must not lower the bitrate of an encoder
	 * that the main stream and the other platforms use as well. */
	obs_data_set_bool(outputSettings, "dyn_bitrate", dynamicBitrate && !useShared);
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
		SetState(id, MultistreamState::Stopping);
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

bool MultistreamOutput::IsStopping(const string &id) const
{
	auto it = running.find(id);
	return it != running.end() && it->second->stopRequested;
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

bool MultistreamOutput::IsCurrentRun(const string &id, uint64_t serial) const
{
	auto it = running.find(id);
	return it != running.end() && it->second->context->serial == serial;
}

void MultistreamOutput::OnStarted(const string &id, uint64_t serial)
{
	if (IsCurrentRun(id, serial) && !IsStopping(id)) {
		SetState(id, MultistreamState::Live);
	}
}

void MultistreamOutput::OnStopped(const string &id, uint64_t serial, int code, const QString &lastError)
{
	if (!IsCurrentRun(id, serial)) {
		return;
	}

	running.erase(id);

	/* Nothing uses the cached encoders anymore */
	if (running.empty()) {
		separateModeEncoders.clear();
		sharedModeAudioEncoder = nullptr;
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

void MultistreamOutput::OnReconnect(const string &id, uint64_t serial)
{
	if (IsCurrentRun(id, serial) && !IsStopping(id)) {
		SetState(id, MultistreamState::Reconnecting);
	}
}

void MultistreamOutput::OnReconnectSuccess(const string &id, uint64_t serial)
{
	if (IsCurrentRun(id, serial) && !IsStopping(id)) {
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
		ctx->self, [self = ctx->self, id = ctx->id, serial = ctx->serial]() { self->OnStarted(id, serial); },
		Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputStop(void *data, calldata_t *params)
{
	auto ctx = static_cast<SignalContext *>(data);
	int code = (int)calldata_int(params, "code");
	QString lastError = QString::fromUtf8(calldata_string(params, "last_error"));
	QMetaObject::invokeMethod(
		ctx->self,
		[self = ctx->self, id = ctx->id, serial = ctx->serial, code, lastError]() {
			self->OnStopped(id, serial, code, lastError);
		},
		Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputReconnect(void *data, calldata_t *)
{
	auto ctx = static_cast<SignalContext *>(data);
	QMetaObject::invokeMethod(
		ctx->self, [self = ctx->self, id = ctx->id, serial = ctx->serial]() { self->OnReconnect(id, serial); },
		Qt::QueuedConnection);
}

void MultistreamOutput::OBSOutputReconnectSuccess(void *data, calldata_t *)
{
	auto ctx = static_cast<SignalContext *>(data);
	QMetaObject::invokeMethod(
		ctx->self,
		[self = ctx->self, id = ctx->id, serial = ctx->serial]() { self->OnReconnectSuccess(id, serial); },
		Qt::QueuedConnection);
}

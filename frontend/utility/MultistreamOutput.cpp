#include "MultistreamOutput.hpp"

#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QUuid>

#include <util/config-file.h>

#include <algorithm>
#include <cmath>
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

/* Limits from the platforms' own streaming guides, for what OBS's service
 * list doesn't provide (resolution, frame rate and per-resolution bitrates):
 * - Twitch:  https://help.twitch.tv/s/article/broadcasting-guidelines
 * - YouTube: https://support.google.com/youtube/answer/2853702
 * - Kick:    https://help.kick.com/en/articles/7066931-how-to-stream-on-kick-com */

/* Kick: H.264, CBR, 1920x1080, 1,000-8,000 kbps (the maximum is used), 2 s
 * keyframe interval. For NVENC: P6 preset, ultra low latency tuning, single
 * pass, main profile, no B-frames, no look-ahead, psycho visual tuning. */
static const MultistreamPlatformLimits KickLimits = {
	1920,
	1080,
	0,
	0,
	0,
	{"h264"},
	{{1080, 0, true, 8000}},
	{{"obs_nvenc", R"({"preset": "p6", "tune": "ull", "multipass": "disabled", "profile": "main", "bf": 0,)"
		       R"( "lookahead": false, "adaptive_quantization": true})"}},
};

/* YouTube: H.264, H.265 (HEVC) or AV1, CBR, up to 60 fps, 2 s keyframe
 * interval. The recommended bitrate for each resolution and frame rate is
 * used as the maximum. 128 kbps AAC for stereo, 384 kbps for 5.1 surround.
 * 2 B-frames and 1 reference frame. */
static const MultistreamPlatformLimits YouTubeLimits = {
	3840,
	2160,
	60,
	128,
	384,
	{"h264", "hevc", "av1"},
	{
		/* H.264 */
		{2160, 60, true, 50000},
		{2160, 30, true, 42000},
		{1440, 60, true, 34000},
		{1440, 30, true, 21000},
		{1080, 60, true, 17000},
		{1080, 30, true, 14000},
		{720, 60, true, 8000},
		{720, 30, true, 8000},
		{480, 30, true, 4000},
		{360, 30, true, 4000},
		/* AV1 and H.265 */
		{2160, 60, false, 35000},
		{2160, 30, false, 30000},
		{1440, 60, false, 24000},
		{1440, 30, false, 15000},
		{1080, 60, false, 12000},
		{1080, 30, false, 10000},
		{720, 60, false, 6000},
		{720, 30, false, 6000},
		{480, 30, false, 3000},
		{360, 30, false, 3000},
	},
	{
		{"obs_nvenc", R"({"bf": 2})"},
		{"obs_x264", R"({"x264opts": "bframes=2 ref=1"})"},
	},
};

/* Twitch: H.264, CBR, up to 1920x1080 and 60 fps, 2 s keyframe interval.
 * 1080p60: 6000 kbps, 1080p30: 4500 kbps, 720p60: 4500 kbps, 720p30: 3000
 * kbps, for NVIDIA NVENC and x264 alike. NVENC: "Quality" preset (P5 with
 * high quality tuning in current OBS) and 2 B-frames. x264: veryfast to
 * medium preset and Main/High profile, of which medium and High are used. */
static const MultistreamPlatformLimits TwitchLimits = {
	1920,
	1080,
	60,
	0,
	0,
	{"h264"},
	{
		{1080, 60, true, 6000},
		{1080, 30, true, 4500},
		{720, 60, true, 4500},
		{720, 30, true, 3000},
	},
	{
		{"obs_nvenc", R"({"preset": "p5", "tune": "hq", "bf": 2})"},
		{"obs_x264", R"({"preset": "medium", "profile": "high"})"},
	},
	{
		{"nvenc_1080p60", "NVIDIA NVENC", "obs_nvenc_h264_tex", 1920, 1080, 60, 6000},
		{"nvenc_1080p30", "NVIDIA NVENC", "obs_nvenc_h264_tex", 1920, 1080, 30, 4500},
		{"nvenc_720p60", "NVIDIA NVENC", "obs_nvenc_h264_tex", 1280, 720, 60, 4500},
		{"nvenc_720p30", "NVIDIA NVENC", "obs_nvenc_h264_tex", 1280, 720, 30, 3000},
		{"x264_1080p60", "x264", "obs_x264", 1920, 1080, 60, 6000},
		{"x264_1080p30", "x264", "obs_x264", 1920, 1080, 30, 4500},
		{"x264_720p60", "x264", "obs_x264", 1280, 720, 60, 4500},
		{"x264_720p30", "x264", "obs_x264", 1280, 720, 30, 3000},
	},
};

const vector<MultistreamPlatformPreset> &GetMultistreamPlatformPresets()
{
	static const vector<MultistreamPlatformPreset> presets = {
		{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "https://studio.youtube.com/",
		 "YouTube - RTMPS", &YouTubeLimits},
		{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "https://dashboard.twitch.tv/settings/stream",
		 "Twitch", &TwitchLimits},
		{"kick", "Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app",
		 "https://kick.com/dashboard/settings/stream", nullptr, &KickLimits},
		{"custom", "Custom RTMP", "", "", nullptr, nullptr},
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

/* An rtmp-services entry to query a platform's recommendations from */
static obs_service_t *CreatePlatformService(const MultistreamPlatformPreset *preset)
{
	if (!preset || !preset->obsServiceName) {
		return nullptr;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "service", preset->obsServiceName);
	return obs_service_create_private("rtmp_common", "multistream_platform_info", settings);
}

bool MultistreamPlatformSupportsMaxQuality(const string &platform)
{
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(platform);
	return preset && (preset->obsServiceName || preset->limits);
}

const MultistreamQualityPreset *FindMultistreamQualityPreset(const string &platform, const string &id)
{
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(platform);
	if (!preset || !preset->limits) {
		return nullptr;
	}
	for (const auto &qualityPreset : preset->limits->qualityPresets) {
		if (id == qualityPreset.id) {
			return &qualityPreset;
		}
	}
	return nullptr;
}

MultistreamQualityMode GetMultistreamQualityMode(const MultistreamDestination &dest)
{
	if (dest.quality == MultistreamQualityMode::Preset &&
	    FindMultistreamQualityPreset(dest.platform, dest.qualityPreset)) {
		return MultistreamQualityMode::Preset;
	}
	if (dest.quality != MultistreamQualityMode::Custom && MultistreamPlatformSupportsMaxQuality(dest.platform)) {
		return MultistreamQualityMode::Max;
	}
	return MultistreamQualityMode::Custom;
}

vector<string> GetMultistreamPlatformCodecs(const string &platform)
{
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(platform);
	if (preset && preset->limits && !preset->limits->codecs.empty()) {
		return preset->limits->codecs;
	}

	OBSServiceAutoRelease service = CreatePlatformService(preset);
	if (service) {
		vector<string> codecs;
		for (const char **codec = obs_service_get_supported_video_codecs(service); codec && *codec; codec++) {
			codecs.emplace_back(*codec);
		}
		if (!codecs.empty()) {
			return codecs;
		}
	}

	/* Custom servers may support Enhanced RTMP, it's up to the user */
	if (platform == "custom") {
		return {"h264", "hevc", "av1"};
	}
	return {"h264"};
}

static uint32_t RoundToEven(double value)
{
	return (uint32_t)(lround(value / 2.0) * 2);
}

static int LookupMaxVideoBitrate(const MultistreamPlatformLimits &limits, uint32_t height, double fps, bool h264)
{
	bool hasCodecLimits = false;
	for (const auto &limit : limits.bitrates) {
		hasCodecLimits = hasCodecLimits || limit.h264 == h264;
	}

	/* The limit of the smallest resolution/frame rate that covers the
	 * stream, or the largest one if the stream exceeds all of them */
	const MultistreamBitrateLimit *best = nullptr;
	const MultistreamBitrateLimit *largest = nullptr;
	for (const auto &limit : limits.bitrates) {
		if (hasCodecLimits && limit.h264 != h264) {
			continue;
		}
		if (!largest || limit.height > largest->height ||
		    (limit.height == largest->height && limit.fps > largest->fps)) {
			largest = &limit;
		}
		if (limit.height < height || (limit.fps && (double)limit.fps + 0.5 < fps)) {
			continue;
		}
		if (!best || limit.height < best->height || (limit.height == best->height && limit.fps < best->fps)) {
			best = &limit;
		}
	}

	best = best ? best : largest;
	return best ? best->maxVideoBitrate : 0;
}

static int LargestAacBitrateUpTo(int bitrate)
{
	int result = 0;
	for (const auto &[available, encoderId] : GetSimpleAACEncoderBitrateMap()) {
		if (available <= bitrate || !result) {
			result = available;
		}
	}
	return result ? result : bitrate;
}

/* The video of the canvas a destination streams */
static video_t *GetDestinationVideo(const MultistreamDestination &dest)
{
	if (!dest.vertical) {
		return obs_get_video();
	}
	VerticalCanvas *vertical = OBSBasic::Get()->GetVerticalCanvas();
	return vertical && vertical->Enabled() ? vertical->GetVideo() : nullptr;
}

static bool GetDestinationVideoInfo(const MultistreamDestination &dest, obs_video_info &ovi)
{
	if (!dest.vertical) {
		return obs_get_video_info(&ovi);
	}
	VerticalCanvas *vertical = OBSBasic::Get()->GetVerticalCanvas();
	return vertical && vertical->Enabled() && vertical->GetVideoInfo(ovi);
}

/* The most audio the platform accepts, from its guide or else OBS's service
 * list. 0 if unknown (custom servers). */
static int GetPlatformMaxAudioBitrate(const string &platform)
{
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(platform);
	if (preset && preset->limits) {
		obs_audio_info oai = {};
		bool surround = obs_get_audio_info(&oai) && get_audio_channels(oai.speakers) > 2;
		int limit = surround && preset->limits->maxSurroundAudioBitrate
				    ? preset->limits->maxSurroundAudioBitrate
				    : preset->limits->maxAudioBitrate;
		if (limit > 0) {
			return limit;
		}
	}

	OBSServiceAutoRelease service = CreatePlatformService(preset);
	if (!service) {
		return 0;
	}
	int maxVideoBitrate = 0;
	int maxAudioBitrate = 0;
	obs_service_get_max_bitrate(service, &maxVideoBitrate, &maxAudioBitrate);
	return maxAudioBitrate;
}

/* The audio bitrate to send a platform: what was asked for, but never more
 * than the platform accepts */
static int CapAudioBitrate(const string &platform, int bitrate)
{
	int maxAudioBitrate = GetPlatformMaxAudioBitrate(platform);
	return maxAudioBitrate > 0 ? min(bitrate, maxAudioBitrate) : bitrate;
}

MultistreamQuality ResolveMultistreamQuality(const MultistreamDestination &dest, const string &codec)
{
	MultistreamQuality quality;
	MultistreamQualityMode mode = GetMultistreamQualityMode(dest);

	if (mode == MultistreamQualityMode::Custom) {
		quality.width = dest.width;
		quality.height = dest.height;
		quality.videoBitrate = dest.videoBitrate;
		quality.audioBitrate = LargestAacBitrateUpTo(CapAudioBitrate(dest.platform, kFixedAudioBitrateKbps));
		return quality;
	}

	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(dest.platform);
	const MultistreamPlatformLimits *limits = preset->limits;
	const MultistreamQualityPreset *qualityPreset =
		mode == MultistreamQualityMode::Preset ? FindMultistreamQualityPreset(dest.platform, dest.qualityPreset)
						       : nullptr;

	obs_video_info ovi = {};
	bool hasVideo = GetDestinationVideoInfo(dest, ovi);
	uint32_t width = hasVideo ? ovi.output_width : (dest.vertical ? 1080 : 1920);
	uint32_t height = hasVideo ? ovi.output_height : (dest.vertical ? 1920 : 1080);
	double fps = hasVideo ? (double)ovi.fps_num / (double)ovi.fps_den : 60.0;

	uint32_t maxWidth = qualityPreset ? qualityPreset->width : (limits ? limits->maxWidth : 0);
	uint32_t maxHeight = qualityPreset ? qualityPreset->height : (limits ? limits->maxHeight : 0);
	uint32_t maxFps = qualityPreset ? qualityPreset->fps : (limits ? limits->maxFps : 0);

	/* The guides' limits are for landscape video; a portrait stream
	 * (e.g. 1080x1920) has the same limits turned on their side. */
	if (height > width) {
		swap(maxWidth, maxHeight);
	}

	/* Scale down to the maximum resolution, keeping the aspect ratio. Never
	 * scale up: that only costs bandwidth. */
	double scale = 1.0;
	if (maxWidth && width > maxWidth) {
		scale = min(scale, (double)maxWidth / (double)width);
	}
	if (maxHeight && height > maxHeight) {
		scale = min(scale, (double)maxHeight / (double)height);
	}
	if (scale < 1.0) {
		width = RoundToEven((double)width * scale);
		height = RoundToEven((double)height * scale);
		quality.width = width;
		quality.height = height;
	}

	/* Skip frames to stay within the maximum frame rate */
	if (maxFps && fps > (double)maxFps + 0.01) {
		quality.fpsDivisor = (uint32_t)ceil(fps / (double)maxFps - 0.001);
		fps /= (double)quality.fpsDivisor;
	}

	int videoBitrate = 0;
	int audioBitrate = 0;

	if (qualityPreset) {
		videoBitrate = qualityPreset->videoBitrate;
	} else if (limits) {
		/* The bitrate tables are by the short side ("1080p") */
		videoBitrate = LookupMaxVideoBitrate(*limits, min(width, height), fps, codec == "h264");
	}

	if (limits) {
		obs_audio_info oai = {};
		bool surround = obs_get_audio_info(&oai) && get_audio_channels(oai.speakers) > 2;
		audioBitrate = surround && limits->maxSurroundAudioBitrate ? limits->maxSurroundAudioBitrate
									   : limits->maxAudioBitrate;
	}

	/* OBS's service list fills in what the platform's guide doesn't say */
	OBSServiceAutoRelease service = CreatePlatformService(preset);
	if (service && (!videoBitrate || !audioBitrate)) {
		int maxVideoBitrate = 0;
		int maxAudioBitrate = 0;
		obs_service_get_max_bitrate(service, &maxVideoBitrate, &maxAudioBitrate);
		videoBitrate = videoBitrate ? videoBitrate : maxVideoBitrate;
		audioBitrate = audioBitrate ? audioBitrate : maxAudioBitrate;
	}

	quality.videoBitrate = videoBitrate > 0 ? videoBitrate : dest.videoBitrate;
	quality.audioBitrate = LargestAacBitrateUpTo(audioBitrate > 0 ? audioBitrate : dest.audioBitrate);
	return quality;
}

string FindMultistreamEncoder(const char *preferredId, const string &codec)
{
	auto codecOf = [](const char *id) -> string {
		const char *encoderCodec = id ? obs_get_encoder_codec(id) : nullptr;
		return encoderCodec ? encoderCodec : "";
	};

	if (preferredId && *preferredId && codecOf(preferredId) == codec) {
		return preferredId;
	}

	static const char *families[] = {"nvenc", "qsv", "amf", "videotoolbox", "vaapi", "x264", "x265", "aom", "svt"};
	string family;
	for (const char *candidate : families) {
		if (preferredId && strstr(preferredId, candidate)) {
			family = candidate;
			break;
		}
	}
	bool preferTexture = preferredId && strstr(preferredId, "tex");

	string best;
	int bestScore = -1;
	const char *id;
	for (size_t idx = 0; obs_enum_encoder_types(idx, &id); idx++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO || codecOf(id) != codec) {
			continue;
		}
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) {
			continue;
		}

		int score = 0;
		if (!family.empty() && strstr(id, family.c_str())) {
			score += 2;
		}
		if (preferTexture == !!strstr(id, "tex")) {
			score += 1;
		}
		if (codec == "h264" && strcmp(id, "obs_x264") == 0) {
			score += 1; /* the usual software fallback */
		}
		if (score > bestScore) {
			best = id;
			bestScore = score;
		}
	}

	return best;
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

static bool PlatformSupportsCodec(const string &platform, const char *codec)
{
	for (const auto &supported : GetMultistreamPlatformCodecs(platform)) {
		if (supported == codec) {
			return true;
		}
	}
	return false;
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
			obs_data_set_default_int(item, "audio_bitrate", kFixedAudioBitrateKbps);
			obs_data_set_default_string(item, "platform", "custom");
			obs_data_set_default_string(item, "quality", "custom");

			MultistreamDestination dest;
			dest.id = obs_data_get_string(item, "id");
			dest.name = obs_data_get_string(item, "name");
			dest.platform = obs_data_get_string(item, "platform");
			dest.server = obs_data_get_string(item, "server");
			dest.key = obs_data_get_string(item, "key");
			dest.enabled = obs_data_get_bool(item, "enabled");
			dest.vertical = strcmp(obs_data_get_string(item, "canvas"), "vertical") == 0;
			const char *quality = obs_data_get_string(item, "quality");
			dest.quality = strcmp(quality, "max") == 0      ? MultistreamQualityMode::Max
				       : strcmp(quality, "preset") == 0 ? MultistreamQualityMode::Preset
									: MultistreamQualityMode::Custom;
			dest.qualityPreset = obs_data_get_string(item, "quality_preset");
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
		obs_data_set_string(item, "canvas", dest.vertical ? "vertical" : "main");
		obs_data_set_string(item, "quality",
				    dest.quality == MultistreamQualityMode::Max      ? "max"
				    : dest.quality == MultistreamQualityMode::Preset ? "preset"
										     : "custom");
		obs_data_set_string(item, "quality_preset", dest.qualityPreset.c_str());
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
		/* Vertical destinations have their own encoder */
		hasDestinations = hasDestinations || (dest.enabled && !dest.vertical);
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
						     const MultistreamQuality &quality, obs_output_t *primary)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);

	OBSDataAutoRelease settings = obs_data_create();

	/* Start from the main stream's settings (preset, profile, tuning, ...)
	 * when the destination uses the same encoder type. */
	if (primaryVideo && encoderId == obs_encoder_get_id(primaryVideo)) {
		OBSDataAutoRelease primarySettings = obs_encoder_get_settings(primaryVideo);
		obs_data_apply(settings, primarySettings);
	}

	obs_data_set_int(settings, "bitrate", quality.videoBitrate);
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "keyint_sec", MultistreamKeyframeIntervalSec);

	/* The platform's recommendations from OBS's service list (keyframe
	 * interval, maximum bitrate, x264 options, ...) */
	OBSServiceAutoRelease platformService = CreatePlatformService(FindMultistreamPlatformPreset(dest.platform));
	if (platformService) {
		obs_service_apply_encoder_settings(platformService, settings, nullptr);

		/* The resolved bitrate already follows the platform's guide, or is
		 * the user's explicit choice */
		obs_data_set_int(settings, "bitrate", quality.videoBitrate);
	}

	/* Encoder settings from the platform's streaming guide */
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(dest.platform);
	if (preset && preset->limits) {
		for (const auto &guideSettings : preset->limits->encoderSettings) {
			if (!strstr(encoderId.c_str(), guideSettings.encoderIdPart)) {
				continue;
			}
			OBSDataAutoRelease data = obs_data_create_from_json(guideSettings.json);
			if (data) {
				/* Keep the x264 options from the main stream and the service */
				string x264opts = obs_data_get_string(settings, "x264opts");
				if (obs_data_has_user_value(data, "x264opts") && !x264opts.empty()) {
					x264opts += string(" ") + obs_data_get_string(data, "x264opts");
					obs_data_set_string(data, "x264opts", x264opts.c_str());
				}
				obs_data_apply(settings, data);
				blog(LOG_INFO, "[Multistream] '%s': %s settings from the %s streaming guide: %s",
				     dest.name.c_str(), encoderId.c_str(), preset->displayName, guideSettings.json);
			}
		}
	}

	int keyint = (int)obs_data_get_int(settings, "keyint_sec");
	if (keyint <= 0 || keyint > MultistreamKeyframeIntervalSec) {
		obs_data_set_int(settings, "keyint_sec", MultistreamKeyframeIntervalSec);
	}

	string name = "multistream_video_" + dest.name;
	obs_encoder_t *encoder = obs_video_encoder_create(encoderId.c_str(), name.c_str(), settings, nullptr);
	if (!encoder) {
		blog(LOG_WARNING, "[Multistream] Failed to create video encoder '%s' for '%s'", encoderId.c_str(),
		     dest.name.c_str());
		return nullptr;
	}

	if (quality.width && quality.height) {
		obs_video_info ovi;
		if (!GetDestinationVideoInfo(dest, ovi) || ovi.output_width != quality.width ||
		    ovi.output_height != quality.height) {
			/* Rescale on the GPU instead of on the CPU */
			obs_encoder_set_scaled_size(encoder, quality.width, quality.height);
			obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
		}
	}

	if (quality.fpsDivisor > 1) {
		obs_encoder_set_frame_rate_divisor(encoder, quality.fpsDivisor);
	}

	obs_encoder_set_video(encoder, GetDestinationVideo(dest));
	return encoder;
}

bool MultistreamOutput::AcquireSharedEncoders(Running &run, const MultistreamDestination &dest, obs_output_t *primary)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);
	if (!primaryVideo) {
		return false;
	}

	run.videoEncoder = obs_encoder_get_ref(primaryVideo);

	/* RTMP needs AAC: reuse the main stream's audio encoder if it is AAC
	 * and within the platform's limit. Otherwise (e.g. Opus for WHIP, or
	 * more audio than the platform accepts) encode AAC once per bitrate.
	 * The video encoder is already running, so libobs won't pair them. */
	int primaryBitrate = primaryAudio ? GetEncoderBitrate(primaryAudio) : 0;
	int bitrate = LargestAacBitrateUpTo(
		CapAudioBitrate(dest.platform, primaryBitrate > 0 ? primaryBitrate : kFixedAudioBitrateKbps));
	if (IsCodec(primaryAudio, "aac") && primaryBitrate <= bitrate) {
		run.audioEncoder = obs_encoder_get_ref(primaryAudio);
		return true;
	}

	OBSWeakEncoderAutoRelease &cached = sharedModeAudioEncoders[bitrate];
	run.audioEncoder = obs_weak_encoder_get_encoder(cached);
	if (!run.audioEncoder) {
		size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;
		string name = "multistream_shared_aac_" + to_string(bitrate);
		run.audioEncoder = CreateAudioEncoder(name, bitrate, mixer);
		if (run.audioEncoder) {
			cached = obs_encoder_get_weak_encoder(run.audioEncoder);
		}
	}

	return !!run.audioEncoder;
}

bool MultistreamOutput::AcquireSeparateEncoders(Running &run, const MultistreamDestination &dest, obs_output_t *primary,
						bool dynamicBitrate)
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primary, 0);
	const char *primaryVideoId = primaryVideo ? obs_encoder_get_id(primaryVideo) : "obs_x264";

	MultistreamQualityMode qualityMode = GetMultistreamQualityMode(dest);
	string encoderId = dest.encoder;

	/* A guide preset is for a specific encoder */
	if (qualityMode == MultistreamQualityMode::Preset) {
		const MultistreamQualityPreset *qualityPreset =
			FindMultistreamQualityPreset(dest.platform, dest.qualityPreset);
		encoderId = FindMultistreamEncoder(qualityPreset->preferredEncoder, "h264");
		if (encoderId != qualityPreset->preferredEncoder) {
			blog(LOG_WARNING, "[Multistream] '%s': %s isn't available, using %s instead", dest.name.c_str(),
			     qualityPreset->encoderName, encoderId.c_str());
		}
	}

	if (!encoderId.empty() && !obs_get_encoder_codec(encoderId.c_str())) {
		/* e.g. a profile from a machine with a different GPU */
		blog(LOG_WARNING, "[Multistream] Encoder '%s' for '%s' is not available, using the default encoder",
		     encoderId.c_str(), dest.name.c_str());
		encoderId.clear();
	}
	if (!encoderId.empty() && !PlatformSupportsCodec(dest.platform, obs_get_encoder_codec(encoderId.c_str()))) {
		blog(LOG_WARNING, "[Multistream] %s doesn't support %s video, using H.264 for '%s'",
		     dest.platform.c_str(), obs_get_encoder_codec(encoderId.c_str()), dest.name.c_str());
		encoderId = FindMultistreamEncoder(encoderId.c_str(), "h264");
	}
	if (encoderId.empty()) {
		/* An H.264 encoder of the same type as the main stream's, so a
		 * hardware encoder stays on the hardware encoder */
		encoderId = FindMultistreamEncoder(primaryVideoId, "h264");
	}
	if (encoderId.empty()) {
		return false;
	}

	MultistreamQuality quality = ResolveMultistreamQuality(dest, obs_get_encoder_codec(encoderId.c_str()));
	size_t mixer = primaryAudio ? obs_encoder_get_mixer_index(primaryAudio) : 0;

	string resolution = quality.width ? to_string(quality.width) + "x" + to_string(quality.height)
					  : string("output resolution");
	string frameRate = quality.fpsDivisor > 1 ? ", output frame rate / " + to_string(quality.fpsDivisor) : "";
	blog(LOG_INFO, "[Multistream] '%s' (%s quality): %s, %s%s, %d kbps video, %d kbps audio", dest.name.c_str(),
	     qualityMode == MultistreamQualityMode::Max      ? "platform maximum"
	     : qualityMode == MultistreamQualityMode::Preset ? dest.qualityPreset.c_str()
							     : "custom",
	     encoderId.c_str(), resolution.c_str(), frameRate.c_str(), quality.videoBitrate, quality.audioBitrate);

	/* Destinations with identical settings share their encoders instead of
	 * encoding the same thing twice. With dynamic bitrate every destination
	 * needs its own encoders, as the bitrate is adjusted per connection. */
	string key = encoderId + "|" + to_string(quality.videoBitrate) + "|" + to_string(quality.width) + "x" +
		     to_string(quality.height) + "/" + to_string(quality.fpsDivisor) + "|" +
		     to_string(quality.audioBitrate) + "|" + to_string(mixer) + "|" + dest.platform +
		     (dest.vertical ? "|vertical" : "|main");
	if (dynamicBitrate) {
		key += "|" + dest.id;
	}

	EncoderPair &cached = separateModeEncoders[key];
	run.videoEncoder = obs_weak_encoder_get_encoder(cached.video);
	run.audioEncoder = obs_weak_encoder_get_encoder(cached.audio);
	if (run.videoEncoder && run.audioEncoder) {
		return true;
	}

	run.videoEncoder = CreateVideoEncoder(dest, encoderId, quality, primary);

	/* The main stream's audio encoder is already running, so reusing it
	 * doesn't pair it with the new video encoder. */
	if (IsCodec(primaryAudio, "aac") && obs_encoder_active(primaryAudio) &&
	    GetEncoderBitrate(primaryAudio) == quality.audioBitrate) {
		run.audioEncoder = obs_encoder_get_ref(primaryAudio);
	} else {
		run.audioEncoder = CreateAudioEncoder("multistream_audio_" + dest.name, quality.audioBitrate, mixer);
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

	if (dest->vertical && !GetDestinationVideo(*dest)) {
		SetState(id, MultistreamState::Error, QTStr("Multistream.Error.VerticalOff"));
		return false;
	}

	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primary);
	/* The main stream's encoders encode the main canvas */
	bool useShared = mode == MultistreamMode::Shared && primarySharedEncoders && primaryVideo && !dest->vertical;
	if (useShared && !PlatformSupportsCodec(dest->platform, obs_encoder_get_codec(primaryVideo))) {
		blog(LOG_INFO, "[Multistream] %s doesn't support the main stream's %s video; using a separate encoder",
		     dest->platform.c_str(), obs_encoder_get_codec(primaryVideo));
		useShared = false;
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

	bool encodersAcquired = useShared ? AcquireSharedEncoders(*run, *dest, primary)
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

void MultistreamOutput::StopVertical(bool force)
{
	vector<string> ids;
	for (const auto &[id, run] : running) {
		const MultistreamDestination *dest = FindDestination(id);
		if (dest && dest->vertical) {
			ids.push_back(id);
		}
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
		sharedModeAudioEncoders.clear();
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

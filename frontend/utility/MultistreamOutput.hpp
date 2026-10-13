#pragma once

#include <obs.hpp>

#include <QObject>
#include <QString>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

enum class MultistreamMode {
	/* Encode once and send the same stream to every destination */
	Shared,
	/* Use a separate encoder for every destination */
	Separate,
};

enum class MultistreamState {
	Offline,
	Connecting,
	Live,
	Reconnecting,
	Stopping,
	Error,
};

/* Maximum video bitrate for streams up to a resolution and frame rate */
struct MultistreamBitrateLimit {
	uint32_t height;
	uint32_t fps; /* 0 = any frame rate */
	bool h264;    /* true: H.264 limit, false: HEVC/AV1 limit */
	int maxVideoBitrate;
};

/* Encoder settings a platform's guide recommends for an encoder type */
struct MultistreamEncoderSettings {
	const char *encoderIdPart; /* applies to encoders whose id contains this */
	const char *json;
};

/* Ingest limits from a platform's own streaming guide, for what OBS's
 * service list (rtmp-services) doesn't provide. 0 means no limit. */
struct MultistreamPlatformLimits {
	uint32_t maxWidth;
	uint32_t maxHeight;
	uint32_t maxFps;
	int maxAudioBitrate;
	std::vector<std::string> codecs; /* empty: use OBS's service list */
	std::vector<MultistreamBitrateLimit> bitrates;
	std::vector<MultistreamEncoderSettings> encoderSettings;
};

struct MultistreamPlatformPreset {
	const char *id;
	const char *displayName;
	const char *server;
	const char *streamKeyUrl;
	/* Name in OBS's service list, which provides the maximum bitrates,
	 * supported codecs and keyframe interval of the platform */
	const char *obsServiceName;
	const MultistreamPlatformLimits *limits;
};

const std::vector<MultistreamPlatformPreset> &GetMultistreamPlatformPresets();
const MultistreamPlatformPreset *FindMultistreamPlatformPreset(const std::string &id);

struct MultistreamDestination {
	std::string id;
	std::string name;
	std::string platform = "custom";
	std::string server;
	std::string key;
	bool enabled = true;

	/* Only used in separate encoder mode */
	bool maxQuality = false; /* stream at the platform's maximum quality */
	std::string encoder;     /* empty = H.264 encoder of the main stream's type */

	/* Custom quality */
	int videoBitrate = 6000;
	int audioBitrate = 160;
	uint32_t width = 0; /* 0 = use the output resolution */
	uint32_t height = 0;
};

/* The quality a destination is streamed at */
struct MultistreamQuality {
	uint32_t width = 0; /* 0 = output resolution */
	uint32_t height = 0;
	uint32_t fpsDivisor = 1;
	int videoBitrate = 0;
	int audioBitrate = 0;
};

bool MultistreamPlatformSupportsMaxQuality(const std::string &platform);
std::vector<std::string> GetMultistreamPlatformCodecs(const std::string &platform);
MultistreamQuality ResolveMultistreamQuality(const MultistreamDestination &dest, const std::string &codec);
/* Finds an encoder for a codec, preferring the same type (NVENC, QSV, AMF,
 * VideoToolbox, ...) as the preferred encoder */
std::string FindMultistreamEncoder(const char *preferredId, const std::string &codec);

class MultistreamOutput : public QObject {
	Q_OBJECT

public:
	struct Status {
		MultistreamState state = MultistreamState::Offline;
		QString lastError;
		uint64_t totalBytes = 0;
		int framesDropped = 0;
		int totalFrames = 0;
	};

	explicit MultistreamOutput(QObject *parent = nullptr);
	~MultistreamOutput();

	void Load(const std::filesystem::path &profilePath);
	bool Save() const;

	bool Enabled() const { return enabled; }
	void SetEnabled(bool enable) { enabled = enable; }
	MultistreamMode Mode() const { return mode; }
	void SetMode(MultistreamMode newMode) { mode = newMode; }

	const std::vector<MultistreamDestination> &Destinations() const { return destinations; }
	void SetDestinations(std::vector<MultistreamDestination> newDestinations);
	const MultistreamDestination *FindDestination(const std::string &id) const;

	/* Called when the main stream goes live; starts every enabled
	 * destination if multistreaming is enabled. sharedEncodersAllowed is
	 * false when the main output's encoders can't be reused (e.g. when
	 * Enhanced Broadcasting is active). */
	void PrimaryStarted(obs_output_t *primary, bool sharedEncodersAllowed);
	/* Called when the main stream stops; stops every destination that
	 * hasn't already been asked to stop (or all of them when forced). */
	void PrimaryStopped(bool force);

	bool PrimaryLive() const { return !!primaryOutput; }

	/* Adjusts the main stream's video encoder settings for the platforms
	 * that receive the same encoded stream in shared mode. */
	void ApplyPrimaryEncoderSettings(obs_data_t *videoSettings) const;

	bool Start(const std::string &id);
	void Stop(const std::string &id, bool force = false);
	void StopAll(bool force = false);

	bool Active() const;
	bool IsActive(const std::string &id) const;
	bool IsStopping(const std::string &id) const;
	Status GetStatus(const std::string &id) const;

	static std::string GenerateId();

signals:
	void DestinationsChanged();
	void StatusChanged(const QString &id);
	void PrimaryStateChanged(bool live);

private:
	struct SignalContext {
		MultistreamOutput *self;
		std::string id;
		/* Distinguishes runs of the same destination so that a late
		 * queued callback from a previous run can't affect a new one */
		uint64_t serial;
	};

	/* Member order matters: signals are disconnected before the output is
	 * released, and the output is released before its encoders/service.
	 * The encoder references are always strong references, whether the
	 * encoders are owned by this destination or shared. */
	struct Running {
		std::unique_ptr<SignalContext> context;
		OBSServiceAutoRelease service;
		OBSEncoderAutoRelease videoEncoder;
		OBSEncoderAutoRelease audioEncoder;
		OBSOutputAutoRelease output;
		OBSSignal startSignal;
		OBSSignal stopSignal;
		OBSSignal reconnectSignal;
		OBSSignal reconnectSuccessSignal;
		bool stopRequested = false;
	};

	struct StateInfo {
		MultistreamState state = MultistreamState::Offline;
		QString lastError;
	};

	/* Encoders shared between destinations. Weak references, so encoders
	 * are freed as soon as no destination uses them anymore. Video and
	 * audio encoders are only ever shared together: libobs pairs a new
	 * audio encoder with the new video encoder it is started with, so an
	 * audio encoder must never be shared with a different video encoder. */
	struct EncoderPair {
		OBSWeakEncoderAutoRelease video;
		OBSWeakEncoderAutoRelease audio;
	};

	bool enabled = false;
	MultistreamMode mode = MultistreamMode::Shared;
	std::vector<MultistreamDestination> destinations;
	std::filesystem::path configPath;

	OBSWeakOutputAutoRelease primaryOutput;
	bool primarySharedEncoders = true;

	OBSWeakEncoderAutoRelease sharedModeAudioEncoder;
	std::map<std::string, EncoderPair> separateModeEncoders;

	uint64_t nextSerial = 0;
	std::map<std::string, std::unique_ptr<Running>> running;
	std::map<std::string, StateInfo> states;

	bool AcquireSharedEncoders(Running &run, obs_output_t *primary);
	bool AcquireSeparateEncoders(Running &run, const MultistreamDestination &dest, obs_output_t *primary,
				     bool dynamicBitrate);
	obs_encoder_t *CreateVideoEncoder(const MultistreamDestination &dest, const std::string &encoderId,
					  const MultistreamQuality &quality, obs_output_t *primary);
	obs_encoder_t *CreateAudioEncoder(const std::string &name, int bitrate, size_t mixer);
	void SetState(const std::string &id, MultistreamState state, const QString &lastError = QString());
	bool IsCurrentRun(const std::string &id, uint64_t serial) const;

	void OnStarted(const std::string &id, uint64_t serial);
	void OnStopped(const std::string &id, uint64_t serial, int code, const QString &lastError);
	void OnReconnect(const std::string &id, uint64_t serial);
	void OnReconnectSuccess(const std::string &id, uint64_t serial);

	static void OBSOutputStart(void *data, calldata_t *params);
	static void OBSOutputStop(void *data, calldata_t *params);
	static void OBSOutputReconnect(void *data, calldata_t *params);
	static void OBSOutputReconnectSuccess(void *data, calldata_t *params);
};

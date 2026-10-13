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
	Error,
};

struct MultistreamPlatformPreset {
	const char *id;
	const char *displayName;
	const char *server;
	const char *streamKeyUrl;
	int defaultVideoBitrate;
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
	int videoBitrate = 6000;
	int audioBitrate = 160;
	std::string encoder; /* empty = same encoder type as the main stream */
	uint32_t width = 0;  /* 0 = use the output resolution */
	uint32_t height = 0;
};

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

	bool Start(const std::string &id);
	void Stop(const std::string &id, bool force = false);
	void StopAll(bool force = false);

	bool Active() const;
	bool IsActive(const std::string &id) const;
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
	};

	/* Member order matters: signals are disconnected before the output is
	 * released, and the output is released before its encoders/service. */
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

	bool enabled = false;
	MultistreamMode mode = MultistreamMode::Shared;
	std::vector<MultistreamDestination> destinations;
	std::filesystem::path configPath;

	OBSWeakOutputAutoRelease primaryOutput;
	bool primarySharedEncoders = true;
	OBSEncoderAutoRelease sharedAudioEncoder;

	std::map<std::string, std::unique_ptr<Running>> running;
	std::map<std::string, StateInfo> states;

	obs_encoder_t *CreateVideoEncoder(const MultistreamDestination &dest, obs_output_t *primary);
	obs_encoder_t *CreateAudioEncoder(const std::string &name, int bitrate, size_t mixer);
	obs_encoder_t *GetSharedAudioEncoder(obs_output_t *primary);
	void SetState(const std::string &id, MultistreamState state, const QString &lastError = QString());

	void OnStarted(const std::string &id);
	void OnStopped(const std::string &id, int code, const QString &lastError);
	void OnReconnect(const std::string &id);
	void OnReconnectSuccess(const std::string &id);

	static void OBSOutputStart(void *data, calldata_t *params);
	static void OBSOutputStop(void *data, calldata_t *params);
	static void OBSOutputReconnect(void *data, calldata_t *params);
	static void OBSOutputReconnectSuccess(void *data, calldata_t *params);
};

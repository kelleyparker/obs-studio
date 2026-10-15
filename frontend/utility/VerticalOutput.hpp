#pragma once

#include <obs.hpp>
#include <utility/audio-encoders.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <string>
#include <vector>

class VerticalCanvas;

/* Recording and replay buffer settings of the vertical canvas, stored in the
 * profile's [Vertical] section. */
struct VerticalOutputSettings {
	std::string recPath;
	std::string recFormat = "hybrid_mp4";
	std::string encoder; /* empty = pick a hardware H.264 encoder automatically */
	int videoBitrate = 8000;
	int audioBitrate = kFixedAudioBitrateKbps;
	int audioTrack = 1; /* 1-6 */
	std::string fileSuffix = "Vertical";
	int replayTimeSec = 30;
	int replaySizeMB = 512;

	static VerticalOutputSettings Load();
	void Save() const;
};

/* Video encoders that can encode the vertical canvas, as {id, display name} */
std::vector<std::pair<std::string, std::string>> GetVerticalVideoEncoders();
/* The encoder used when none is selected (or the selected one is missing) */
std::string GetDefaultVerticalVideoEncoder();

class VerticalOutput : public QObject {
	Q_OBJECT

public:
	explicit VerticalOutput(VerticalCanvas *canvas, QObject *parent = nullptr);
	~VerticalOutput();

	/* On failure, error is empty if the user has already been told why */
	bool StartRecording(QString &error);
	void StopRecording(bool force = false);
	bool RecordingActive() const;
	/* Seconds since the recording started, or 0 */
	uint64_t RecordingSeconds() const;
	QString LastRecordingPath() const { return lastRecordingPath; }

	bool StartReplayBuffer(QString &error);
	void StopReplayBuffer(bool force = false);
	bool ReplayBufferActive() const;
	void SaveReplay();

	bool Active() const { return RecordingActive() || ReplayBufferActive(); }
	void StopAll(bool force);

signals:
	void RecordingStarted();
	void RecordingStopped(int code, const QString &error);
	void ReplayBufferStarted();
	void ReplayBufferStopped(int code, const QString &error);
	void ReplaySaved(const QString &path);

private:
	QPointer<VerticalCanvas> canvas;

	/* Member order matters: signals are disconnected before the outputs are
	 * released, and the outputs are released before the encoders. */
	OBSEncoderAutoRelease videoEncoder;
	OBSEncoderAutoRelease audioEncoder;
	OBSOutputAutoRelease recordOutput;
	OBSOutputAutoRelease replayOutput;
	std::vector<OBSSignal> recordSignals;
	std::vector<OBSSignal> replaySignals;

	uint64_t recordStartTime = 0;
	QString lastRecordingPath;

	bool AcquireEncoders(const VerticalOutputSettings &settings, QString &error);
	void ReleaseIdle();

	void OnRecordingStarted();
	void OnRecordingStopped(int code, const QString &error);
	void OnReplayBufferStarted();
	void OnReplayBufferStopped(int code, const QString &error);
	void OnReplaySaved();

	static void RecordStart(void *data, calldata_t *params);
	static void RecordStop(void *data, calldata_t *params);
	static void ReplayStart(void *data, calldata_t *params);
	static void ReplayStop(void *data, calldata_t *params);
	static void ReplaySavedSignal(void *data, calldata_t *params);
};

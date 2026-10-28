#pragma once

#include <obs.hpp>

#include <QObject>
#include <QString>

#include <vector>

/* Records one OBS source (e.g. a Display Capture that sits in an unused scene)
 * to its own hybrid MP4 next to the main recording, so the YouTube pipeline
 * can cut to it as screen B-roll. The source is rendered through a private
 * obs_view, so the main canvas is untouched. Mic audio (track 1) is included
 * only so the pipeline can line the two files up. */
class YouTubeBrollRecorder : public QObject {
	Q_OBJECT

public:
	explicit YouTubeBrollRecorder(QObject *parent = nullptr);
	~YouTubeBrollRecorder();

	bool Start(const QString &sourceName, QString &error);
	void Stop();
	/* True from Start() until the output has fully stopped */
	bool Active() const { return recordOutput != nullptr; }

	QString LastRecordingPath() const { return lastRecordingPath; }
	/* os_gettime_ns() when the output reported it started, or 0 */
	uint64_t StartTimeNs() const { return startTimeNs; }

signals:
	void Started();
	void Stopped(int code);

private:
	/* Member order matters: signals are disconnected before the output is
	 * released, the output before the encoders, the encoders before the view. */
	OBSView view;
	video_t *viewVideo = nullptr;
	OBSEncoderAutoRelease videoEncoder;
	OBSEncoderAutoRelease audioEncoder;
	OBSOutputAutoRelease recordOutput;
	std::vector<OBSSignal> recordSignals;

	uint64_t startTimeNs = 0;
	QString lastRecordingPath;

	void Release();
	void OnStarted(uint64_t timeNs);
	void OnStopped(int code);

	static void RecordStart(void *data, calldata_t *params);
	static void RecordStop(void *data, calldata_t *params);
};

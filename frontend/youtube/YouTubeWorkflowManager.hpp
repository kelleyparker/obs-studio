#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

class YouTubeBrollRecorder;

struct YouTubeWorkflowConfig {
	// 1-to-4 Priority Ranking (e.g. {"gemini", "openai", "claude", "local"})
	QStringList priorityList = {"gemini", "openai", "claude", "local"};
	QString thumbnailStyle = "ai_graphic"; // "ai_graphic" or "frame_overlay"
	QString whisperModel = "large-v3-turbo";
	QString ollamaModel = "gemma4:12b";

	// Upload defaults
	QString privacyStatus = "unlisted"; // "unlisted", "private", "public"
	QString categoryId = "28";          // 28 = Science & Technology (Default), 22 = People & Blogs, 20 = Gaming
	bool madeForKids = false;
	QString connectedChannelTitle;
	QString clientSecretFilePath;

	// Smart Video Editing Presets
	double silenceDb = -42.0;
	double minSilenceSec = 1.20;
	double paddingSec = 0.25;
	bool cutRetakes = true;

	// Screen B-roll: record this source alongside the webcam and cut to it
	bool brollEnabled = false;
	QString brollSourceName;
	QString brollStyle = "pip"; // "pip" (webcam bubble over the screen) or "cutaway"
	double brollMaxCoverage = 0.40;
	bool brollReview = true;
};

class YouTubeWorkflowManager : public QObject {
	Q_OBJECT

public:
	static YouTubeWorkflowManager *Instance();

	void LoadConfig();
	void SaveConfig();

	YouTubeWorkflowConfig &GetConfig() { return config; }
	const YouTubeWorkflowConfig &GetConfig() const { return config; }

	// Encrypted Vault Key Setters / Checkers (never expose raw keys unless piping to worker)
	void SetVaultKey(const QString &keyName, const QString &plainValue);
	bool HasVaultKey(const QString &keyName) const;
	QString GetVaultKeyDecrypted(const QString &keyName) const;
	void ClearVaultKey(const QString &keyName);

	// One-click migration: imports a global Windows User Environment variable into OBS DPAPI Vault
	// and deletes it from the Windows User Environment so ONLY OBS can access it.
	bool ImportAndRemoveWindowsEnvKey(const QString &envVarName, const QString &vaultKeyName);

	// Workflow state
	void SetWorkflowArmed(bool armed);
	bool IsWorkflowArmed() const { return workflowArmed; }
	bool IsPipelineRunning() const;

	void OnRecordingStarted();
	void OnRecordingStopped(const QString &rawRecordingPath, bool willAutoRemux);
	void OnRemuxFinished(const QString &remuxedMp4Path, bool success);

	void StartPipelineForVideo(const QString &videoPath);
	/* Second half of a run paused for B-roll review (the plan file has been updated) */
	void ResumePipeline(const QString &statePath);
	void StartOAuthLogin();
	void CancelPipeline();

	/* Pushes an edited title/description to the most recently uploaded video. */
	void SubmitVideoEdits(const QString &title, const QString &description);

	QString GetLastVideoPath() const { return lastVideoPath; }
	QString GetLastVideoId() const { return lastVideoId; }

	/* Called by OBSBasic when the main recording output stops */
	void OnMainRecordingStopped();
	/* "YouTube: Mark B-roll moment" hotkey */
	void MarkBrollMoment();

signals:
	void WorkflowArmedChanged(bool armed);
	void PipelineStateChanged(const QString &bannerText, bool running);
	void StepProgressUpdated(const QString &stepName, int percent, const QString &message);
	void MetadataGenerated(const QString &title, const QStringList &alternativeTitles,
			       const QString &description, const QString &hookText,
			       const QString &providerBadge);
	void ThumbnailGenerated(const QString &thumbnailPath, const QString &providerBadge);
	void PipelineCompleted(const QString &videoUrl, const QString &editedVideoPath, const QString &thumbnailPath,
			       const QString &message);
	void UploadStatsReady(const QString &statsText);
	void BrollPlanReady(const QString &planPath, const QString &statePath);
	void VideoUpdateFinished(bool ok, const QString &message);
	void LogMessageReceived(const QString &level, const QString &message);
	void OAuthStatusUpdated(bool connected, const QString &channelTitle, const QString &message);

private:
	explicit YouTubeWorkflowManager(QObject *parent = nullptr);
	~YouTubeWorkflowManager() override;

	QString ResolvePipelineScriptPath() const;
	void HandleStdoutLines();
	void ProcessJsonEvent(const QByteArray &line);
	bool LaunchPipelineWorker(QJsonObject payload);
	void OnBrollRecorderStopped(int code);

	YouTubeWorkflowConfig config;
	bool workflowArmed = false;
	bool waitingForRemux = false;
	QString lastVideoPath;
	QString lastVideoId;
	QByteArray stdoutBuffer;
	QPointer<QProcess> workerProcess;

	YouTubeBrollRecorder *brollRecorder = nullptr;
	uint64_t mainRecordingStartNs = 0;
	QString pendingPipelineVideo; // waiting for the B-roll recording to finish
	struct BrollSession {
		QString screenPath;
		double startDeltaSec = 0.0;
		std::vector<double> markers;
	} brollSession;
};

#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

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
	void StartOAuthLogin();
	void CancelPipeline();

	/* Pushes an edited title/description to the most recently uploaded video. */
	void SubmitVideoEdits(const QString &title, const QString &description);

	QString GetLastVideoPath() const { return lastVideoPath; }
	QString GetLastVideoId() const { return lastVideoId; }

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
	void VideoUpdateFinished(bool ok, const QString &message);
	void LogMessageReceived(const QString &level, const QString &message);
	void OAuthStatusUpdated(bool connected, const QString &channelTitle, const QString &message);

private:
	explicit YouTubeWorkflowManager(QObject *parent = nullptr);
	~YouTubeWorkflowManager() override;

	QString ResolvePipelineScriptPath() const;
	void HandleStdoutLines();
	void ProcessJsonEvent(const QByteArray &line);

	YouTubeWorkflowConfig config;
	bool workflowArmed = false;
	bool waitingForRemux = false;
	QString lastVideoPath;
	QString lastVideoId;
	QByteArray stdoutBuffer;
	QPointer<QProcess> workerProcess;
};

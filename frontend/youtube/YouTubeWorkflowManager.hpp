#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <vector>

class YouTubeBrollRecorder;

struct YouTubeWorkflowConfig {
	// 1-to-4 Priority Ranking (e.g. {"gemini", "openai", "claude", "local"})
	QStringList priorityList = {"gemini", "openai", "claude", "local"};
	QString whisperModel = "large-v3-turbo";
	QString ollamaModel = "gemma4:12b";

	// "step" stops for approval at each review gate; "auto" runs from recording to upload
	QString pipelineMode = "step";
	// AI thumbnail images: "sd" (free local Stable Diffusion), "local" (free Ollama), "gemini" (Nano Banana 2),
	// "gemini_pro" (Nano Banana Pro), "openai" (GPT Image), "claude" (paid) or "off"
	QString thumbnailImageSource = "sd";
	QString thumbnailImageModel = "x/z-image-turbo";
	// When a cloud image source is out of quota or refuses the key, try the other cloud sources that have a key
	bool cloudImageFallback = true;

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

	// How AI thumbnails are tweaked (remembered for the next video): "Leave out" text, and the
	// person composited on top ("none", "video" or "photo") with the photo and side
	QString thumbAvoid;
	QString thumbPersonMode = "none";
	QString thumbPhotoPath;
	QString thumbPersonSide = "right";

	// What Auto mode does at the end: "upload", "premiere" or "both"
	QString autoFinish = "upload";

	// A run paused at a review gate, so the dock can show it again after OBS restarts
	QString pendingReviewGate; // "cuts", "broll", "metadata", "thumbnail" or "publish"
	QString pendingReviewState;
	QString pendingReviewData;
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
	/* Continues a run paused at a review gate (the gate's file has been updated); eview carries
	 * choices that have no file, such as the publish gate's {"action": "upload"|"upload_file"|"finish"} */
	void ResumePipeline(const QString &statePath, const QJsonObject &review = QJsonObject());
	/* At the thumbnail gate: "generate" new AI images, "refine" one, or "recompose" text and photo */
	void RegenerateThumbnails(const QString &action, const QJsonObject &options);
	bool IsRegeneratingThumbnails() const { return thumbRegenActive; }
	/* Starts the last video over from "transcribe", "cuts", "metadata" or "thumbnails", keeping the
	 * recording; optionally deletes its YouTube upload first */
	void RedoPipeline(const QString &from, bool deleteFromYouTube);
	/* Permanently deletes the last uploaded video from YouTube */
	void DeleteUploadedVideo();
	/* Writes a Premiere Pro (FCP 7 XML) sequence of the last edit */
	void ExportToPremiere();
	/* The state file of the last video (paused or finished run), or empty if there is none */
	QString LastStatePath() const;
	/* The rendered edit of the last video and where its cuts are, for the preview player */
	bool GetEditInfo(QString &videoPath, QList<double> &joins) const;
	/* Forgets a paused run without continuing it */
	void DiscardPendingReview();
	bool HasPendingReview() const { return !config.pendingReviewGate.isEmpty(); }

	void SetPipelineMode(const QString &mode);
	void SetThumbnailImageSource(const QString &source);
	void StartOAuthLogin();
	void CancelPipeline();

	/* Pushes an edited title/description to the most recently uploaded video. */
	void SubmitVideoEdits(const QString &title, const QString &description);

	QString GetLastVideoPath() const { return lastVideoPath; }
	QString GetLastVideoId() const { return lastVideoId; }
	QString GetLastVideoUrl() const { return lastVideoUrl; }

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
	/* Step mode: the run is paused until the user approves this gate */
	void ReviewReady(const QString &gate, const QString &statePath, const QString &dataPath);
	void ReviewCleared();
	void PipelineModeChanged(const QString &mode);
	void ThumbnailImageSourceChanged(const QString &source);
	void VideoUpdateFinished(bool ok, const QString &message);
	void VideoDeleteFinished(bool ok, const QString &message);
	/* The edit has been rendered and can be watched */
	void EditReady(const QString &videoPath);
	/* A run is about to render the edit again; anything playing it must let go of the file */
	void EditWillChange();
	void PremiereExported(const QString &xmlPath);
	void ThumbnailRegenStarted();
	void ThumbnailRegenFinished();
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
	void SetPendingReview(const QString &gate, const QString &statePath, const QString &dataPath);
	/* The settings every pipeline run gets (gates, models, thumbnail tweaks, upload defaults) */
	QJsonObject PipelineSettingsPayload() const;
	void SetLastVideo(const QString &path);
	void SetLastUpload(const QString &videoId, const QString &url);

	YouTubeWorkflowConfig config;
	bool workflowArmed = false;
	bool waitingForRemux = false;
	bool thumbRegenActive = false;
	QString lastVideoPath;
	QString lastVideoId;
	QString lastVideoUrl;
	QString lastEditedVideo;
	QList<double> lastJoins;
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

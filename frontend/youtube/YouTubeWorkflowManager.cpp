#include "YouTubeWorkflowManager.hpp"
#include "YouTubeBrollRecorder.hpp"
#include "YouTubeSecretVault.hpp"

#include <OBSApp.hpp>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static YouTubeWorkflowManager *g_ytWorkflowInstance = nullptr;

/* Run the Python worker in UTF-8 mode so its stdio pipes don't fall back to
 * the Windows ANSI code page (cp1252) and fail on emoji or non-Latin text. */
static void SetPythonUtf8Environment(QProcess *process)
{
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert("PYTHONUTF8", "1");
	env.insert("PYTHONIOENCODING", "utf-8");
	process->setProcessEnvironment(env);
}

YouTubeWorkflowManager *YouTubeWorkflowManager::Instance()
{
	if (!g_ytWorkflowInstance) {
		g_ytWorkflowInstance = new YouTubeWorkflowManager(qApp);
	}
	return g_ytWorkflowInstance;
}

YouTubeWorkflowManager::YouTubeWorkflowManager(QObject *parent) : QObject(parent)
{
	LoadConfig();

	/* The last video and its upload, so Start Over, Delete and Export still work after a restart */
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		lastVideoPath = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "LastVideoPath"));
		lastVideoId = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "LastVideoId"));
		lastVideoUrl = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "LastVideoUrl"));
	}

	brollRecorder = new YouTubeBrollRecorder(this);
	connect(brollRecorder, &YouTubeBrollRecorder::Stopped, this, &YouTubeWorkflowManager::OnBrollRecorderStopped);
}

YouTubeWorkflowManager::~YouTubeWorkflowManager()
{
	CancelPipeline();
}

void YouTubeWorkflowManager::LoadConfig()
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return;
	}

	const char *prioStr = config_get_string(cfg, "YouTubeWorkflow", "PriorityList");
	if (prioStr && *prioStr) {
		QStringList parsed = QString::fromUtf8(prioStr).split(',', Qt::SkipEmptyParts);
		if (!parsed.isEmpty()) {
			config.priorityList = parsed;
		}
	}

	const char *mode = config_get_string(cfg, "YouTubeWorkflow", "PipelineMode");
	if (mode && *mode) {
		config.pipelineMode = QString::fromUtf8(mode);
	}

	const char *imageSource = config_get_string(cfg, "YouTubeWorkflow", "ThumbnailImageSource");
	if (imageSource && *imageSource) {
		config.thumbnailImageSource = QString::fromUtf8(imageSource);
	}

	const char *imageModel = config_get_string(cfg, "YouTubeWorkflow", "ThumbnailImageModel");
	if (imageModel && *imageModel) {
		config.thumbnailImageModel = QString::fromUtf8(imageModel);
	}

	config.thumbAvoid = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "ThumbAvoid"));
	config.thumbPhotoPath = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "ThumbPhotoPath"));
	const char *personMode = config_get_string(cfg, "YouTubeWorkflow", "ThumbPersonMode");
	if (personMode && *personMode) {
		config.thumbPersonMode = QString::fromUtf8(personMode);
	}
	const char *personSide = config_get_string(cfg, "YouTubeWorkflow", "ThumbPersonSide");
	if (personSide && *personSide) {
		config.thumbPersonSide = QString::fromUtf8(personSide);
	}
	const char *autoFinish = config_get_string(cfg, "YouTubeWorkflow", "AutoFinish");
	if (autoFinish && *autoFinish) {
		config.autoFinish = QString::fromUtf8(autoFinish);
	}

	config.pendingReviewGate = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "PendingReviewGate"));
	config.pendingReviewState = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "PendingReviewState"));
	config.pendingReviewData = QString::fromUtf8(config_get_string(cfg, "YouTubeWorkflow", "PendingReviewData"));

	const char *whisper = config_get_string(cfg, "YouTubeWorkflow", "WhisperModel");
	if (whisper && *whisper) {
		config.whisperModel = QString::fromUtf8(whisper);
	}

	const char *ollama = config_get_string(cfg, "YouTubeWorkflow", "OllamaModel");
	if (ollama && *ollama) {
		config.ollamaModel = QString::fromUtf8(ollama);
	}

	const char *privacy = config_get_string(cfg, "YouTubeWorkflow", "PrivacyStatus");
	if (privacy && *privacy) {
		config.privacyStatus = QString::fromUtf8(privacy);
	}

	const char *catId = config_get_string(cfg, "YouTubeWorkflow", "CategoryId");
	if (catId && *catId) {
		config.categoryId = QString::fromUtf8(catId);
	}

	config.madeForKids = config_get_bool(cfg, "YouTubeWorkflow", "MadeForKids");

	const char *channelTitle = config_get_string(cfg, "YouTubeWorkflow", "ConnectedChannelTitle");
	if (channelTitle) {
		config.connectedChannelTitle = QString::fromUtf8(channelTitle);
	}

	const char *secretFile = config_get_string(cfg, "YouTubeWorkflow", "ClientSecretFilePath");
	if (secretFile) {
		config.clientSecretFilePath = QString::fromUtf8(secretFile);
	}

	if (config_has_user_value(cfg, "YouTubeWorkflow", "SilenceDb")) {
		config.silenceDb = config_get_double(cfg, "YouTubeWorkflow", "SilenceDb");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "MinSilenceSec")) {
		config.minSilenceSec = config_get_double(cfg, "YouTubeWorkflow", "MinSilenceSec");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "PaddingSec")) {
		config.paddingSec = config_get_double(cfg, "YouTubeWorkflow", "PaddingSec");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "CutRetakes")) {
		config.cutRetakes = config_get_bool(cfg, "YouTubeWorkflow", "CutRetakes");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "NormalizeLoudness")) {
		config.normalizeLoudness = config_get_bool(cfg, "YouTubeWorkflow", "NormalizeLoudness");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "CloudImageFallback")) {
		config.cloudImageFallback = config_get_bool(cfg, "YouTubeWorkflow", "CloudImageFallback");
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "MusicEnabled")) {
		config.musicEnabled = config_get_bool(cfg, "YouTubeWorkflow", "MusicEnabled");
	}

	config.brollEnabled = config_get_bool(cfg, "YouTubeWorkflow", "BrollEnabled");
	const char *brollSource = config_get_string(cfg, "YouTubeWorkflow", "BrollSource");
	if (brollSource) {
		config.brollSourceName = QString::fromUtf8(brollSource);
	}
	const char *brollStyle = config_get_string(cfg, "YouTubeWorkflow", "BrollStyle");
	if (brollStyle && *brollStyle) {
		config.brollStyle = QString::fromUtf8(brollStyle);
	}
	if (config_has_user_value(cfg, "YouTubeWorkflow", "BrollMaxCoverage")) {
		config.brollMaxCoverage = config_get_double(cfg, "YouTubeWorkflow", "BrollMaxCoverage");
	}
}

void YouTubeWorkflowManager::SaveConfig()
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return;
	}

	QByteArray prioJoined = config.priorityList.join(',').toUtf8();
	config_set_string(cfg, "YouTubeWorkflow", "PriorityList", prioJoined.constData());
	config_set_string(cfg, "YouTubeWorkflow", "PipelineMode", config.pipelineMode.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbnailImageSource",
			  config.thumbnailImageSource.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbnailImageModel",
			  config.thumbnailImageModel.toUtf8().constData());
	config_set_bool(cfg, "YouTubeWorkflow", "CloudImageFallback", config.cloudImageFallback);
	config_set_bool(cfg, "YouTubeWorkflow", "MusicEnabled", config.musicEnabled);
	config_set_string(cfg, "YouTubeWorkflow", "ThumbAvoid", config.thumbAvoid.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbPhotoPath", config.thumbPhotoPath.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbPersonMode", config.thumbPersonMode.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbPersonSide", config.thumbPersonSide.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "AutoFinish", config.autoFinish.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "PendingReviewGate", config.pendingReviewGate.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "PendingReviewState",
			  config.pendingReviewState.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "PendingReviewData", config.pendingReviewData.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "WhisperModel", config.whisperModel.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "OllamaModel", config.ollamaModel.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "PrivacyStatus", config.privacyStatus.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "CategoryId", config.categoryId.toUtf8().constData());
	config_set_bool(cfg, "YouTubeWorkflow", "MadeForKids", config.madeForKids);
	config_set_string(cfg, "YouTubeWorkflow", "ConnectedChannelTitle",
			  config.connectedChannelTitle.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "ClientSecretFilePath",
			  config.clientSecretFilePath.toUtf8().constData());
	config_set_double(cfg, "YouTubeWorkflow", "SilenceDb", config.silenceDb);
	config_set_double(cfg, "YouTubeWorkflow", "MinSilenceSec", config.minSilenceSec);
	config_set_double(cfg, "YouTubeWorkflow", "PaddingSec", config.paddingSec);
	config_set_bool(cfg, "YouTubeWorkflow", "CutRetakes", config.cutRetakes);
	config_set_bool(cfg, "YouTubeWorkflow", "NormalizeLoudness", config.normalizeLoudness);
	config_set_bool(cfg, "YouTubeWorkflow", "BrollEnabled", config.brollEnabled);
	config_set_string(cfg, "YouTubeWorkflow", "BrollSource", config.brollSourceName.toUtf8().constData());
	config_set_string(cfg, "YouTubeWorkflow", "BrollStyle", config.brollStyle.toUtf8().constData());
	config_set_double(cfg, "YouTubeWorkflow", "BrollMaxCoverage", config.brollMaxCoverage);

	config_save_safe(cfg, "tmp", nullptr);
}

void YouTubeWorkflowManager::SetVaultKey(const QString &keyName, const QString &plainValue)
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return;
	}

	QByteArray settingName = ("Vault_" + keyName).toUtf8();
	if (plainValue.trimmed().isEmpty()) {
		config_set_string(cfg, "YouTubeWorkflow", settingName.constData(), "");
	} else {
		QString encrypted = YouTubeSecretVault::EncryptSecret(plainValue.trimmed());
		config_set_string(cfg, "YouTubeWorkflow", settingName.constData(), encrypted.toUtf8().constData());
	}
	config_save_safe(cfg, "tmp", nullptr);
}

bool YouTubeWorkflowManager::HasVaultKey(const QString &keyName) const
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return false;
	}
	QByteArray settingName = ("Vault_" + keyName).toUtf8();
	const char *val = config_get_string(cfg, "YouTubeWorkflow", settingName.constData());
	return val && *val != '\0';
}

QString YouTubeWorkflowManager::GetVaultKeyDecrypted(const QString &keyName) const
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return QString();
	}
	QByteArray settingName = ("Vault_" + keyName).toUtf8();
	const char *val = config_get_string(cfg, "YouTubeWorkflow", settingName.constData());
	if (!val || !*val) {
		return QString();
	}
	return YouTubeSecretVault::DecryptSecret(QString::fromUtf8(val));
}

void YouTubeWorkflowManager::ClearVaultKey(const QString &keyName)
{
	SetVaultKey(keyName, QString());
}

bool YouTubeWorkflowManager::ImportAndRemoveWindowsEnvKey(const QString &envVarName, const QString &vaultKeyName)
{
	QSettings userEnv("HKEY_CURRENT_USER\\Environment", QSettings::NativeFormat);
	QString secret = userEnv.value(envVarName).toString();
	if (secret.isEmpty()) {
		secret = qEnvironmentVariable(envVarName.toUtf8().constData());
	}
	if (secret.trimmed().isEmpty()) {
		return false;
	}

	SetVaultKey(vaultKeyName, secret);
	YouTubeSecretVault::WipeString(secret);

	// Remove from Windows User Environment registry & current process env
	userEnv.remove(envVarName);
	userEnv.sync();
	qunsetenv(envVarName.toUtf8().constData());
#ifdef _WIN32
	SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
			    SMTO_ABORTIFHUNG, 1000, nullptr);
#endif
	return true;
}

void YouTubeWorkflowManager::SetWorkflowArmed(bool armed)
{
	if (workflowArmed != armed) {
		workflowArmed = armed;
		emit WorkflowArmedChanged(workflowArmed);
	}
}

bool YouTubeWorkflowManager::IsPipelineRunning() const
{
	return workerProcess && workerProcess->state() != QProcess::NotRunning;
}

void YouTubeWorkflowManager::OnRecordingStarted()
{
	mainRecordingStartNs = os_gettime_ns();
	if (!workflowArmed) {
		return;
	}

	emit PipelineStateChanged("🔴 Recording Active — Click 'Stop Recording' (or ⏹ YT) to Edit & Upload", false);
	emit LogMessageReceived("info", "YouTube Workflow armed: Recording in progress...");

	LoadConfig();
	brollSession = BrollSession();
	if (!config.brollEnabled) {
		return;
	}
	if (config.brollSourceName.isEmpty()) {
		emit LogMessageReceived("warning", "B-roll is on but no screen source is selected; skipping B-roll.");
		return;
	}
	QString error;
	if (brollRecorder->Start(config.brollSourceName, error)) {
		emit LogMessageReceived("info",
					QString("Recording '%1' as screen B-roll.").arg(config.brollSourceName));
	} else {
		emit LogMessageReceived("warning", QString("B-roll recording not started: %1").arg(error));
	}
}

void YouTubeWorkflowManager::OnMainRecordingStopped()
{
	uint64_t mainStartNs = mainRecordingStartNs;
	mainRecordingStartNs = 0;
	if (!brollRecorder->Active()) {
		return;
	}
	/* Seconds the screen recording started after the webcam one; the
	 * pipeline refines this by matching the mic audio in both files. */
	brollSession.screenPath = brollRecorder->LastRecordingPath();
	if (brollRecorder->StartTimeNs() && mainStartNs) {
		brollSession.startDeltaSec = (double(brollRecorder->StartTimeNs()) - double(mainStartNs)) / 1e9;
	}
	brollRecorder->Stop();
}

void YouTubeWorkflowManager::MarkBrollMoment()
{
	if (!mainRecordingStartNs || !obs_frontend_recording_active()) {
		return;
	}
	double t = double(os_gettime_ns() - mainRecordingStartNs) / 1e9;
	brollSession.markers.push_back(t);
	obs_frontend_recording_add_chapter("B-roll");
	emit LogMessageReceived("info", QString("B-roll moment marked at %1:%2")
						.arg(int(t) / 60, 2, 10, QChar('0'))
						.arg(int(t) % 60, 2, 10, QChar('0')));
}

void YouTubeWorkflowManager::OnBrollRecorderStopped(int code)
{
	if (code != OBS_OUTPUT_SUCCESS) {
		emit LogMessageReceived("warning",
					QString("B-roll recording ended with an error (code %1).").arg(code));
	}
	/* The recorder has remuxed its MKV to MP4 by now */
	if (!brollSession.screenPath.isEmpty()) {
		brollSession.screenPath = brollRecorder->LastRecordingPath();
	}
	if (!pendingPipelineVideo.isEmpty()) {
		QString video = pendingPipelineVideo;
		pendingPipelineVideo.clear();
		StartPipelineForVideo(video);
	}
}

void YouTubeWorkflowManager::OnRecordingStopped(const QString &rawRecordingPath, bool willAutoRemux)
{
	if (!workflowArmed) {
		return;
	}

	SetWorkflowArmed(false);
	SetLastVideo(rawRecordingPath);

	if (willAutoRemux) {
		waitingForRemux = true;
		emit PipelineStateChanged("📹 Auto-Remuxing MKV to MP4...", true);
		emit StepProgressUpdated("remux", 50, "Remuxing MKV recording to MP4...");
		emit LogMessageReceived("info", "Recording stopped. Waiting for automatic MP4 remux to complete...");
	} else {
		waitingForRemux = false;
		emit StepProgressUpdated("remux", 100, "MP4 ready.");
		StartPipelineForVideo(rawRecordingPath);
	}
}

void YouTubeWorkflowManager::OnRemuxFinished(const QString &remuxedMp4Path, bool success)
{
	if (!waitingForRemux) {
		return;
	}
	waitingForRemux = false;

	if (!success || remuxedMp4Path.isEmpty()) {
		emit PipelineStateChanged("⚠️ Auto-Remux failed — Pipeline aborted", false);
		emit LogMessageReceived("error", "Auto-Remux failed; cannot run YouTube workflow.");
		return;
	}

	SetLastVideo(remuxedMp4Path);
	emit StepProgressUpdated("remux", 100, QString("Remuxed to %1").arg(QFileInfo(remuxedMp4Path).fileName()));
	StartPipelineForVideo(remuxedMp4Path);
}

QString YouTubeWorkflowManager::ResolvePipelineScriptPath() const
{
	QString appDir = QCoreApplication::applicationDirPath();
	QStringList candidates = {
		QDir::cleanPath(appDir + "/../../data/obs-studio/scripts/youtube_pipeline.py"),
		QDir::cleanPath(appDir + "/scripts/youtube_pipeline.py"),
		QStringLiteral("C:/Users/cliffgoat/git/obs-studio/frontend/scripts/youtube_pipeline.py")};
	for (const QString &c : candidates) {
		if (QFileInfo::exists(c)) {
			return c;
		}
	}
	return candidates.last();
}

QJsonObject YouTubeWorkflowManager::PipelineSettingsPayload() const
{
	bool stepMode = config.pipelineMode == "step";
	QJsonArray prioArr;
	for (const QString &p : config.priorityList) {
		prioArr.append(p);
	}

	QJsonObject thumbOptions;
	thumbOptions["avoid"] = config.thumbAvoid;
	thumbOptions["person"] = config.thumbPersonMode;
	thumbOptions["photo_path"] = config.thumbPhotoPath;
	thumbOptions["side"] = config.thumbPersonSide;

	QJsonObject payload;
	payload["priority_list"] = prioArr;
	payload["thumbnail_image_source"] = config.thumbnailImageSource;
	payload["thumbnail_image_model"] = config.thumbnailImageModel;
	payload["cloud_image_fallback"] = config.cloudImageFallback;
	payload["music_enabled"] = config.musicEnabled;
	payload["thumb_options"] = thumbOptions;
	payload["auto_finish"] = config.autoFinish;
	payload["review_gates"] = stepMode ? QJsonArray{"cuts", "broll", "metadata", "music", "thumbnail", "publish"}
					   : QJsonArray();
	payload["whisper_model"] = config.whisperModel;
	payload["ollama_model"] = config.ollamaModel;
	payload["privacy_status"] = config.privacyStatus;
	payload["category_id"] = config.categoryId;
	payload["made_for_kids"] = config.madeForKids;
	payload["silence_db"] = config.silenceDb;
	payload["min_silence_sec"] = config.minSilenceSec;
	payload["padding_sec"] = config.paddingSec;
	payload["cut_retakes"] = config.cutRetakes;
	payload["normalize_loudness"] = config.normalizeLoudness;
	return payload;
}

void YouTubeWorkflowManager::SetLastVideo(const QString &path)
{
	lastVideoPath = path;
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		config_set_string(cfg, "YouTubeWorkflow", "LastVideoPath", path.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
}

void YouTubeWorkflowManager::SetLastUpload(const QString &videoId, const QString &url)
{
	lastVideoId = videoId;
	lastVideoUrl = url;
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		config_set_string(cfg, "YouTubeWorkflow", "LastVideoId", videoId.toUtf8().constData());
		config_set_string(cfg, "YouTubeWorkflow", "LastVideoUrl", url.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
}

QString YouTubeWorkflowManager::LastStatePath() const
{
	if (lastVideoPath.isEmpty()) {
		return QString();
	}
	QFileInfo video(lastVideoPath);
	/* The pipeline names it after the MP4 it worked on: {stem}_yt_state.json */
	QString state = video.absoluteDir().filePath(video.completeBaseName() + "_yt_state.json");
	return QFileInfo::exists(state) ? state : QString();
}

bool YouTubeWorkflowManager::GetEditInfo(QString &videoPath, QList<double> &joins) const
{
	if (!lastEditedVideo.isEmpty() && QFileInfo::exists(lastEditedVideo)) {
		videoPath = lastEditedVideo;
		joins = lastJoins;
		return true;
	}
	QFile file(LastStatePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	QJsonObject ctx = QJsonDocument::fromJson(file.readAll()).object().value("ctx").toObject();
	videoPath = ctx.value("final_video").toString();
	joins.clear();
	for (const QJsonValue &v : ctx.value("joins").toArray()) {
		joins.append(v.toDouble());
	}
	return !videoPath.isEmpty() && QFileInfo::exists(videoPath);
}

void YouTubeWorkflowManager::StartPipelineForVideo(const QString &videoPath)
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}
	if (brollRecorder->Active()) {
		pendingPipelineVideo = videoPath;
		emit LogMessageReceived("info", "Waiting for the B-roll screen recording to finish...");
		return;
	}

	LoadConfig();
	DiscardPendingReview();
	emit EditWillChange();
	SetLastVideo(videoPath);
	lastVideoId.clear();
	lastVideoUrl.clear();
	lastEditedVideo.clear();
	lastJoins.clear();

	bool stepMode = config.pipelineMode == "step";
	emit PipelineStateChanged(stepMode ? "⚙️ Step mode: analyzing, then waiting for you to review the cuts..."
					   : "⚙️ Auto mode: edit, thumbnail & upload without stopping...",
				  true);
	emit StepProgressUpdated("remux", 100, "MP4 Ready");
	emit StepProgressUpdated("transcribe", 0, "Queued...");
	emit StepProgressUpdated("edit", 0, "Queued...");
	emit StepProgressUpdated("broll", 0, "Queued...");
	emit StepProgressUpdated("llm", 0, "Queued...");
	emit StepProgressUpdated("music", 0, config.musicEnabled ? "Queued..." : "Off");
	emit StepProgressUpdated("thumbnail", 0, "Queued...");
	emit StepProgressUpdated("upload", 0, "Queued...");

	QJsonObject payload = PipelineSettingsPayload();
	payload["mode"] = "pipeline";
	payload["input_video"] = videoPath;

	if (config.brollEnabled && !brollSession.screenPath.isEmpty() && QFileInfo::exists(brollSession.screenPath)) {
		QJsonArray markers;
		for (double m : brollSession.markers) {
			markers.append(m);
		}
		QJsonObject broll;
		broll["enabled"] = true;
		broll["screen_video"] = brollSession.screenPath;
		broll["start_delta_sec"] = brollSession.startDeltaSec;
		broll["markers"] = markers;
		broll["style"] = config.brollStyle;
		broll["max_coverage"] = config.brollMaxCoverage;
		broll["review"] = stepMode;
		payload["broll"] = broll;
	}

	LaunchPipelineWorker(payload);
}

void YouTubeWorkflowManager::ResumePipeline(const QString &statePathRef, const QJsonObject &review)
{
	/* Copy first: callers may pass a review field that our own signals clear below */
	const QString statePath = statePathRef;
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}
	LoadConfig();
	QString gate = config.pendingReviewGate;
	SetPendingReview(QString(), QString(), QString());
	/* The music gate mixes the song into {stem}_yt_final.mp4, which the player may have open */
	if (gate == "cuts" || gate == "broll" || gate == "music") {
		emit EditWillChange();
	}

	emit PipelineStateChanged("⚙️ Continuing the pipeline...", true);
	/* Reset only the steps that still have to run after this gate */
	QStringList steps = {"edit", "broll", "llm", "music", "thumbnail", "upload"};
	qsizetype from = 0;
	if (gate == "metadata") {
		from = 3;
	} else if (gate == "music") {
		from = 4;
	} else if (gate == "thumbnail" || gate == "publish") {
		from = 5;
	}
	for (qsizetype i = from; i < steps.size(); i++) {
		emit StepProgressUpdated(steps[i], 0, "Queued...");
	}

	QJsonObject payload;
	payload["mode"] = "resume_pipeline";
	payload["state_path"] = statePath;
	if (!review.isEmpty()) {
		payload["review"] = review;
	}
	LaunchPipelineWorker(payload);
}

void YouTubeWorkflowManager::RegenerateThumbnails(const QString &action, const QJsonObject &options)
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}
	if (config.pendingReviewGate != "thumbnail") {
		emit LogMessageReceived("warning", "No job is waiting at the thumbnail step.");
		return;
	}

	/* Remember the tweaks for the next video */
	config.thumbAvoid = options.value("avoid").toString(config.thumbAvoid);
	config.thumbPersonMode = options.value("person").toString(config.thumbPersonMode);
	config.thumbPhotoPath = options.value("photo_path").toString(config.thumbPhotoPath);
	config.thumbPersonSide = options.value("side").toString(config.thumbPersonSide);
	SaveConfig();

	thumbRegenActive = true;
	emit ThumbnailRegenStarted();
	emit StepProgressUpdated("thumbnail", 0, "Starting...");

	QJsonObject payload;
	payload["mode"] = "thumbnail_regen";
	payload["state_path"] = config.pendingReviewState;
	payload["action"] = action;
	payload["options"] = options;
	if (!LaunchPipelineWorker(payload)) {
		thumbRegenActive = false;
		emit ThumbnailRegenFinished();
	}
}

void YouTubeWorkflowManager::RegenerateMusic(const QJsonObject &options)
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}
	if (config.pendingReviewGate != "music") {
		emit LogMessageReceived("warning", "No job is waiting at the music step.");
		return;
	}

	musicRegenActive = true;
	emit MusicRegenStarted();
	emit StepProgressUpdated("music", 0, "Starting...");

	QJsonObject payload;
	payload["mode"] = "music_regen";
	payload["state_path"] = config.pendingReviewState;
	payload["options"] = options;
	if (!LaunchPipelineWorker(payload)) {
		musicRegenActive = false;
		emit MusicRegenFinished();
	}
}

void YouTubeWorkflowManager::RedoPipeline(const QString &from, bool deleteFromYouTube)
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}
	QString statePath = LastStatePath();
	if (statePath.isEmpty()) {
		/* Nothing saved yet: all that can be done is a fresh run */
		if (!lastVideoPath.isEmpty() && QFileInfo::exists(lastVideoPath)) {
			StartPipelineForVideo(lastVideoPath);
		}
		return;
	}

	LoadConfig();
	DiscardPendingReview();
	/* Every redo up to the music may write the edit or {stem}_yt_final.mp4 again */
	if (from != "thumbnails") {
		emit EditWillChange();
	}
	lastEditedVideo.clear();
	lastJoins.clear();

	const QStringList steps = {"transcribe", "edit", "broll", "llm", "music", "thumbnail", "upload"};
	const QStringList starts = {"transcribe", "cuts", "", "metadata", "music", "thumbnails"};
	qsizetype first = starts.indexOf(from);
	if (first < 0) {
		first = 0;
	}
	for (qsizetype i = first; i < steps.size(); i++) {
		emit StepProgressUpdated(steps[i], 0, "Queued...");
	}
	emit PipelineStateChanged(deleteFromYouTube && !lastVideoId.isEmpty()
					  ? "🗑 Deleting the YouTube upload, then starting over..."
					  : "🔄 Starting over (your recording is kept)...",
				  true);

	QJsonObject payload = PipelineSettingsPayload();
	payload["mode"] = "redo_pipeline";
	payload["state_path"] = statePath;
	payload["from"] = from;
	if (deleteFromYouTube && !lastVideoId.isEmpty()) {
		payload["delete_video_id"] = lastVideoId;
	}
	LaunchPipelineWorker(payload);
}

void YouTubeWorkflowManager::DeleteUploadedVideo()
{
	if (IsPipelineRunning()) {
		emit VideoDeleteFinished(false, "Wait for the current job to finish first.");
		return;
	}
	if (lastVideoId.isEmpty()) {
		emit VideoDeleteFinished(false, "There's no uploaded video to delete.");
		return;
	}
	QJsonObject payload;
	payload["mode"] = "delete_video";
	payload["video_id"] = lastVideoId;
	LaunchPipelineWorker(payload);
}

void YouTubeWorkflowManager::ExportToPremiere()
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "Wait for the current job to finish before exporting.");
		return;
	}
	QString statePath = HasPendingReview() ? config.pendingReviewState : LastStatePath();
	if (statePath.isEmpty() || !QFileInfo::exists(statePath)) {
		emit LogMessageReceived("warning", "There's no edit to export yet.");
		return;
	}
	emit LogMessageReceived("info", "Exporting the edit for Premiere Pro...");
	QJsonObject payload;
	payload["mode"] = "export_premiere";
	payload["state_path"] = statePath;
	LaunchPipelineWorker(payload);
}

void YouTubeWorkflowManager::SetPendingReview(const QString &gate, const QString &statePath, const QString &dataPath)
{
	config.pendingReviewGate = gate;
	config.pendingReviewState = statePath;
	config.pendingReviewData = dataPath;

	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		config_set_string(cfg, "YouTubeWorkflow", "PendingReviewGate", gate.toUtf8().constData());
		config_set_string(cfg, "YouTubeWorkflow", "PendingReviewState", statePath.toUtf8().constData());
		config_set_string(cfg, "YouTubeWorkflow", "PendingReviewData", dataPath.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
}

void YouTubeWorkflowManager::DiscardPendingReview()
{
	if (config.pendingReviewGate.isEmpty()) {
		return;
	}
	SetPendingReview(QString(), QString(), QString());
	emit ReviewCleared();
}

void YouTubeWorkflowManager::SetPipelineMode(const QString &mode)
{
	if (config.pipelineMode == mode) {
		return;
	}
	config.pipelineMode = mode;
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		config_set_string(cfg, "YouTubeWorkflow", "PipelineMode", mode.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
	emit PipelineModeChanged(mode);
}

void YouTubeWorkflowManager::SetThumbnailImageSource(const QString &source)
{
	if (config.thumbnailImageSource == source) {
		return;
	}
	config.thumbnailImageSource = source;
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (cfg) {
		config_set_string(cfg, "YouTubeWorkflow", "ThumbnailImageSource", source.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
	emit ThumbnailImageSourceChanged(source);
}

bool YouTubeWorkflowManager::LaunchPipelineWorker(QJsonObject payload)
{
	QString scriptPath = ResolvePipelineScriptPath();
	if (!QFileInfo::exists(scriptPath)) {
		emit PipelineStateChanged("❌ Pipeline script missing", false);
		emit LogMessageReceived("error", QString("Could not find script at: %1").arg(scriptPath));
		return false;
	}

	workerProcess = new QProcess(this);
	stdoutBuffer.clear();

	connect(workerProcess, &QProcess::readyReadStandardOutput, this, &YouTubeWorkflowManager::HandleStdoutLines);
	connect(workerProcess, &QProcess::readyReadStandardError, this, [this]() {
		if (!workerProcess) {
			return;
		}
		QString errText = QString::fromUtf8(workerProcess->readAllStandardError()).trimmed();
		if (!errText.isEmpty()) {
			emit LogMessageReceived("warning", errText);
		}
	});
	connect(workerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
		[this](int exitCode, QProcess::ExitStatus status) {
			HandleStdoutLines();
			if (status == QProcess::CrashExit || exitCode != 0) {
				emit PipelineStateChanged(
					QString("⚠️ Pipeline exited (code %1) — Check log below").arg(exitCode), false);
			}
			if (workerProcess) {
				workerProcess->deleteLater();
				workerProcess = nullptr;
			}
			if (thumbRegenActive) {
				thumbRegenActive = false;
				emit ThumbnailRegenFinished();
			}
			if (musicRegenActive) {
				musicRegenActive = false;
				emit MusicRegenFinished();
			}
		});

	SetPythonUtf8Environment(workerProcess);
	workerProcess->start(QStringLiteral("python"), {scriptPath, QStringLiteral("--stdin-config")});
	if (!workerProcess->waitForStarted(5000)) {
		emit PipelineStateChanged("❌ Failed to start Python worker process", false);
		emit LogMessageReceived("error", "Failed to start python.exe for YouTube pipeline.");
		workerProcess->deleteLater();
		workerProcess = nullptr;
		return false;
	}

	// Decrypt vault keys strictly in local scope, pipe over private stdin, and wipe immediately
	QString geminiKey = GetVaultKeyDecrypted("GeminiKey");
	QString openaiKey = GetVaultKeyDecrypted("OpenAIKey");
	QString claudeKey = GetVaultKeyDecrypted("ClaudeKey");
	QString ytTokenJson = GetVaultKeyDecrypted("YouTubeTokenJson");

	QJsonObject secretsObj;
	secretsObj["gemini_key"] = geminiKey;
	secretsObj["openai_key"] = openaiKey;
	secretsObj["claude_key"] = claudeKey;
	secretsObj["youtube_token_json"] = ytTokenJson;

	YouTubeSecretVault::WipeString(geminiKey);
	YouTubeSecretVault::WipeString(openaiKey);
	YouTubeSecretVault::WipeString(claudeKey);
	YouTubeSecretVault::WipeString(ytTokenJson);

	payload["secrets"] = secretsObj;

	QByteArray jsonLine = QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n";
	workerProcess->write(jsonLine);
	workerProcess->closeWriteChannel();

#ifdef _WIN32
	SecureZeroMemory(jsonLine.data(), jsonLine.size());
#endif
	return true;
}

void YouTubeWorkflowManager::StartOAuthLogin()
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "Wait for current job to finish before running OAuth login.");
		return;
	}

	LoadConfig();
	QString scriptPath = ResolvePipelineScriptPath();

	workerProcess = new QProcess(this);
	stdoutBuffer.clear();

	connect(workerProcess, &QProcess::readyReadStandardOutput, this, &YouTubeWorkflowManager::HandleStdoutLines);
	connect(workerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
		[this](int, QProcess::ExitStatus) {
			HandleStdoutLines();
			if (workerProcess) {
				workerProcess->deleteLater();
				workerProcess = nullptr;
			}
		});

	SetPythonUtf8Environment(workerProcess);
	workerProcess->start(QStringLiteral("python"), {scriptPath, QStringLiteral("--stdin-config")});
	if (!workerProcess->waitForStarted(5000)) {
		emit OAuthStatusUpdated(false, QString(), "Failed to launch Python for Google OAuth.");
		return;
	}

	QString clientId = GetVaultKeyDecrypted("YouTubeClientId");
	QString clientSecret = GetVaultKeyDecrypted("YouTubeClientSecret");

	QJsonObject payload;
	payload["mode"] = "oauth_login";
	payload["client_id"] = clientId;
	payload["client_secret"] = clientSecret;
	payload["client_secret_file"] = config.clientSecretFilePath;

	YouTubeSecretVault::WipeString(clientId);
	YouTubeSecretVault::WipeString(clientSecret);

	QByteArray jsonLine = QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n";
	workerProcess->write(jsonLine);
	workerProcess->closeWriteChannel();
#ifdef _WIN32
	SecureZeroMemory(jsonLine.data(), jsonLine.size());
#endif
}

void YouTubeWorkflowManager::SubmitVideoEdits(const QString &title, const QString &description)
{
	if (IsPipelineRunning()) {
		emit VideoUpdateFinished(false, "Wait for the current job to finish before submitting.");
		return;
	}
	if (lastVideoId.isEmpty()) {
		emit VideoUpdateFinished(false, "No uploaded video to update yet.");
		return;
	}

	QString scriptPath = ResolvePipelineScriptPath();
	if (!QFileInfo::exists(scriptPath)) {
		emit VideoUpdateFinished(false, QString("Could not find script at: %1").arg(scriptPath));
		return;
	}

	workerProcess = new QProcess(this);
	stdoutBuffer.clear();

	connect(workerProcess, &QProcess::readyReadStandardOutput, this, &YouTubeWorkflowManager::HandleStdoutLines);
	connect(workerProcess, &QProcess::readyReadStandardError, this, [this]() {
		if (!workerProcess) {
			return;
		}
		QString errText = QString::fromUtf8(workerProcess->readAllStandardError()).trimmed();
		if (!errText.isEmpty()) {
			emit LogMessageReceived("warning", errText);
		}
	});
	connect(workerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
		[this](int, QProcess::ExitStatus status) {
			HandleStdoutLines();
			if (status == QProcess::CrashExit) {
				emit VideoUpdateFinished(false, "The YouTube update worker crashed.");
			}
			if (workerProcess) {
				workerProcess->deleteLater();
				workerProcess = nullptr;
			}
		});

	SetPythonUtf8Environment(workerProcess);
	workerProcess->start(QStringLiteral("python"), {scriptPath, QStringLiteral("--stdin-config")});
	if (!workerProcess->waitForStarted(5000)) {
		emit VideoUpdateFinished(false, "Failed to start python.exe for the YouTube update.");
		workerProcess->deleteLater();
		workerProcess = nullptr;
		return;
	}

	QString ytTokenJson = GetVaultKeyDecrypted("YouTubeTokenJson");
	QJsonObject secretsObj;
	secretsObj["youtube_token_json"] = ytTokenJson;
	YouTubeSecretVault::WipeString(ytTokenJson);

	QJsonObject payload;
	payload["mode"] = "update_video";
	payload["video_id"] = lastVideoId;
	payload["title"] = title;
	payload["description"] = description;
	payload["secrets"] = secretsObj;

	QByteArray jsonLine = QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n";
	workerProcess->write(jsonLine);
	workerProcess->closeWriteChannel();
#ifdef _WIN32
	SecureZeroMemory(jsonLine.data(), jsonLine.size());
#endif
}

void YouTubeWorkflowManager::CancelPipeline()
{
	if (thumbRegenActive && workerProcess) {
		/* Stop only the new images; the job keeps waiting with the thumbnails it had */
		workerProcess->kill();
		workerProcess->waitForFinished(2000);
		workerProcess->deleteLater();
		workerProcess = nullptr;
		thumbRegenActive = false;
		emit ThumbnailRegenFinished();
		emit LogMessageReceived("warning", "Stopped making new thumbnails.");
		emit PipelineStateChanged("👀 Waiting for you: pick a thumbnail below", false);
		emit ReviewReady(config.pendingReviewGate, config.pendingReviewState, config.pendingReviewData);
		return;
	}
	if (musicRegenActive && workerProcess) {
		/* Stop only the new songs (ACE-Step quits with the worker); the job keeps waiting with the songs it had */
		workerProcess->kill();
		workerProcess->waitForFinished(2000);
		workerProcess->deleteLater();
		workerProcess = nullptr;
		musicRegenActive = false;
		emit MusicRegenFinished();
		emit LogMessageReceived("warning", "Stopped making new songs.");
		emit PipelineStateChanged("👀 Waiting for you: pick the music below", false);
		emit ReviewReady(config.pendingReviewGate, config.pendingReviewState, config.pendingReviewData);
		return;
	}
	waitingForRemux = false;
	SetWorkflowArmed(false);
	if (HasPendingReview()) {
		DiscardPendingReview();
		emit PipelineStateChanged("⏹ Paused job discarded", false);
		emit LogMessageReceived("warning", "The paused YouTube job was discarded.");
	}
	if (workerProcess && workerProcess->state() != QProcess::NotRunning) {
		workerProcess->kill();
		workerProcess->waitForFinished(2000);
		workerProcess->deleteLater();
		workerProcess = nullptr;
		emit PipelineStateChanged("⏹ Pipeline Cancelled by User", false);
		emit LogMessageReceived("warning", "YouTube pipeline cancelled by user.");
	}
}

void YouTubeWorkflowManager::HandleStdoutLines()
{
	if (!workerProcess) {
		return;
	}
	stdoutBuffer.append(workerProcess->readAllStandardOutput());
	while (true) {
		int nl = stdoutBuffer.indexOf('\n');
		if (nl < 0) {
			break;
		}
		QByteArray line = stdoutBuffer.left(nl).trimmed();
		stdoutBuffer.remove(0, nl + 1);
		if (!line.isEmpty()) {
			ProcessJsonEvent(line);
		}
	}
}

void YouTubeWorkflowManager::ProcessJsonEvent(const QByteArray &line)
{
	QJsonParseError err{};
	QJsonDocument doc = QJsonDocument::fromJson(line, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		emit LogMessageReceived("info", QString::fromUtf8(line));
		return;
	}

	QJsonObject obj = doc.object();
	QString type = obj.value("type").toString();

	if (type == "log") {
		emit LogMessageReceived(obj.value("level").toString("info"), obj.value("message").toString());
	} else if (type == "step") {
		QString step = obj.value("step").toString();
		int pct = obj.value("progress").toInt();
		QString msg = obj.value("message").toString();
		emit StepProgressUpdated(step, pct, msg);
		emit PipelineStateChanged(QString("⚙️ %1").arg(msg), true);
	} else if (type == "metadata_ready") {
		QStringList altTitles;
		QJsonArray altArr = obj.value("alternative_titles").toArray();
		for (const auto &v : altArr) {
			altTitles.append(v.toString());
		}
		emit MetadataGenerated(obj.value("title").toString(), altTitles, obj.value("description").toString(),
				       obj.value("thumbnail_hook").toString(), obj.value("provider").toString());
	} else if (type == "thumbnail_ready") {
		emit ThumbnailGenerated(obj.value("thumbnail_path").toString(), obj.value("provider").toString());
	} else if (type == "complete") {
		QString url = obj.value("url").toString();
		if (!url.isEmpty()) {
			SetLastUpload(obj.value("video_id").toString(), url);
		}
		QString editedVideo = obj.value("edited_video").toString();
		QString thumbPath = obj.value("thumbnail_path").toString();
		QString msg = obj.value("message").toString();
		emit PipelineStateChanged(QString("✅ %1").arg(msg), false);
		emit PipelineCompleted(url, editedVideo, thumbPath, msg);
		emit LogMessageReceived("info", msg);
	} else if (type == "upload_pending") {
		QString editedVideo = obj.value("edited_video").toString();
		QString thumbPath = obj.value("thumbnail_path").toString();
		QString msg = obj.value("message").toString();
		emit PipelineStateChanged(QString("📁 %1").arg(msg), false);
		emit PipelineCompleted(QString(), editedVideo, thumbPath, msg);
	} else if (type == "error") {
		QString msg = obj.value("message").toString();
		emit PipelineStateChanged(QString("❌ Error: %1").arg(msg), false);
		emit LogMessageReceived("error", msg);
	} else if (type == "broll_plan_ready") {
		int count = obj.value("insert_count").toInt();
		emit PipelineStateChanged(
			QString("👀 Review %1 B-roll suggestion(s) below, then Render & Upload").arg(count), false);
		emit LogMessageReceived("info",
					QString("B-roll plan ready: %1").arg(obj.value("plan_path").toString()));
		SetPendingReview("broll", obj.value("state_path").toString(), obj.value("plan_path").toString());
		emit BrollPlanReady(obj.value("plan_path").toString(), obj.value("state_path").toString());
	} else if (type == "review_ready") {
		QString gate = obj.value("gate").toString();
		QString banner;
		if (gate == "cuts") {
			banner = QString("👀 Waiting for you: review %1 cuts (%2s removed) in the Cut Editor")
					 .arg(obj.value("count").toInt())
					 .arg(obj.value("removed_sec").toDouble(), 0, 'f', 1);
		} else if (gate == "metadata") {
			banner = "👀 Waiting for you: approve the title & description below";
		} else if (gate == "music") {
			banner = "👀 Waiting for you: pick the music below";
		} else if (gate == "thumbnail") {
			banner = "👀 Waiting for you: pick a thumbnail below";
		} else if (gate == "publish") {
			banner = "📤 Ready to publish: upload to YouTube, export for Premiere Pro, or finish";
		}
		SetPendingReview(gate, obj.value("state_path").toString(), obj.value("data_path").toString());
		emit PipelineStateChanged(banner, false);
		emit ReviewReady(gate, obj.value("state_path").toString(), obj.value("data_path").toString());
	} else if (type == "render_ready") {
		lastEditedVideo = obj.value("video").toString();
		lastJoins.clear();
		for (const QJsonValue &v : obj.value("joins").toArray()) {
			lastJoins.append(v.toDouble());
		}
		emit EditReady(lastEditedVideo);
	} else if (type == "premiere_exported") {
		emit PremiereExported(obj.value("xml_path").toString(), obj.value("open").toBool());
	} else if (type == "video_deleted") {
		QString id = obj.value("video_id").toString();
		if (id == lastVideoId) {
			SetLastUpload(QString(), QString());
		}
		emit VideoDeleteFinished(true, QString("Deleted https://youtu.be/%1 from YouTube.").arg(id));
	} else if (type == "delete_error") {
		emit VideoDeleteFinished(false, obj.value("message").toString("Deleting the video failed"));
	} else if (type == "upload_stats") {
		emit UploadStatsReady(obj.value("text").toString());
	} else if (type == "update_success") {
		emit VideoUpdateFinished(true, QString("Updated on YouTube: %1").arg(obj.value("title").toString()));
	} else if (type == "update_error") {
		emit VideoUpdateFinished(false, obj.value("message").toString("YouTube update failed"));
	} else if (type == "oauth_success") {
		QString channelTitle = obj.value("channel_title").toString("Connected Channel");
		QString tokenJson = obj.value("token_json").toString();
		SetVaultKey("YouTubeTokenJson", tokenJson);
		YouTubeSecretVault::WipeString(tokenJson);
		config.connectedChannelTitle = channelTitle;
		SaveConfig();
		emit OAuthStatusUpdated(true, channelTitle,
					QString("Connected to YouTube channel: %1").arg(channelTitle));
	} else if (type == "oauth_error") {
		emit OAuthStatusUpdated(false, QString(), obj.value("message").toString("OAuth failed"));
	}
}

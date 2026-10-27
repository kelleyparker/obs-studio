#include "YouTubeWorkflowManager.hpp"
#include "YouTubeSecretVault.hpp"

#include <OBSApp.hpp>
#include <util/config-file.h>

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

	const char *thumbStyle = config_get_string(cfg, "YouTubeWorkflow", "ThumbnailStyle");
	if (thumbStyle && *thumbStyle) {
		config.thumbnailStyle = QString::fromUtf8(thumbStyle);
	}

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
}

void YouTubeWorkflowManager::SaveConfig()
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	if (!cfg) {
		return;
	}

	QByteArray prioJoined = config.priorityList.join(',').toUtf8();
	config_set_string(cfg, "YouTubeWorkflow", "PriorityList", prioJoined.constData());
	config_set_string(cfg, "YouTubeWorkflow", "ThumbnailStyle", config.thumbnailStyle.toUtf8().constData());
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
	if (workflowArmed) {
		emit PipelineStateChanged("🔴 Recording Active — Click 'Stop Recording' (or ⏹ YT) to Edit & Upload",
					  false);
		emit LogMessageReceived("info", "YouTube Workflow armed: Recording in progress...");
	}
}

void YouTubeWorkflowManager::OnRecordingStopped(const QString &rawRecordingPath, bool willAutoRemux)
{
	if (!workflowArmed) {
		return;
	}

	SetWorkflowArmed(false);
	lastVideoPath = rawRecordingPath;

	if (willAutoRemux) {
		waitingForRemux = true;
		emit PipelineStateChanged("📹 Stage 1/5: Auto-Remuxing MKV to MP4...", true);
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

	lastVideoPath = remuxedMp4Path;
	emit StepProgressUpdated("remux", 100, QString("Remuxed to %1").arg(QFileInfo(remuxedMp4Path).fileName()));
	StartPipelineForVideo(remuxedMp4Path);
}

QString YouTubeWorkflowManager::ResolvePipelineScriptPath() const
{
	QString appDir = QCoreApplication::applicationDirPath();
	QStringList candidates = {
		QDir::cleanPath(appDir + "/../../data/obs-studio/scripts/youtube_pipeline.py"),
		QDir::cleanPath(appDir + "/scripts/youtube_pipeline.py"),
		QStringLiteral("C:/Users/cliffgoat/git/obs-studio/frontend/scripts/youtube_pipeline.py")
	};
	for (const QString &c : candidates) {
		if (QFileInfo::exists(c)) {
			return c;
		}
	}
	return candidates.last();
}

void YouTubeWorkflowManager::StartPipelineForVideo(const QString &videoPath)
{
	if (IsPipelineRunning()) {
		emit LogMessageReceived("warning", "A YouTube pipeline job is already running.");
		return;
	}

	LoadConfig();
	lastVideoPath = videoPath;

	QString scriptPath = ResolvePipelineScriptPath();
	if (!QFileInfo::exists(scriptPath)) {
		emit PipelineStateChanged("❌ Pipeline script missing", false);
		emit LogMessageReceived("error", QString("Could not find script at: %1").arg(scriptPath));
		return;
	}

	emit PipelineStateChanged("⚙️ Running YouTube AI Edit, Thumbnail & Upload Pipeline...", true);
	emit StepProgressUpdated("remux", 100, "MP4 Ready");
	emit StepProgressUpdated("transcribe", 0, "Queued...");
	emit StepProgressUpdated("edit", 0, "Queued...");
	emit StepProgressUpdated("llm", 0, "Queued...");
	emit StepProgressUpdated("thumbnail", 0, "Queued...");
	emit StepProgressUpdated("upload", 0, "Queued...");

	workerProcess = new QProcess(this);
	stdoutBuffer.clear();

	connect(workerProcess, &QProcess::readyReadStandardOutput, this, &YouTubeWorkflowManager::HandleStdoutLines);
	connect(workerProcess, &QProcess::readyReadStandardError, this, [this]() {
		if (!workerProcess)
			return;
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
		});

	SetPythonUtf8Environment(workerProcess);
	workerProcess->start(QStringLiteral("python"), {scriptPath, QStringLiteral("--stdin-config")});
	if (!workerProcess->waitForStarted(5000)) {
		emit PipelineStateChanged("❌ Failed to start Python worker process", false);
		emit LogMessageReceived("error", "Failed to start python.exe for YouTube pipeline.");
		workerProcess->deleteLater();
		workerProcess = nullptr;
		return;
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

	QJsonArray prioArr;
	for (const QString &p : config.priorityList) {
		prioArr.append(p);
	}

	QJsonObject payload;
	payload["mode"] = "pipeline";
	payload["input_video"] = videoPath;
	payload["priority_list"] = prioArr;
	payload["thumbnail_style"] = config.thumbnailStyle;
	payload["whisper_model"] = config.whisperModel;
	payload["ollama_model"] = config.ollamaModel;
	payload["privacy_status"] = config.privacyStatus;
	payload["category_id"] = config.categoryId;
	payload["made_for_kids"] = config.madeForKids;
	payload["silence_db"] = config.silenceDb;
	payload["min_silence_sec"] = config.minSilenceSec;
	payload["padding_sec"] = config.paddingSec;
	payload["cut_retakes"] = config.cutRetakes;
	payload["secrets"] = secretsObj;

	QByteArray jsonLine = QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n";
	workerProcess->write(jsonLine);
	workerProcess->closeWriteChannel();

#ifdef _WIN32
	SecureZeroMemory(jsonLine.data(), jsonLine.size());
#endif
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

void YouTubeWorkflowManager::CancelPipeline()
{
	waitingForRemux = false;
	SetWorkflowArmed(false);
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
		emit MetadataGenerated(
			obj.value("title").toString(),
			altTitles,
			obj.value("description").toString(),
			obj.value("thumbnail_hook").toString(),
			obj.value("provider").toString()
		);
	} else if (type == "thumbnail_ready") {
		emit ThumbnailGenerated(obj.value("thumbnail_path").toString(), obj.value("provider").toString());
	} else if (type == "complete") {
		QString url = obj.value("url").toString();
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
	} else if (type == "oauth_success") {
		QString channelTitle = obj.value("channel_title").toString("Connected Channel");
		QString tokenJson = obj.value("token_json").toString();
		SetVaultKey("YouTubeTokenJson", tokenJson);
		YouTubeSecretVault::WipeString(tokenJson);
		config.connectedChannelTitle = channelTitle;
		SaveConfig();
		emit OAuthStatusUpdated(true, channelTitle, QString("Connected to YouTube channel: %1").arg(channelTitle));
	} else if (type == "oauth_error") {
		emit OAuthStatusUpdated(false, QString(), obj.value("message").toString("OAuth failed"));
	}
}

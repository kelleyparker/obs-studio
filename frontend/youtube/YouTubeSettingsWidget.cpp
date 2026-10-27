#include "YouTubeSettingsWidget.hpp"
#include "YouTubeSecretVault.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
QString ProviderDisplayName(const QString &key)
{
	if (key == "gemini")
		return "Google Gemini (Gemini 2.5 Flash + Imagen 3 Thumbnail)";
	if (key == "openai")
		return "ChatGPT / OpenAI (GPT-4o + DALL-E 3 Thumbnail)";
	if (key == "claude")
		return "Anthropic Claude (Claude Sonnet Copywriting + Hybrid Thumbnail)";
	if (key == "local")
		return "Local GPU LLM (Ollama on RTX GPU + Smart Frame Compositor — 100% Free)";
	return key;
}
} // namespace

YouTubeSettingsWidget::YouTubeSettingsWidget(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	QWidget *container = new QWidget(scroll);
	QVBoxLayout *mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(12);

	// Header Banner
	QLabel *header = new QLabel(
		"<b>🎬 YouTube Automated Edit, AI Thumbnail & Upload Workflow</b><br/>"
		"<span style='color:#c8b8d8;'>Configure your 1-to-4 AI provider priority chain, OBS-exclusive DPAPI encrypted API keys, "
		"talking-head dead-air/retake trimming presets, and YouTube OAuth upload defaults.</span>",
		container);
	header->setWordWrap(true);
	header->setStyleSheet(
		"QLabel { background-color: #2b1d38; border: 1px solid #6b4c85; border-radius: 6px; padding: 10px; }");
	mainLayout->addWidget(header);

	// =========================================================================
	// SECTION 1: 1-to-4 AI Provider Priority Ranking & Models
	// =========================================================================
	QGroupBox *prioGroup =
		new QGroupBox("1. AI Provider Priority Ranking (1 to 4) & Local GPU Models", container);
	QVBoxLayout *prioLayout = new QVBoxLayout(prioGroup);

	QFormLayout *prioTopForm = new QFormLayout();
	preferredDefaultCombo = new QComboBox(prioGroup);
	preferredDefaultCombo->addItem("Google Gemini (Gemini 2.5 + Imagen 3)", "gemini");
	preferredDefaultCombo->addItem("ChatGPT / OpenAI (GPT-4o + DALL-E 3)", "openai");
	preferredDefaultCombo->addItem("Anthropic Claude (Sonnet + Smart Frame)", "claude");
	preferredDefaultCombo->addItem("Local GPU LLM (Ollama + Smart Frame — 100% Free)", "local");
	prioTopForm->addRow("Preferred Default (Promotes to Priority #1):", preferredDefaultCombo);
	prioLayout->addLayout(prioTopForm);

	QLabel *prioHint = new QLabel(
		"<i>Fallback Chain Order (1 = Highest Priority → 4 = Guaranteed Local GPU Fallback if a cloud key is blank or out of quota):</i>",
		prioGroup);
	prioHint->setWordWrap(true);
	prioLayout->addWidget(prioHint);

	QHBoxLayout *listRow = new QHBoxLayout();
	priorityListWidget = new QListWidget(prioGroup);
	priorityListWidget->setFixedHeight(102);
	listRow->addWidget(priorityListWidget, 1);

	QVBoxLayout *moveBtnCol = new QVBoxLayout();
	moveUpBtn = new QPushButton("▲ Move Up", prioGroup);
	moveDownBtn = new QPushButton("▼ Move Down", prioGroup);
	moveBtnCol->addWidget(moveUpBtn);
	moveBtnCol->addWidget(moveDownBtn);
	moveBtnCol->addStretch();
	listRow->addLayout(moveBtnCol);
	prioLayout->addLayout(listRow);

	QFormLayout *modelForm = new QFormLayout();
	thumbnailStyleCombo = new QComboBox(prioGroup);
	thumbnailStyleCombo->addItem("AI Graphic + Bold Hook Overlay (Cloud AI -> Local Frame Fallback)", "ai_graphic");
	thumbnailStyleCombo->addItem("Talking-Head Best Video Frame + AI Bold Hook Text (Local OpenCV/Pillow)",
				     "frame_overlay");
	modelForm->addRow("Custom Thumbnail Style:", thumbnailStyleCombo);

	whisperModelCombo = new QComboBox(prioGroup);
	whisperModelCombo->addItem("large-v3-turbo (Recommended — 8x Faster RTX CUDA FP16)", "large-v3-turbo");
	whisperModelCombo->addItem("large-v3 (Maximum Accuracy CUDA FP16)", "large-v3");
	whisperModelCombo->addItem("medium (Balanced)", "medium");
	whisperModelCombo->addItem("small (Lightweight)", "small");
	whisperModelCombo->addItem("base (Ultra-Fast)", "base");
	modelForm->addRow("Whisper Transcription Model:", whisperModelCombo);

	ollamaModelCombo = new QComboBox(prioGroup);
	ollamaModelCombo->setEditable(true);
	ollamaModelCombo->addItem("gemma4:12b");
	ollamaModelCombo->addItem("llama3.1:8b");
	ollamaModelCombo->addItem("qwen3:14b");
	ollamaModelCombo->addItem("qwen2.5-coder:14b");
	ollamaModelCombo->addItem("qwen3:32b");
	modelForm->addRow("Local GPU Ollama LLM (RTX 5070 / RTX 3050):", ollamaModelCombo);

	prioLayout->addLayout(modelForm);
	mainLayout->addWidget(prioGroup);

	// =========================================================================
	// SECTION 2: OBS-Exclusive Encrypted API Key Vault (Windows DPAPI)
	// =========================================================================
	QGroupBox *vaultGroup =
		new QGroupBox("2. OBS-Exclusive Encrypted API Key Vault (🔒 Windows DPAPI Protected)", container);
	QVBoxLayout *vaultLayout = new QVBoxLayout(vaultGroup);

	vaultStatusLabel = new QLabel(
		"🔒 Keys entered below are encrypted with Windows DPAPI + an OBS binary salt and sent strictly via private in-memory stdin pipe. "
		"No other app on your PC can read them.",
		vaultGroup);
	vaultStatusLabel->setWordWrap(true);
	vaultStatusLabel->setStyleSheet("color: #98e6b0; font-size: 11px;");
	vaultLayout->addWidget(vaultStatusLabel);

	QFormLayout *vaultForm = new QFormLayout();

	auto makeKeyRow = [this, vaultGroup](QLineEdit *&editOut, const QString &placeholder) -> QWidget * {
		QWidget *rowW = new QWidget(vaultGroup);
		QHBoxLayout *hl = new QHBoxLayout(rowW);
		hl->setContentsMargins(0, 0, 0, 0);
		editOut = new QLineEdit(rowW);
		editOut->setEchoMode(QLineEdit::Password);
		editOut->setPlaceholderText(placeholder);
		QPushButton *showBtn = new QPushButton("Show", rowW);
		showBtn->setCheckable(true);
		showBtn->setMinimumWidth(75);
		QPushButton *clearBtn = new QPushButton("Clear", rowW);
		clearBtn->setMinimumWidth(75);

		connect(showBtn, &QPushButton::toggled, this, [editOut, showBtn](bool checked) {
			editOut->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
			showBtn->setText(checked ? "Hide" : "Show");
		});
		connect(clearBtn, &QPushButton::clicked, this, [this, editOut]() {
			editOut->clear();
			emit SettingsModified();
		});
		connect(editOut, &QLineEdit::textEdited, this, [this]() { emit SettingsModified(); });

		hl->addWidget(editOut, 1);
		hl->addWidget(showBtn);
		hl->addWidget(clearBtn);
		return rowW;
	};

	vaultForm->addRow("Google Gemini API Key:",
			  makeKeyRow(geminiKeyEdit, "AIza... (Locked to OBS DPAPI Vault)"));
	vaultForm->addRow("ChatGPT / OpenAI API Key:",
			  makeKeyRow(openaiKeyEdit, "sk-... (Locked to OBS DPAPI Vault)"));
	vaultForm->addRow("Anthropic Claude API Key:",
			  makeKeyRow(claudeKeyEdit, "sk-ant-... (Locked to OBS DPAPI Vault)"));
	vaultLayout->addLayout(vaultForm);

	QPushButton *importEnvBtn = new QPushButton(
		"🛡️ Import Global Windows OPENAI_API_KEY into OBS Vault & Remove from Windows Env", vaultGroup);
	connect(importEnvBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnImportGlobalOpenAIKey);
	vaultLayout->addWidget(importEnvBtn);

	mainLayout->addWidget(vaultGroup);

	// =========================================================================
	// SECTION 3: YouTube Channel Account & Automated Upload Defaults
	// =========================================================================
	QGroupBox *ytGroup = new QGroupBox("3. YouTube Account Login & Automated Upload Defaults", container);
	QVBoxLayout *ytLayout = new QVBoxLayout(ytGroup);

	oauthStatusLabel = new QLabel("⚪ YouTube Account Status: Not Connected", ytGroup);
	oauthStatusLabel->setStyleSheet("font-weight: bold; color: #ffd27f;");
	ytLayout->addWidget(oauthStatusLabel);

	QFormLayout *ytForm = new QFormLayout();
	ytForm->addRow("Google OAuth Client ID:",
		       makeKeyRow(oauthClientIdEdit, "Optional if using client_secret.json below"));
	ytForm->addRow("Google OAuth Client Secret:",
		       makeKeyRow(oauthClientSecretEdit, "Optional if using client_secret.json below"));

	QWidget *secretFileRow = new QWidget(ytGroup);
	QHBoxLayout *sfLayout = new QHBoxLayout(secretFileRow);
	sfLayout->setContentsMargins(0, 0, 0, 0);
	clientSecretFileEdit = new QLineEdit(secretFileRow);
	clientSecretFileEdit->setPlaceholderText("Path to Google Cloud client_secret.json (Desktop OAuth client)...");
	QPushButton *browseSecretBtn = new QPushButton("Browse...", secretFileRow);
	connect(browseSecretBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnBrowseClientSecretJson);
	connect(clientSecretFileEdit, &QLineEdit::textEdited, this, [this]() { emit SettingsModified(); });
	sfLayout->addWidget(clientSecretFileEdit, 1);
	sfLayout->addWidget(browseSecretBtn);
	ytForm->addRow("Or client_secret.json File:", secretFileRow);

	QHBoxLayout *oauthBtnRow = new QHBoxLayout();
	QPushButton *connectYtBtn = new QPushButton("🔗 Sign in with Google / Connect YouTube Channel", ytGroup);
	connectYtBtn->setStyleSheet("font-weight: bold;");
	QPushButton *disconnectYtBtn = new QPushButton("Disconnect Account", ytGroup);
	connect(connectYtBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnConnectYouTubeAccount);
	connect(disconnectYtBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnDisconnectYouTubeAccount);
	oauthBtnRow->addWidget(connectYtBtn, 1);
	oauthBtnRow->addWidget(disconnectYtBtn);
	ytLayout->addLayout(ytForm);
	ytLayout->addLayout(oauthBtnRow);

	QFormLayout *uploadDefaultsForm = new QFormLayout();
	privacyCombo = new QComboBox(ytGroup);
	privacyCombo->addItem("Unlisted (Recommended for Automated Workflow Testing)", "unlisted");
	privacyCombo->addItem("Private (Only You Can View)", "private");
	privacyCombo->addItem("Public (Publish Live Immediately)", "public");
	uploadDefaultsForm->addRow("Default Upload Privacy:", privacyCombo);

	categoryCombo = new QComboBox(ytGroup);
	categoryCombo->addItem("Science & Technology (28)", "28");
	categoryCombo->addItem("People & Blogs (22)", "22");
	categoryCombo->addItem("Gaming (20)", "20");
	categoryCombo->addItem("Education (27)", "27");
	categoryCombo->addItem("Entertainment (24)", "24");
	categoryCombo->addItem("Howto & Style (26)", "26");
	uploadDefaultsForm->addRow("Default Video Category:", categoryCombo);

	madeForKidsCheck = new QCheckBox("Video is Made for Kids (COPPA)", ytGroup);
	uploadDefaultsForm->addRow("Audience:", madeForKidsCheck);
	ytLayout->addLayout(uploadDefaultsForm);

	mainLayout->addWidget(ytGroup);

	// =========================================================================
	// SECTION 4: Talking-Head Video Editor Presets (Dead Air & Retakes)
	// =========================================================================
	QGroupBox *editGroup =
		new QGroupBox("4. Talking-Head Smart Editor Presets (Dead Air & Butchered Sentences)", container);
	QFormLayout *editForm = new QFormLayout(editGroup);

	silenceDbSpin = new QDoubleSpinBox(editGroup);
	silenceDbSpin->setRange(-60.0, -10.0);
	silenceDbSpin->setSingleStep(1.0);
	silenceDbSpin->setSuffix(" dB");
	silenceDbSpin->setValue(-32.0);
	editForm->addRow("Dead-Air Silence Threshold:", silenceDbSpin);

	minSilenceSecSpin = new QDoubleSpinBox(editGroup);
	minSilenceSecSpin->setRange(0.20, 5.00);
	minSilenceSecSpin->setSingleStep(0.10);
	minSilenceSecSpin->setSuffix(" sec");
	minSilenceSecSpin->setValue(0.50);
	editForm->addRow("Minimum Dead-Air Duration to Cut:", minSilenceSecSpin);

	paddingSecSpin = new QDoubleSpinBox(editGroup);
	paddingSecSpin->setRange(0.05, 0.50);
	paddingSecSpin->setSingleStep(0.05);
	paddingSecSpin->setSuffix(" sec");
	paddingSecSpin->setValue(0.15);
	editForm->addRow("Natural Speech Breath Cushion / Padding:", paddingSecSpin);

	cutRetakesCheck = new QCheckBox(
		"Automatically Cut Butchered Sentences & Repeated Takes (uses Whisper word timestamps)", editGroup);
	cutRetakesCheck->setChecked(true);
	editForm->addRow("Retake Trimming:", cutRetakesCheck);

	mainLayout->addWidget(editGroup);
	mainLayout->addStretch();

	scroll->setWidget(container);
	outerLayout->addWidget(scroll);

	// Connect signals
	connect(preferredDefaultCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&YouTubeSettingsWidget::OnPreferredDefaultChanged);
	connect(moveUpBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnMovePriorityUp);
	connect(moveDownBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnMovePriorityDown);

	auto notifyMod = [this]() {
		if (!internalUpdating) {
			emit SettingsModified();
		}
	};
	connect(thumbnailStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(whisperModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(ollamaModelCombo, &QComboBox::currentTextChanged, this, notifyMod);
	connect(privacyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(madeForKidsCheck, &QCheckBox::toggled, this, notifyMod);
	connect(silenceDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(minSilenceSecSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(paddingSecSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(cutRetakesCheck, &QCheckBox::toggled, this, notifyMod);

	connect(YouTubeWorkflowManager::Instance(), &YouTubeWorkflowManager::OAuthStatusUpdated, this,
		[this](bool connected, const QString &channelTitle, const QString &message) {
			if (connected) {
				oauthStatusLabel->setText(QString("✅ Connected to YouTube Channel: %1").arg(channelTitle));
				oauthStatusLabel->setStyleSheet("font-weight: bold; color: #7fff9f;");
			} else {
				oauthStatusLabel->setText(QString("⚠️ YouTube OAuth: %1").arg(message));
				oauthStatusLabel->setStyleSheet("font-weight: bold; color: #ff8080;");
			}
		});

	LoadFromManager();
}

QStringList YouTubeSettingsWidget::CurrentPriorityKeys() const
{
	QStringList keys;
	for (int i = 0; i < priorityListWidget->count(); ++i) {
		keys.append(priorityListWidget->item(i)->data(Qt::UserRole).toString());
	}
	return keys;
}

void YouTubeSettingsWidget::SetPriorityKeys(const QStringList &keys)
{
	priorityListWidget->clear();
	QStringList allDefaults = {"gemini", "openai", "claude", "local"};
	QStringList merged = keys;
	for (const QString &d : allDefaults) {
		if (!merged.contains(d)) {
			merged.append(d);
		}
	}

	for (const QString &k : merged) {
		QListWidgetItem *item = new QListWidgetItem(priorityListWidget);
		item->setData(Qt::UserRole, k);
		priorityListWidget->addItem(item);
	}
	RefreshPriorityLabels();
}

void YouTubeSettingsWidget::RefreshPriorityLabels()
{
	for (int i = 0; i < priorityListWidget->count(); ++i) {
		QListWidgetItem *item = priorityListWidget->item(i);
		QString key = item->data(Qt::UserRole).toString();
		QString badge = (i == 0) ? "Priority #1 (Preferred Default)" : QString("Priority #%1").arg(i + 1);
		item->setText(QString("%1  —  %2").arg(badge, ProviderDisplayName(key)));
	}

	if (priorityListWidget->count() > 0) {
		QString topKey = priorityListWidget->item(0)->data(Qt::UserRole).toString();
		int comboIdx = preferredDefaultCombo->findData(topKey);
		if (comboIdx >= 0 && preferredDefaultCombo->currentIndex() != comboIdx) {
			internalUpdating = true;
			preferredDefaultCombo->setCurrentIndex(comboIdx);
			internalUpdating = false;
		}
	}
}

void YouTubeSettingsWidget::OnPreferredDefaultChanged(int index)
{
	if (internalUpdating || index < 0) {
		return;
	}
	QString chosenKey = preferredDefaultCombo->itemData(index).toString();
	QStringList current = CurrentPriorityKeys();
	current.removeAll(chosenKey);
	current.prepend(chosenKey);
	SetPriorityKeys(current);
	priorityListWidget->setCurrentRow(0);
	emit SettingsModified();
}

void YouTubeSettingsWidget::OnMovePriorityUp()
{
	int row = priorityListWidget->currentRow();
	if (row <= 0) {
		return;
	}
	QStringList keys = CurrentPriorityKeys();
	keys.swapItemsAt(row, row - 1);
	SetPriorityKeys(keys);
	priorityListWidget->setCurrentRow(row - 1);
	emit SettingsModified();
}

void YouTubeSettingsWidget::OnMovePriorityDown()
{
	int row = priorityListWidget->currentRow();
	if (row < 0 || row >= priorityListWidget->count() - 1) {
		return;
	}
	QStringList keys = CurrentPriorityKeys();
	keys.swapItemsAt(row, row + 1);
	SetPriorityKeys(keys);
	priorityListWidget->setCurrentRow(row + 1);
	emit SettingsModified();
}

void YouTubeSettingsWidget::OnImportGlobalOpenAIKey()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (mgr->ImportAndRemoveWindowsEnvKey("OPENAI_API_KEY", "OpenAIKey")) {
		openaiKeyEdit->setText(mgr->GetVaultKeyDecrypted("OpenAIKey"));
		vaultStatusLabel->setText(
			"✅ Imported OPENAI_API_KEY into OBS DPAPI Vault and removed it from global Windows Environment Variables!");
		QMessageBox::information(
			this, "Key Locked to OBS Vault",
			"Your OPENAI_API_KEY has been encrypted into OBS's DPAPI Vault and removed from global Windows Environment Variables.\n\n"
			"Now ONLY OBS Studio can access this key.");
	} else {
		QMessageBox::information(
			this, "No Global Key Found",
			"No global OPENAI_API_KEY was found in your Windows User Environment (or it was already imported into OBS).");
	}
}

void YouTubeSettingsWidget::OnBrowseClientSecretJson()
{
	QString file = QFileDialog::getOpenFileName(this, "Select Google Cloud OAuth client_secret.json", QString(),
						    "JSON Files (*.json);;All Files (*.*)");
	if (!file.isEmpty()) {
		clientSecretFileEdit->setText(file);
		emit SettingsModified();
	}
}

void YouTubeSettingsWidget::OnConnectYouTubeAccount()
{
	SaveToManager();
	oauthStatusLabel->setText("⏳ Opening browser for Google OAuth 2.0 login...");
	YouTubeWorkflowManager::Instance()->StartOAuthLogin();
}

void YouTubeSettingsWidget::OnDisconnectYouTubeAccount()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	mgr->ClearVaultKey("YouTubeTokenJson");
	mgr->GetConfig().connectedChannelTitle.clear();
	mgr->SaveConfig();
	oauthStatusLabel->setText("⚪ YouTube Account Status: Disconnected");
	oauthStatusLabel->setStyleSheet("font-weight: bold; color: #ffd27f;");
}

void YouTubeSettingsWidget::LoadFromManager()
{
	internalUpdating = true;
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	mgr->LoadConfig();
	const YouTubeWorkflowConfig &cfg = mgr->GetConfig();

	SetPriorityKeys(cfg.priorityList);

	int styleIdx = thumbnailStyleCombo->findData(cfg.thumbnailStyle);
	if (styleIdx >= 0)
		thumbnailStyleCombo->setCurrentIndex(styleIdx);

	int wIdx = whisperModelCombo->findData(cfg.whisperModel);
	if (wIdx >= 0)
		whisperModelCombo->setCurrentIndex(wIdx);

	ollamaModelCombo->setCurrentText(cfg.ollamaModel);

	geminiKeyEdit->setText(mgr->GetVaultKeyDecrypted("GeminiKey"));
	openaiKeyEdit->setText(mgr->GetVaultKeyDecrypted("OpenAIKey"));
	claudeKeyEdit->setText(mgr->GetVaultKeyDecrypted("ClaudeKey"));
	oauthClientIdEdit->setText(mgr->GetVaultKeyDecrypted("YouTubeClientId"));
	oauthClientSecretEdit->setText(mgr->GetVaultKeyDecrypted("YouTubeClientSecret"));
	clientSecretFileEdit->setText(cfg.clientSecretFilePath);

	if (mgr->HasVaultKey("YouTubeTokenJson")) {
		QString ch = cfg.connectedChannelTitle.isEmpty() ? "Authorized Channel" : cfg.connectedChannelTitle;
		oauthStatusLabel->setText(QString("✅ Connected to YouTube Channel: %1").arg(ch));
		oauthStatusLabel->setStyleSheet("font-weight: bold; color: #7fff9f;");
	} else {
		oauthStatusLabel->setText("⚪ YouTube Account Status: Not Connected");
		oauthStatusLabel->setStyleSheet("font-weight: bold; color: #ffd27f;");
	}

	int privIdx = privacyCombo->findData(cfg.privacyStatus);
	if (privIdx >= 0)
		privacyCombo->setCurrentIndex(privIdx);

	int catIdx = categoryCombo->findData(cfg.categoryId);
	if (catIdx >= 0)
		categoryCombo->setCurrentIndex(catIdx);

	madeForKidsCheck->setChecked(cfg.madeForKids);

	silenceDbSpin->setValue(cfg.silenceDb);
	minSilenceSecSpin->setValue(cfg.minSilenceSec);
	paddingSecSpin->setValue(cfg.paddingSec);
	cutRetakesCheck->setChecked(cfg.cutRetakes);

	internalUpdating = false;
}

void YouTubeSettingsWidget::SaveToManager()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	YouTubeWorkflowConfig &cfg = mgr->GetConfig();

	cfg.priorityList = CurrentPriorityKeys();
	cfg.thumbnailStyle = thumbnailStyleCombo->currentData().toString();
	cfg.whisperModel = whisperModelCombo->currentData().toString();
	cfg.ollamaModel = ollamaModelCombo->currentText().trimmed();
	cfg.privacyStatus = privacyCombo->currentData().toString();
	cfg.categoryId = categoryCombo->currentData().toString();
	cfg.madeForKids = madeForKidsCheck->isChecked();
	cfg.clientSecretFilePath = clientSecretFileEdit->text().trimmed();
	cfg.silenceDb = silenceDbSpin->value();
	cfg.minSilenceSec = minSilenceSecSpin->value();
	cfg.paddingSec = paddingSecSpin->value();
	cfg.cutRetakes = cutRetakesCheck->isChecked();

	mgr->SetVaultKey("GeminiKey", geminiKeyEdit->text());
	mgr->SetVaultKey("OpenAIKey", openaiKeyEdit->text());
	mgr->SetVaultKey("ClaudeKey", claudeKeyEdit->text());
	mgr->SetVaultKey("YouTubeClientId", oauthClientIdEdit->text());
	mgr->SetVaultKey("YouTubeClientSecret", oauthClientSecretEdit->text());

	mgr->SaveConfig();
}

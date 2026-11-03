#include "YouTubeSettingsWidget.hpp"
#include "YouTubeScriptWriter.hpp"
#include "YouTubeSecretVault.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <obs.h>


#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTextDocumentFragment>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString ProviderDisplayName(const QString &key)
{
	if (key == "gemini")
		return "Google Gemini (Gemini 2.5 Flash + Nano Banana Thumbnail)";
	if (key == "openai")
		return "ChatGPT / OpenAI (GPT-4o + GPT Image Thumbnail)";
	if (key == "claude")
		return "Anthropic Claude (Claude Sonnet Copywriting + Hybrid Thumbnail)";
	if (key == "local")
		return "Local GPU LLM (Ollama on RTX GPU + Smart Frame Compositor — 100% Free)";
	return key;
}

/* What the settings filter matches a widget on: labels, button text, list
 * and dropdown choices, placeholders and tooltips. Never a QLineEdit's text,
 * so a typed API key can't be found by searching for it. */
QString SearchText(QWidget *root)
{
	QStringList parts;
	QList<QWidget *> widgets = root->findChildren<QWidget *>();
	widgets.prepend(root);
	for (QWidget *w : widgets) {
		parts << w->toolTip();
		if (auto *label = qobject_cast<QLabel *>(w)) {
			parts << QTextDocumentFragment::fromHtml(label->text()).toPlainText();
		} else if (auto *button = qobject_cast<QAbstractButton *>(w)) {
			parts << button->text();
		} else if (auto *combo = qobject_cast<QComboBox *>(w)) {
			for (int i = 0; i < combo->count(); ++i)
				parts << combo->itemText(i);
		} else if (auto *list = qobject_cast<QListWidget *>(w)) {
			for (int i = 0; i < list->count(); ++i)
				parts << list->item(i)->text();
		} else if (auto *edit = qobject_cast<QLineEdit *>(w)) {
			parts << edit->placeholderText();
		}
	}
	return parts.join('\n');
}

void CollectWidgets(QLayoutItem *item, QList<QWidget *> &out)
{
	if (QWidget *w = item->widget()) {
		out << w;
	} else if (QLayout *layout = item->layout()) {
		for (int i = 0; i < layout->count(); ++i)
			CollectWidgets(layout->itemAt(i), out);
	}
}

QString SearchText(QLayoutItem *item)
{
	QList<QWidget *> widgets;
	CollectWidgets(item, widgets);
	QStringList parts;
	for (QWidget *w : widgets)
		parts << SearchText(w);
	return parts.join('\n');
}
} // namespace

YouTubeSettingsWidget::YouTubeSettingsWidget(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	// Filter row, outside the scroll area so it stays put (as on the Hotkeys page)
	QHBoxLayout *filterRow = new QHBoxLayout();
	filterRow->setContentsMargins(12, 12, 12, 0);
	filterRow->addWidget(new QLabel("Filter:", this));
	filterEdit = new QLineEdit(this);
	filterEdit->setPlaceholderText("Search YouTube settings…");
	filterEdit->setClearButtonEnabled(true);
	filterEdit->installEventFilter(this);
	filterRow->addWidget(filterEdit, 1);
	outerLayout->addLayout(filterRow);

	QScrollArea *scroll = new QScrollArea(this);
	scrollArea = scroll;
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
	headerLabel = header;

	// =========================================================================
	// SECTION 1: 1-to-4 AI Provider Priority Ranking & Models
	// =========================================================================
	QGroupBox *prioGroup =
		new QGroupBox("1. AI Provider Priority Ranking (1 to 4) & Local GPU Models", container);
	QVBoxLayout *prioLayout = new QVBoxLayout(prioGroup);

	QFormLayout *prioTopForm = new QFormLayout();
	preferredDefaultCombo = new QComboBox(prioGroup);
	preferredDefaultCombo->addItem("Google Gemini (Gemini 2.5 + Nano Banana)", "gemini");
	preferredDefaultCombo->addItem("ChatGPT / OpenAI (GPT-4o + GPT Image)", "openai");
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
	thumbnailImageSourceCombo = new QComboBox(prioGroup);
	FillThumbnailImageSources(thumbnailImageSourceCombo);
	thumbnailImageSourceCombo->setToolTip(
		"Where the AI thumbnail images come from. Auto mode makes one; Step mode makes 4 next to 4 video frames.\n"
		"Local Stable Diffusion and local Ollama are free. Stable Diffusion runs on your NVIDIA GPU from\n"
		"%USERPROFILE%\\OBSYouTubeImageGen. If local image generation isn't installed or fails, OBS makes free\n"
		"thumbnails designed by your local LLM on your best video frames instead.\n"
		"Nano Banana and GPT Image are paid per image (4 per Step run, 2 per refine) and give the best images.\n"
		"Your photo and video frames are never sent to them: only your prompt, the text-free background,\n"
		"and the reference images you attach at the thumbnail step.");
	modelForm->addRow("AI thumbnail images:", thumbnailImageSourceCombo);

	cloudImageFallbackCheck = new QCheckBox("Try the other cloud image models if one is out of quota or its key fails",
						prioGroup);
	cloudImageFallbackCheck->setChecked(true);
	cloudImageFallbackCheck->setToolTip(
		"Nano Banana Pro, Nano Banana 2 and GPT Image are tried in turn (only those with an API key saved below)\n"
		"before falling back to free local thumbnails. Another Nano Banana model on the same key is always tried.\n"
		"Each image you get is billed by the model that made it.");
	modelForm->addRow("Cloud fallback:", cloudImageFallbackCheck);

	musicEnabledCheck = new QCheckBox("Make background music and mix it under my voice", prioGroup);
	musicEnabledCheck->setChecked(true);
	musicEnabledCheck->setToolTip(
		"Your local LLM writes a style for the video, the music source below makes the songs, and the one you\n"
		"pick plays about 20 dB under your voice, dipping further while you talk.\n"
		"Step mode offers 3 songs to choose from; Auto mode makes one.");
	modelForm->addRow("Background music:", musicEnabledCheck);

	musicSourceCombo = new QComboBox(prioGroup);
	FillMusicSources(musicSourceCombo);
	musicSourceCombo->setToolTip(
		"What makes the music in both Step and Auto mode. In Step mode, the music picker can make more songs\n"
		"with the other one without changing this.\n"
		"ACE-Step makes the songs on your GPU (nothing is uploaded); it needs ACE-Step in\n"
		"%USERPROFILE%\\OBSYouTubeMusicGen and takes about 3 minutes per minute of video.\n"
		"Lyria is Google's music model, on the Gemini API key below (billing must be on: there's no free tier).\n"
		"It makes about 2 minutes of music per song at about $0.08, in about half a minute: a 10-minute video\n"
		"is about $0.72 for Step mode's 3 songs, $0.24 in Auto mode. Only the style text is sent, never your voice.\n"
		"Any song Lyria can't make (out of quota, key refused) is made on this PC with ACE-Step instead.");
	modelForm->addRow("Music source:", musicSourceCombo);

	thumbnailImageModelCombo = new QComboBox(prioGroup);
	thumbnailImageModelCombo->setEditable(true);
	thumbnailImageModelCombo->addItem("x/z-image-turbo");
	thumbnailImageModelCombo->addItem("x/flux2-klein");
	thumbnailImageModelCombo->setToolTip("Ollama image model for local thumbnails. Install it with: ollama pull <name>");
	modelForm->addRow("Local image model (Ollama):", thumbnailImageModelCombo);

	autoFinishCombo = new QComboBox(prioGroup);
	autoFinishCombo->addItem("Upload to YouTube", "upload");
	autoFinishCombo->addItem("Export to Premiere Pro (don't upload)", "premiere");
	autoFinishCombo->addItem("Both: export to Premiere Pro and upload", "both");
	autoFinishCombo->setToolTip("What Auto mode does with the finished edit. Step mode always asks you at the end.");
	modelForm->addRow("When Auto mode finishes:", autoFinishCombo);

	/* Script writer: Claude writes the script before you record (📝 Script… in the YouTube dock) */
	QWidget *scriptRow = new QWidget(prioGroup);
	QHBoxLayout *scriptLayout = new QHBoxLayout(scriptRow);
	scriptLayout->setContentsMargins(0, 0, 0, 0);
	scriptModelCombo = new QComboBox(scriptRow);
	YouTubeScriptWriter::FillModels(scriptModelCombo);
	scriptEffortCombo = new QComboBox(scriptRow);
	YouTubeScriptWriter::FillEfforts(scriptEffortCombo);
	scriptCostLabel = new QLabel(scriptRow);
	scriptCostLabel->setStyleSheet("color: #b8a8c8;");
	scriptLayout->addWidget(scriptModelCombo, 1);
	scriptLayout->addWidget(scriptEffortCombo);
	scriptLayout->addWidget(scriptCostLabel);
	scriptRow->setToolTip(
		"Who writes your video scripts and teleprompter text, before you record (📝 Script… in the YouTube dock).\n"
		"Uses the Claude API key below and is billed per script. Only your topic, notes and past video titles\n"
		"are sent, never your recordings. Higher effort thinks longer: better on hard topics, slower, costs more.");
	auto updateScriptCost = [this]() {
		scriptCostLabel->setText(
			QString("~$%1 per 8-min script")
				.arg(YouTubeScriptWriter::EstimateCost(scriptModelCombo->currentData().toString(),
								      scriptEffortCombo->currentData().toString(), 8.0),
				     0, 'f', 2));
	};
	connect(scriptModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateScriptCost);
	connect(scriptEffortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateScriptCost);
	updateScriptCost();
	modelForm->addRow("Script writer:", scriptRow);

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

	editingPresetCombo = new QComboBox(editGroup);
	editingPresetCombo->addItem("Conservative (Default — Safe & Natural)", "conservative");
	editingPresetCombo->addItem("Aggressive (Fast Jumpcut Style)", "aggressive");
	editingPresetCombo->addItem("Dead-Air Only (No Sentence Cuts)", "dead_air_only");
	editingPresetCombo->addItem("Custom (Manual Thresholds)", "custom");
	editForm->addRow("Editing Pacing Preset:", editingPresetCombo);

	silenceDbSpin = new QDoubleSpinBox(editGroup);
	silenceDbSpin->setRange(-60.0, -10.0);
	silenceDbSpin->setSingleStep(1.0);
	silenceDbSpin->setSuffix(" dB");
	silenceDbSpin->setValue(-42.0);
	editForm->addRow("Dead-Air Silence Threshold:", silenceDbSpin);

	minSilenceSecSpin = new QDoubleSpinBox(editGroup);
	minSilenceSecSpin->setRange(0.20, 5.00);
	minSilenceSecSpin->setSingleStep(0.10);
	minSilenceSecSpin->setSuffix(" sec");
	minSilenceSecSpin->setValue(1.20);
	editForm->addRow("Minimum Dead-Air Duration to Cut:", minSilenceSecSpin);

	paddingSecSpin = new QDoubleSpinBox(editGroup);
	paddingSecSpin->setRange(0.05, 0.50);
	paddingSecSpin->setSingleStep(0.05);
	paddingSecSpin->setSuffix(" sec");
	paddingSecSpin->setValue(0.25);
	editForm->addRow("Natural Speech Breath Cushion / Padding:", paddingSecSpin);

	cutRetakesCheck = new QCheckBox(
		"Automatically Cut Butchered Sentences & Repeated Takes (uses Whisper word timestamps)", editGroup);
	cutRetakesCheck->setChecked(true);
	editForm->addRow("Retake Trimming:", cutRetakesCheck);

	normalizeLoudnessCheck = new QCheckBox("Bring my voice up to YouTube's loudness (-14 LUFS)", editGroup);
	normalizeLoudnessCheck->setChecked(true);
	normalizeLoudnessCheck->setToolTip(
		"YouTube turns loud videos down but never turns quiet ones up. The edit measures your voice and\n"
		"turns it up (or down) to -14 LUFS, with a limiter so the peaks can't clip. Background music is\n"
		"mixed relative to the result. The Premiere project keeps the recording as it was.");
	editForm->addRow("Voice Loudness:", normalizeLoudnessCheck);

	mainLayout->addWidget(editGroup);

	// =========================================================================
	// SECTION 5: B-roll (AI pictures of what you say, and/or your screen)
	// =========================================================================
	QGroupBox *brollGroup = new QGroupBox("5. B-roll (AI Pictures of What You Say, or Your Screen)", container);
	QFormLayout *brollForm = new QFormLayout(brollGroup);

	brollEnabledCheck = new QCheckBox("Add B-roll to my videos", brollGroup);
	brollForm->addRow("B-roll:", brollEnabledCheck);

	QWidget *amountRow = new QWidget(brollGroup);
	QHBoxLayout *amountLayout = new QHBoxLayout(amountRow);
	amountLayout->setContentsMargins(0, 0, 0, 0);
	QLabel *fewerLabel = new QLabel("Fewer", amountRow);
	brollAmountSlider = new QSlider(Qt::Horizontal, amountRow);
	brollAmountSlider->setRange(0, kBrollAmountMax);
	brollAmountSlider->setPageStep(1);
	brollAmountSlider->setTickPosition(QSlider::TicksBelow);
	brollAmountSlider->setValue(2);
	brollAmountSlider->setToolTip(
		"How many AI pictures to add, from Off to Lots (about one every 2 minutes up to one every 25 seconds). "
		"The AI picks the moments where a picture helps explain what you say. In Step mode you can change "
		"this for each video before any picture is made.");
	QLabel *moreLabel = new QLabel("More", amountRow);
	brollAmountLabel = new QLabel(amountRow);
	brollAmountLabel->setMinimumWidth(70);
	amountLayout->addWidget(fewerLabel);
	amountLayout->addWidget(brollAmountSlider, 1);
	amountLayout->addWidget(moreLabel);
	amountLayout->addWidget(brollAmountLabel);
	connect(brollAmountSlider, &QSlider::valueChanged, this,
		[this](int value) { brollAmountLabel->setText(BrollAmountName(value)); });
	brollAmountLabel->setText(BrollAmountName(brollAmountSlider->value()));
	brollForm->addRow("AI pictures:", amountRow);

	brollImageSourceCombo = new QComboBox(brollGroup);
	FillBrollImageSources(brollImageSourceCombo);
	brollImageSourceCombo->setToolTip(
		"Who makes the pictures. Auto mode always uses this; in Step mode you can pick for each picture. "
		"Only the picture's description is sent to a cloud model, never your video. A picture the cloud "
		"can't make is made on this PC instead.");
	brollForm->addRow("Made with:", brollImageSourceCombo);

	brollSourceCombo = new QComboBox(brollGroup);
	brollSourceCombo->setToolTip(
		"Optional. Any video source, e.g. a Display Capture in a spare scene, recorded to its own file "
		"next to the webcam so the edit can cut to your screen. The main recording is not changed.");
	brollForm->addRow("Screen source (optional):", brollSourceCombo);

	brollStyleCombo = new QComboBox(brollGroup);
	brollStyleCombo->addItem("B-roll with webcam bubble (PiP)", "pip");
	brollStyleCombo->addItem("Full-screen cutaway", "cutaway");
	brollForm->addRow("Style:", brollStyleCombo);

	brollCoverageSpin = new QDoubleSpinBox(brollGroup);
	brollCoverageSpin->setRange(5.0, 80.0);
	brollCoverageSpin->setSingleStep(5.0);
	brollCoverageSpin->setDecimals(0);
	brollCoverageSpin->setSuffix(" % of the video");
	brollCoverageSpin->setValue(40.0);
	brollForm->addRow("Maximum B-roll:", brollCoverageSpin);

	QLabel *brollHint = new QLabel(
		"Tip: bind \"YouTube: Mark B-roll moment\" in Settings → Hotkeys and press it while recording to flag "
		"spots that need a picture or your screen. In Step mode (YouTube dock) you pick the spots, choose who "
		"makes each picture and see them before rendering.",
		brollGroup);
	brollHint->setWordWrap(true);
	brollHint->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	brollForm->addRow(brollHint);

	mainLayout->addWidget(brollGroup);

	sections = {prioGroup, vaultGroup, ytGroup, editGroup, brollGroup};
	/* Never taller than their contents, so when the filter hides most of a section the spare height
	 * goes to the stretch below rather than to the stretch beside the Move Up/Down buttons */
	for (QGroupBox *section : sections) {
		section->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	}
	noMatchLabel = new QLabel(container);
	noMatchLabel->setWordWrap(true);
	noMatchLabel->setStyleSheet("color: #b8a8c8;");
	noMatchLabel->hide();
	mainLayout->addWidget(noMatchLabel);
	mainLayout->addStretch();

	scroll->setWidget(container);
	outerLayout->addWidget(scroll);

	// Connect signals
	connect(editingPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
		QString preset = editingPresetCombo->itemData(idx).toString();
		if (preset == "conservative") {
			silenceDbSpin->setValue(-42.0);
			minSilenceSecSpin->setValue(1.20);
			paddingSecSpin->setValue(0.25);
			cutRetakesCheck->setChecked(true);
		} else if (preset == "aggressive") {
			silenceDbSpin->setValue(-36.0);
			minSilenceSecSpin->setValue(0.70);
			paddingSecSpin->setValue(0.15);
			cutRetakesCheck->setChecked(true);
		} else if (preset == "dead_air_only") {
			silenceDbSpin->setValue(-42.0);
			minSilenceSecSpin->setValue(1.20);
			paddingSecSpin->setValue(0.25);
			cutRetakesCheck->setChecked(false);
		}
		emit SettingsModified();
	});

	connect(filterEdit, &QLineEdit::textChanged, this, &YouTubeSettingsWidget::ApplyFilter);

	connect(preferredDefaultCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&YouTubeSettingsWidget::OnPreferredDefaultChanged);
	connect(moveUpBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnMovePriorityUp);
	connect(moveDownBtn, &QPushButton::clicked, this, &YouTubeSettingsWidget::OnMovePriorityDown);

	auto notifyMod = [this]() {
		if (!internalUpdating) {
			emit SettingsModified();
		}
	};
	connect(thumbnailImageSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(thumbnailImageModelCombo, &QComboBox::currentTextChanged, this, notifyMod);
	connect(cloudImageFallbackCheck, &QCheckBox::toggled, this, notifyMod);
	connect(musicEnabledCheck, &QCheckBox::toggled, this, notifyMod);
	connect(musicSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(autoFinishCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(scriptModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(scriptEffortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(whisperModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(ollamaModelCombo, &QComboBox::currentTextChanged, this, notifyMod);
	connect(privacyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(madeForKidsCheck, &QCheckBox::toggled, this, notifyMod);
	connect(silenceDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(minSilenceSecSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(paddingSecSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(cutRetakesCheck, &QCheckBox::toggled, this, notifyMod);
	connect(normalizeLoudnessCheck, &QCheckBox::toggled, this, notifyMod);
	connect(brollEnabledCheck, &QCheckBox::toggled, this, notifyMod);
	connect(brollSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(brollStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);
	connect(brollCoverageSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, notifyMod);
	connect(brollAmountSlider, &QSlider::valueChanged, this, notifyMod);
	connect(brollImageSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, notifyMod);

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

bool YouTubeSettingsWidget::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == filterEdit && event->type() == QEvent::KeyPress &&
	    static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && !filterEdit->text().isEmpty()) {
		filterEdit->clear();
		return true;
	}
	return QWidget::eventFilter(obj, event);
}

void YouTubeSettingsWidget::ApplyFilter(const QString &text)
{
	const QString needle = text.trimmed();
	auto matches = [&needle](const QString &haystack) {
		return haystack.contains(needle, Qt::CaseInsensitive);
	};

	bool anyShown = false;
	for (QGroupBox *section : sections) {
		const bool showAll = needle.isEmpty() || matches(section->title());
		bool sectionShown = false;

		auto filterForm = [&](QFormLayout *form) {
			for (int row = 0; row < form->rowCount(); ++row) {
				QString haystack;
				for (auto role : {QFormLayout::LabelRole, QFormLayout::FieldRole, QFormLayout::SpanningRole}) {
					if (QLayoutItem *item = form->itemAt(row, role))
						haystack += SearchText(item) + '\n';
				}
				const bool show = showAll || matches(haystack);
				form->setRowVisible(row, show);
				sectionShown |= show;
			}
		};

		if (QFormLayout *form = qobject_cast<QFormLayout *>(section->layout())) {
			filterForm(form);
		} else if (QLayout *layout = section->layout()) {
			/* Forms are filtered row by row; anything else (the priority
			 * list and its buttons, notices, button rows) as one piece */
			for (int i = 0; i < layout->count(); ++i) {
				QLayoutItem *item = layout->itemAt(i);
				if (QFormLayout *subForm = qobject_cast<QFormLayout *>(item->layout())) {
					filterForm(subForm);
					continue;
				}
				QList<QWidget *> widgets;
				CollectWidgets(item, widgets);
				if (widgets.isEmpty())
					continue;
				const bool show = showAll || matches(SearchText(item));
				for (QWidget *w : widgets)
					w->setVisible(show);
				sectionShown |= show;
			}
		}

		section->setVisible(sectionShown);
		anyShown |= sectionShown;
	}

	headerLabel->setVisible(needle.isEmpty());
	noMatchLabel->setText(QString("No YouTube settings match \"%1\".").arg(needle));
	noMatchLabel->setVisible(!anyShown);
	scrollArea->ensureVisible(0, 0);
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

	int sourceIdx = thumbnailImageSourceCombo->findData(cfg.thumbnailImageSource);
	if (sourceIdx >= 0)
		thumbnailImageSourceCombo->setCurrentIndex(sourceIdx);
	thumbnailImageModelCombo->setCurrentText(cfg.thumbnailImageModel);
	cloudImageFallbackCheck->setChecked(cfg.cloudImageFallback);
	musicEnabledCheck->setChecked(cfg.musicEnabled);
	musicSourceCombo->setCurrentIndex(std::max(0, musicSourceCombo->findData(cfg.musicSource)));
	autoFinishCombo->setCurrentIndex(std::max(0, autoFinishCombo->findData(cfg.autoFinish)));
	scriptModelCombo->setCurrentIndex(std::max(0, scriptModelCombo->findData(cfg.scriptModel)));
	scriptEffortCombo->setCurrentIndex(std::max(0, scriptEffortCombo->findData(cfg.scriptEffort)));

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
	normalizeLoudnessCheck->setChecked(cfg.normalizeLoudness);

	if (qFuzzyCompare(cfg.silenceDb, -42.0) && qFuzzyCompare(cfg.minSilenceSec, 1.20) &&
	    qFuzzyCompare(cfg.paddingSec, 0.25) && cfg.cutRetakes) {
		editingPresetCombo->setCurrentIndex(0); // Conservative
	} else if (qFuzzyCompare(cfg.silenceDb, -36.0) && qFuzzyCompare(cfg.minSilenceSec, 0.70) &&
		   qFuzzyCompare(cfg.paddingSec, 0.15) && cfg.cutRetakes) {
		editingPresetCombo->setCurrentIndex(1); // Aggressive
	} else if (qFuzzyCompare(cfg.silenceDb, -42.0) && qFuzzyCompare(cfg.minSilenceSec, 1.20) &&
		   !cfg.cutRetakes) {
		editingPresetCombo->setCurrentIndex(2); // Dead-Air Only
	} else {
		editingPresetCombo->setCurrentIndex(3); // Custom
	}

	brollEnabledCheck->setChecked(cfg.brollEnabled);
	RefreshBrollSources(cfg.brollSourceName);
	int brollStyleIdx = brollStyleCombo->findData(cfg.brollStyle);
	brollStyleCombo->setCurrentIndex(brollStyleIdx >= 0 ? brollStyleIdx : 0);
	brollCoverageSpin->setValue(cfg.brollMaxCoverage * 100.0);
	brollAmountSlider->setValue(cfg.brollAmount);
	brollImageSourceCombo->setCurrentIndex(std::max(0, brollImageSourceCombo->findData(cfg.brollImageSource)));

	internalUpdating = false;
}

void YouTubeSettingsWidget::RefreshBrollSources(const QString &selected)
{
	brollSourceCombo->clear();
	brollSourceCombo->addItem("— Select a screen source —", QString());

	auto addSource = [](void *param, obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
			QString name = QString::fromUtf8(obs_source_get_name(source));
			static_cast<QComboBox *>(param)->addItem(name, name);
		}
		return true;
	};
	obs_enum_sources(addSource, brollSourceCombo);

	int idx = brollSourceCombo->findData(selected);
	if (idx < 0 && !selected.isEmpty()) {
		brollSourceCombo->addItem(QString("%1 (missing)").arg(selected), selected);
		idx = brollSourceCombo->count() - 1;
	}
	brollSourceCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void YouTubeSettingsWidget::SaveToManager()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	YouTubeWorkflowConfig &cfg = mgr->GetConfig();

	cfg.priorityList = CurrentPriorityKeys();
	cfg.thumbnailImageSource = thumbnailImageSourceCombo->currentData().toString();
	cfg.thumbnailImageModel = thumbnailImageModelCombo->currentText().trimmed();
	cfg.cloudImageFallback = cloudImageFallbackCheck->isChecked();
	cfg.musicEnabled = musicEnabledCheck->isChecked();
	cfg.musicSource = musicSourceCombo->currentData().toString();
	cfg.autoFinish = autoFinishCombo->currentData().toString();
	cfg.scriptModel = scriptModelCombo->currentData().toString();
	cfg.scriptEffort = scriptEffortCombo->currentData().toString();
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
	cfg.normalizeLoudness = normalizeLoudnessCheck->isChecked();
	cfg.brollEnabled = brollEnabledCheck->isChecked();
	cfg.brollSourceName = brollSourceCombo->currentData().toString();
	cfg.brollStyle = brollStyleCombo->currentData().toString();
	cfg.brollMaxCoverage = brollCoverageSpin->value() / 100.0;
	cfg.brollAmount = brollAmountSlider->value();
	cfg.brollImageSource = brollImageSourceCombo->currentData().toString();

	mgr->SetVaultKey("GeminiKey", geminiKeyEdit->text());
	mgr->SetVaultKey("OpenAIKey", openaiKeyEdit->text());
	mgr->SetVaultKey("ClaudeKey", claudeKeyEdit->text());
	mgr->SetVaultKey("YouTubeClientId", oauthClientIdEdit->text());
	mgr->SetVaultKey("YouTubeClientSecret", oauthClientSecretEdit->text());

	mgr->SaveConfig();
	emit mgr->ThumbnailImageSourceChanged(cfg.thumbnailImageSource);
	emit mgr->MusicSourceChanged(cfg.musicSource);
}

void YouTubeSettingsWidget::FillMusicSources(QComboBox *combo)
{
	combo->addItem("ACE-Step on this PC (free, default)", "acestep");
	combo->addItem("Google Lyria 3.5 (paid, ~$0.08 per song, fast)", "lyria");
}

void YouTubeSettingsWidget::FillBrollImageSources(QComboBox *combo, bool compact)
{
	/* Prices: Google's paid tier per 1K picture, as CLOUD_IMAGE_PRICE in youtube_pipeline.py */
	combo->addItem(compact ? "This PC (free)" : "Stable Diffusion on this PC (free, default)", "sd");
	combo->addItem(compact ? "Nano Banana 2 (~$0.07)" : "Google Nano Banana 2 (paid, ~$0.07 per picture)",
		       "gemini");
	combo->addItem(compact ? "Nano Banana Pro (~$0.13)"
			       : "Google Nano Banana Pro (paid, ~$0.13 per picture, best quality)",
		       "gemini_pro");
}

double YouTubeSettingsWidget::BrollImagePrice(const QString &source)
{
	if (source == "gemini") {
		return 0.067;
	}
	if (source == "gemini_pro") {
		return 0.134;
	}
	return 0.0;
}

QString YouTubeSettingsWidget::BrollAmountName(int amount)
{
	static const char *names[] = {"Off", "Very few", "Few", "Some", "Many", "Lots"};
	return names[std::clamp(amount, 0, kBrollAmountMax)];
}

void YouTubeSettingsWidget::FillThumbnailImageSources(QComboBox *combo)
{
	combo->addItem("Local Stable Diffusion XL (free, on this PC, default)", "sd");
	combo->addItem("Local Ollama image model (free, macOS only for now)", "local");
	combo->addItem("Google Nano Banana 2 (paid)", "gemini");
	combo->addItem("Google Nano Banana Pro (paid, best quality)", "gemini_pro");
	combo->addItem("OpenAI GPT Image 2 (paid)", "openai");
	combo->addItem("Claude design + local render (paid)", "claude");
	combo->addItem("Off (video frames only)", "off");
}

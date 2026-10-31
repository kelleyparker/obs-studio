#include "YouTubePipelineDock.hpp"
#include "YouTubeBrollReview.hpp"
#include "YouTubeCutEditor.hpp"
#include "YouTubeMusicPicker.hpp"
#include "YouTubeSettingsWidget.hpp"
#include "YouTubeTerminalWidget.hpp"
#include "YouTubeThumbnailPicker.hpp"
#include "YouTubeVideoPreview.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <widgets/OBSBasic.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QRadioButton>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

/* What the dock shows with no job, at startup and after New Project */
static const char *const IDLE_BANNER = "🎬 Idle — Click '▶ YT' next to Start Recording to launch workflow";
static const char *const NO_ALT_TITLES = "— Select a Prospective Alternative Title to Swap —";
static const char *const NO_PROVIDER = "AI Provider: Priority Chain Ready (1 to 4)";
static const char *const NO_THUMBNAIL = "16:9 Custom Thumbnail Preview\n(1280x720 JPG)";
static const char *const NOT_UPLOADED = "YouTube Link: <i>Not uploaded yet</i>";

YouTubePipelineDock::YouTubePipelineDock(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(4, 4, 4, 4);
	outerLayout->setSpacing(4);

	QScrollArea *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);

	QWidget *container = new QWidget(scrollArea);
	QVBoxLayout *mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(8);

	// 1. Top Status Banner
	statusBanner = new QLabel(IDLE_BANNER, container);
	statusBanner->setWordWrap(true);
	statusBanner->setStyleSheet(
		"QLabel { background-color: #2b1d38; color: #f0e6ff; border: 1px solid #ff3333; "
		"border-radius: 6px; padding: 8px; font-weight: bold; }");
	mainLayout->addWidget(statusBanner);

	// 1a. The job the dock shows: put it away and start clean, or open a saved one where it was left
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QHBoxLayout *projectRow = new QHBoxLayout();
	QPushButton *newProjectBtn = new QPushButton("🆕 New Project", container);
	newProjectBtn->setToolTip("Put the current job away and start clean; its files stay next to the recording");
	QPushButton *openProjectBtn = new QPushButton("📂 Open Project…", container);
	openProjectBtn->setToolTip(
		"Pick a saved job (or a recording) and carry on where it was left, in the Step or Auto mode it ran in");
	projectRow->addWidget(newProjectBtn);
	projectRow->addWidget(openProjectBtn);
	mainLayout->addLayout(projectRow);
	connect(newProjectBtn, &QPushButton::clicked, this, [this, mgr]() {
		if (mgr->IsPipelineRunning() && !ConfirmStopRunningJob()) {
			return;
		}
		mgr->NewProject();
		OnLogMessageReceived("info",
				     "New project: click ▶ YT to record one, or Open Project to pick up a saved one.");
	});
	connect(openProjectBtn, &QPushButton::clicked, this, &YouTubePipelineDock::ShowOpenProjectDialog);

	// 1b. Step | Auto toggle and AI thumbnail source (both apply to the next run)
	QHBoxLayout *modeRow = new QHBoxLayout();
	stepModeBtn = new QPushButton("👣 Step", container);
	autoModeBtn = new QPushButton("⚡ Auto", container);
	stepModeBtn->setToolTip("Stop for your approval: cuts, B-roll, title & description, music, thumbnail");
	autoModeBtn->setToolTip("Run from recording to upload without stopping");
	QButtonGroup *modeGroup = new QButtonGroup(container);
	for (QPushButton *b : {stepModeBtn, autoModeBtn}) {
		b->setCheckable(true);
		modeGroup->addButton(b);
	}
	modeGroup->setExclusive(true);
	imageSourceCombo = new QComboBox(container);
	YouTubeSettingsWidget::FillThumbnailImageSources(imageSourceCombo);
	imageSourceCombo->setToolTip("Where AI thumbnail images come from (also in Settings → YouTube Automation)");
	modeRow->addWidget(new QLabel("Mode:", container));
	modeRow->addWidget(stepModeBtn);
	modeRow->addWidget(autoModeBtn);
	modeRow->addSpacing(10);
	modeRow->addWidget(new QLabel("AI thumbnails:", container));
	modeRow->addWidget(imageSourceCombo, 1);
	mainLayout->addLayout(modeRow);

	SetModeButtons(mgr->GetConfig().pipelineMode);
	imageSourceCombo->setCurrentIndex(std::max(0, imageSourceCombo->findData(mgr->GetConfig().thumbnailImageSource)));
	connect(stepModeBtn, &QPushButton::clicked, mgr, [mgr]() { mgr->SetPipelineMode("step"); });
	connect(autoModeBtn, &QPushButton::clicked, mgr, [mgr]() { mgr->SetPipelineMode("auto"); });
	connect(mgr, &YouTubeWorkflowManager::PipelineModeChanged, this, &YouTubePipelineDock::SetModeButtons);
	connect(imageSourceCombo, QOverload<int>::of(&QComboBox::activated), this, [this, mgr](int index) {
		mgr->SetThumbnailImageSource(imageSourceCombo->itemData(index).toString());
	});
	connect(mgr, &YouTubeWorkflowManager::ThumbnailImageSourceChanged, this, [this](const QString &source) {
		int idx = imageSourceCombo->findData(source);
		if (idx >= 0) {
			imageSourceCombo->setCurrentIndex(idx);
		}
	});

	// 1c. Review gate panel (Step mode)
	reviewPanel = new QGroupBox("👀 Waiting for you", container);
	reviewPanel->setStyleSheet("QGroupBox { border: 1px solid #ffcc44; }");
	QVBoxLayout *reviewLayout = new QVBoxLayout(reviewPanel);
	reviewLabel = new QLabel(reviewPanel);
	reviewLabel->setWordWrap(true);
	reviewLayout->addWidget(reviewLabel);
	openCutEditorBtn = new QPushButton("✂ Open Cut Editor", reviewPanel);
	reviewLayout->addWidget(openCutEditorBtn);
	connect(openCutEditorBtn, &QPushButton::clicked, this, [this]() { OpenCutEditor(false); });
	reviewWatchBtn = new QPushButton("▶ Watch Edit", reviewPanel);
	reviewWatchBtn->setToolTip("Play the rendered edit before exporting or uploading");
	reviewLayout->addWidget(reviewWatchBtn);
	connect(reviewWatchBtn, &QPushButton::clicked, this, &YouTubePipelineDock::WatchEdit);
	continueBtn = new QPushButton("▶ Continue", reviewPanel);
	continueBtn->setStyleSheet("font-weight: bold;");
	continueBtn->setToolTip("Run the rest of the job from where it stopped");
	continueBtn->hide();
	reviewLayout->addWidget(continueBtn);
	connect(continueBtn, &QPushButton::clicked, this,
		[this]() { YouTubeWorkflowManager::Instance()->ContinuePipeline(continueState); });

	/* "Ready to publish" gate */
	publishRow = new QWidget(reviewPanel);
	QGridLayout *publishGrid = new QGridLayout(publishRow);
	publishGrid->setContentsMargins(0, 0, 0, 0);
	QPushButton *uploadNowBtn = new QPushButton("⬆ Upload to YouTube", publishRow);
	uploadNowBtn->setStyleSheet("font-weight: bold;");
	QPushButton *exportBtn = new QPushButton("🎬 Export to Premiere Pro", publishRow);
	exportBtn->setToolTip("Write a Premiere Pro sequence of the edit and open it on Premiere Pro's timeline; the job "
			      "keeps waiting so you can upload later");
	QPushButton *uploadFileBtn = new QPushButton("⬆ Upload a different file…", publishRow);
	uploadFileBtn->setToolTip("Upload your own finished file (e.g. rendered from Premiere) with this title, "
				  "description and thumbnail");
	QPushButton *finishBtn = new QPushButton("📁 Finish without uploading", publishRow);
	publishGrid->addWidget(uploadNowBtn, 0, 0);
	publishGrid->addWidget(exportBtn, 0, 1);
	publishGrid->addWidget(uploadFileBtn, 1, 0);
	publishGrid->addWidget(finishBtn, 1, 1);
	reviewLayout->addWidget(publishRow);
	connect(uploadNowBtn, &QPushButton::clicked, this, [this]() {
		YouTubeWorkflowManager::Instance()->ResumePipeline(reviewState, QJsonObject{{"action", "upload"}});
	});
	connect(exportBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::ExportToPremiere);
	connect(uploadFileBtn, &QPushButton::clicked, this, [this]() {
		QString start = QFileInfo(YouTubeWorkflowManager::Instance()->GetLastVideoPath()).absolutePath();
		QString file = QFileDialog::getOpenFileName(this, "Finished video to upload", start,
							    "Videos (*.mp4 *.mov *.mkv)");
		if (!file.isEmpty()) {
			YouTubeWorkflowManager::Instance()->ResumePipeline(
				reviewState, QJsonObject{{"action", "upload_file"}, {"file", file}});
		}
	});
	connect(finishBtn, &QPushButton::clicked, this, [this]() {
		YouTubeWorkflowManager::Instance()->ResumePipeline(reviewState, QJsonObject{{"action", "finish"}});
	});
	reviewPanel->hide();
	mainLayout->addWidget(reviewPanel);

	// 2. Pipeline Stages Group (Download-like progress bars)
	QGroupBox *stagesGroup = new QGroupBox("Pipeline Progress", container);
	QVBoxLayout *stagesLayout = new QVBoxLayout(stagesGroup);
	stagesLayout->setSpacing(6);

	struct StageDef {
		QString key;
		QString title;
	};
	const QList<StageDef> stageDefs = {
		{"remux", "1. Auto-Remux MKV → MP4"},
		{"transcribe", "2. Whisper Large-v3-Turbo Transcription"},
		{"edit", "3. Cut Dead Air & Butchered Retakes"},
		{"broll", "4. Screen B-roll Plan & Review"},
		{"llm", "5. AI Title & SEO Description (Priority 1-4)"},
		{"music", "6. Background Music (ACE-Step, on this PC)"},
		{"thumbnail", "7. Custom 1280x720 AI Thumbnail"},
		{"upload", "8. YouTube Upload (UNLISTED)"}
	};

	for (const StageDef &def : stageDefs) {
		QWidget *rowWidget = new QWidget(stagesGroup);
		QVBoxLayout *rowLayout = new QVBoxLayout(rowWidget);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->setSpacing(2);

		QHBoxLayout *headerRow = new QHBoxLayout();
		QLabel *tLabel = new QLabel(def.title, rowWidget);
		tLabel->setStyleSheet("font-weight: bold;");
		QLabel *dLabel = new QLabel("Waiting", rowWidget);
		dLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
		headerRow->addWidget(tLabel);
		headerRow->addStretch();
		headerRow->addWidget(dLabel);

		QProgressBar *bar = new QProgressBar(rowWidget);
		bar->setRange(0, 100);
		bar->setValue(0);
		bar->setTextVisible(true);
		bar->setFixedHeight(16);

		rowLayout->addLayout(headerRow);
		rowLayout->addWidget(bar);
		stagesLayout->addWidget(rowWidget);

		stepRows[def.key] = StepRow{tLabel, dLabel, bar};
	}
	mainLayout->addWidget(stagesGroup);

	// 2b. B-roll review (shown while the pipeline waits for it)
	brollReview = new YouTubeBrollReview(container);
	mainLayout->addWidget(brollReview);

	// 2c. Music choice (Step mode)
	musicPicker = new YouTubeMusicPicker(container);
	mainLayout->addWidget(musicPicker);

	// 2d. Thumbnail choice (Step mode)
	thumbnailPicker = new YouTubeThumbnailPicker(container);
	mainLayout->addWidget(thumbnailPicker);

	// 3. AI Metadata & Custom Thumbnail Card
	QGroupBox *metaGroup = new QGroupBox("AI Generated Video Details & Custom Thumbnail", container);
	QVBoxLayout *metaLayout = new QVBoxLayout(metaGroup);
	metaLayout->setSpacing(6);

	providerBadgeLabel = new QLabel(NO_PROVIDER, metaGroup);
	providerBadgeLabel->setStyleSheet("color: #ffcc44; font-weight: bold; font-size: 11px;");
	metaLayout->addWidget(providerBadgeLabel);

	thumbnailPreview = new QLabel(NO_THUMBNAIL, metaGroup);
	thumbnailPreview->setAlignment(Qt::AlignCenter);
	thumbnailPreview->setMinimumSize(240, 135);
	thumbnailPreview->setMaximumHeight(165);
	thumbnailPreview->setStyleSheet(
		"QLabel { background-color: #191222; border: 1px dashed #6b4c85; border-radius: 4px; color: #9a88ab; }");
	metaLayout->addWidget(thumbnailPreview);

	QLabel *titleLbl = new QLabel("Generated Title (Current #1 Preferred):", metaGroup);
	titleEdit = new QLineEdit(metaGroup);
	titleEdit->setPlaceholderText("AI-generated YouTube title will appear here...");
	metaLayout->addWidget(titleLbl);
	metaLayout->addWidget(titleEdit);

	QLabel *altTitleLbl = new QLabel("Prospective Alternative Titles (Click to Swap Title):", metaGroup);
	alternativeTitlesCombo = new QComboBox(metaGroup);
	alternativeTitlesCombo->addItem(NO_ALT_TITLES);
	connect(alternativeTitlesCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
		if (index >= 0) {
			QString selected = alternativeTitlesCombo->itemData(index).toString();
			if (!selected.isEmpty()) {
				titleEdit->setText(selected);
			}
		}
	});
	metaLayout->addWidget(altTitleLbl);
	metaLayout->addWidget(alternativeTitlesCombo);

	QLabel *descLbl = new QLabel("Generated SEO Description & Chapters:", metaGroup);
	descriptionEdit = new QPlainTextEdit(metaGroup);
	descriptionEdit->setPlaceholderText("AI-generated description & chapter timestamps will appear here...");
	descriptionEdit->setMaximumHeight(150);
	metaLayout->addWidget(descLbl);
	metaLayout->addWidget(descriptionEdit);

	youtubeUrlLabel = new QLabel(NOT_UPLOADED, metaGroup);
	youtubeUrlLabel->setOpenExternalLinks(true);
	youtubeUrlLabel->setWordWrap(true);
	deleteBtn = new QPushButton("🗑 Delete from YouTube", metaGroup);
	deleteBtn->setToolTip("Permanently delete the uploaded video from YouTube (your recording is kept)");
	QHBoxLayout *urlRow = new QHBoxLayout();
	urlRow->addWidget(youtubeUrlLabel, 1);
	urlRow->addWidget(deleteBtn);
	metaLayout->addLayout(urlRow);
	connect(deleteBtn, &QPushButton::clicked, this, &YouTubePipelineDock::ConfirmDeleteFromYouTube);
	if (!mgr->GetLastVideoUrl().isEmpty()) {
		youtubeUrlLabel->setText(QString("🔗 <b>Last upload:</b> <a style='color:#ff6666;' href='%1'>%1</a>")
						 .arg(mgr->GetLastVideoUrl()));
	}
	deleteBtn->setVisible(!mgr->GetLastVideoId().isEmpty());

	statsLabel = new QLabel(metaGroup);
	statsLabel->setWordWrap(true);
	statsLabel->setTextFormat(Qt::PlainText);
	statsLabel->setStyleSheet("color: #bfe3c0; font-size: 11px;");
	statsLabel->hide();
	metaLayout->addWidget(statsLabel);

	submitBtn = new QPushButton("✅ Submit to YouTube", metaGroup);
	submitBtn->setToolTip("Push the title and description above to the uploaded video");
	submitBtn->setEnabled(false);
	connect(submitBtn, &QPushButton::clicked, this, [this]() {
		submitBtn->setEnabled(false);
		if (reviewGate == "metadata") {
			ApproveMetadata();
			return;
		}
		YouTubeWorkflowManager::Instance()->SubmitVideoEdits(titleEdit->text().trimmed(),
								      descriptionEdit->toPlainText());
	});
	metaLayout->addWidget(submitBtn);

	mainLayout->addWidget(metaGroup);

	// 4. Console Log (left) and embedded terminal (right)
	QGroupBox *logGroup = new QGroupBox("Workflow Console Log  |  Terminal", container);
	QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
	QSplitter *logSplitter = new QSplitter(Qt::Horizontal, logGroup);
	logSplitter->setChildrenCollapsible(false);

	consoleLog = new QPlainTextEdit(logSplitter);
	consoleLog->setReadOnly(true);
	consoleLog->setMinimumHeight(280);
	consoleLog->setMaximumBlockCount(5000);
	consoleLog->setStyleSheet(
		"QPlainTextEdit { background-color: #140e1c; color: #d8cce8; font-family: Consolas, monospace; font-size: 11px; }");

	terminal = new YouTubeTerminalWidget(logSplitter);
	terminal->setMinimumSize(160, 280);

	logSplitter->addWidget(consoleLog);
	logSplitter->addWidget(terminal);
	logSplitter->setStretchFactor(0, 1);
	logSplitter->setStretchFactor(1, 1);
	logSplitter->setSizes({1000, 1000});
	logLayout->addWidget(logSplitter);
	mainLayout->addWidget(logGroup, 1);

	// 5. Action Buttons
	QHBoxLayout *btnLayout = new QHBoxLayout();
	openFolderBtn = new QPushButton("📂 Open Folder", container);
	watchBtn = new QPushButton("▶ Watch Edit", container);
	watchBtn->setToolTip("Play the rendered edit");
	premiereBtn = new QPushButton("🎬 Export to Premiere", container);
	premiereBtn->setToolTip(
		"Write a Premiere Pro sequence (FCP XML) of the last edit, using the original recording, and open it "
		"on Premiere Pro's timeline");
	rerunBtn = new QPushButton("🔄 Start Over…", container);
	rerunBtn->setToolTip("Redo the last video from a step of your choice; the recording is kept");
	cancelBtn = new QPushButton("⏹ Cancel", container);
	cancelBtn->setEnabled(false);

	btnLayout->addWidget(openFolderBtn);
	btnLayout->addWidget(watchBtn);
	btnLayout->addWidget(premiereBtn);
	btnLayout->addWidget(rerunBtn);
	btnLayout->addWidget(cancelBtn);
	mainLayout->addLayout(btnLayout);

	scrollArea->setWidget(container);
	outerLayout->addWidget(scrollArea);

	// Connect signals from YouTubeWorkflowManager
	connect(mgr, &YouTubeWorkflowManager::PipelineStateChanged, this, &YouTubePipelineDock::OnPipelineStateChanged);
	connect(mgr, &YouTubeWorkflowManager::StepProgressUpdated, this, &YouTubePipelineDock::OnStepProgressUpdated);
	connect(mgr, &YouTubeWorkflowManager::MetadataGenerated, this, &YouTubePipelineDock::OnMetadataGenerated);
	connect(mgr, &YouTubeWorkflowManager::ThumbnailGenerated, this, &YouTubePipelineDock::OnThumbnailGenerated);
	connect(mgr, &YouTubeWorkflowManager::PipelineCompleted, this, &YouTubePipelineDock::OnPipelineCompleted);
	connect(mgr, &YouTubeWorkflowManager::LogMessageReceived, this, &YouTubePipelineDock::OnLogMessageReceived);
	connect(mgr, &YouTubeWorkflowManager::UploadStatsReady, this, &YouTubePipelineDock::OnUploadStatsReady);
	connect(mgr, &YouTubeWorkflowManager::VideoUpdateFinished, this, &YouTubePipelineDock::OnVideoUpdateFinished);
	connect(mgr, &YouTubeWorkflowManager::BrollPlanReady, brollReview, &YouTubeBrollReview::LoadPlan);
	connect(mgr, &YouTubeWorkflowManager::BrollPlanReady, this,
		[this](const QString &planPath, const QString &statePath) { OnReviewReady("broll", statePath, planPath); });
	connect(mgr, &YouTubeWorkflowManager::ReviewReady, this, &YouTubePipelineDock::OnReviewReady);
	connect(mgr, &YouTubeWorkflowManager::ReviewCleared, this, &YouTubePipelineDock::OnReviewCleared);

	connect(openFolderBtn, &QPushButton::clicked, this, [this]() {
		QString target = !currentEditedVideoPath.isEmpty()
					 ? currentEditedVideoPath
					 : YouTubeWorkflowManager::Instance()->GetLastVideoPath();
		if (!target.isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(target).absolutePath()));
		}
	});

	connect(rerunBtn, &QPushButton::clicked, this, &YouTubePipelineDock::ShowStartOverDialog);
	connect(watchBtn, &QPushButton::clicked, this, &YouTubePipelineDock::WatchEdit);
	connect(premiereBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::ExportToPremiere);
	connect(mgr, &YouTubeWorkflowManager::VideoDeleteFinished, this, &YouTubePipelineDock::OnVideoDeleteFinished);
	connect(mgr, &YouTubeWorkflowManager::PremiereExported, this, &YouTubePipelineDock::OnPremiereExported);
	connect(mgr, &YouTubeWorkflowManager::ProjectReset, this, &YouTubePipelineDock::ResetJobView);
	connect(mgr, &YouTubeWorkflowManager::ProjectOpened, this, &YouTubePipelineDock::RestoreJobView);
	connect(mgr, &YouTubeWorkflowManager::EditReady, this, &YouTubePipelineDock::RefreshEditButtons);
	connect(mgr, &YouTubeWorkflowManager::EditWillChange, this, [this]() {
		if (preview) {
			preview->Unload();
			preview->hide();
		}
	});
	RefreshEditButtons();

	connect(cancelBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);

	/* The last job comes back as it was left: waiting at a review, finished, or stopped mid-run. A review
	 * discarded with Discard Job stays discarded (its state file still names the gate). */
	const YouTubeWorkflowConfig &cfg = mgr->GetConfig();
	if (mgr->HasPendingReview()) {
		if (QFileInfo::exists(cfg.pendingReviewState) && QFileInfo::exists(cfg.pendingReviewData)) {
			RestoreJobView(cfg.pendingReviewState);
		} else {
			mgr->DiscardPendingReview();
		}
	} else if (!mgr->LastStatePath().isEmpty()) {
		QFile file(mgr->LastStatePath());
		QJsonObject ctx;
		if (file.open(QIODevice::ReadOnly)) {
			ctx = QJsonDocument::fromJson(file.readAll()).object().value("ctx").toObject();
		}
		if (ctx.value("pending_gate").toString().isEmpty()) {
			RestoreJobView(mgr->LastStatePath());
		}
	}
}

void YouTubePipelineDock::SetModeButtons(const QString &mode)
{
	stepModeBtn->setChecked(mode != "auto");
	autoModeBtn->setChecked(mode == "auto");
}

void YouTubePipelineDock::OnReviewReady(const QString &gate, const QString &statePath, const QString &dataPath)
{
	reviewGate = gate;
	reviewState = statePath;
	reviewData = dataPath;
	cancelBtn->setEnabled(true);
	cancelBtn->setText("⏹ Discard Job");

	continueState.clear();
	continueBtn->hide();
	openCutEditorBtn->setVisible(gate == "cuts");
	reviewWatchBtn->setVisible(gate == "metadata" || gate == "music" || gate == "thumbnail" || gate == "publish");
	publishRow->setVisible(gate == "publish");
	RefreshEditButtons();
	submitBtn->setText("✅ Submit to YouTube");
	if (gate == "cuts") {
		reviewLabel->setText("Review the dead-air and retake cuts in the Cut Editor. Nothing is rendered until "
				     "you click Approve & Render there.");
		OpenCutEditor(true);
	} else if (gate == "broll") {
		reviewLabel->setText("Accept or reject the B-roll suggestions below, then Render & Upload.");
	} else if (gate == "metadata") {
		reviewLabel->setText("Edit the title and description below, then click Approve & Continue.");
		submitBtn->setText("✅ Approve & Continue");
		submitBtn->setEnabled(true);
	} else if (gate == "music") {
		reviewLabel->setText("Listen to the songs below, on their own or under your voice, and pick one (or no "
				     "music). You can also make new ones from your own style prompt.");
		musicPicker->Load(dataPath, statePath);
	} else if (gate == "thumbnail") {
		reviewLabel->setText("Pick a thumbnail below, or tweak the AI images and make new ones. Nothing is "
				     "uploaded yet.");
		thumbnailPicker->LoadCandidates(dataPath, statePath);
	} else if (gate == "publish") {
		reviewLabel->setText("Everything is ready. Upload it to YouTube, export it for Premiere Pro to fine-tune "
				     "(the job keeps waiting), upload your own finished file, or finish here.");
	}
	reviewPanel->show();
}

void YouTubePipelineDock::OnReviewCleared()
{
	reviewGate.clear();
	reviewState.clear();
	reviewData.clear();
	continueState.clear();
	continueBtn->hide();
	reviewPanel->hide();
	musicPicker->Close();
	thumbnailPicker->hide();
	brollReview->hide();
	submitBtn->setText("✅ Submit to YouTube");
	cancelBtn->setText("⏹ Cancel");
	if (cutEditor) {
		cutEditor->close();
	}
}

void YouTubePipelineDock::OpenCutEditor(bool reload)
{
	if (reviewGate != "cuts") {
		return;
	}
	bool fresh = !cutEditor;
	if (fresh) {
		cutEditor = new YouTubeCutEditor(window());
		cutEditor->setAttribute(Qt::WA_DeleteOnClose, false);
		connect(cutEditor, &YouTubeCutEditor::Approved, this, [](const QString &statePath) {
			YouTubeWorkflowManager::Instance()->ResumePipeline(statePath);
		});
	}
	if ((fresh || reload || cutEditor->StatePath() != reviewState) && !cutEditor->Load(reviewData, reviewState)) {
		OnLogMessageReceived("error", QString("Could not open the cut list: %1").arg(reviewData));
		return;
	}
	cutEditor->show();
	cutEditor->raise();
	cutEditor->activateWindow();
}

void YouTubePipelineDock::ApproveMetadata()
{
	QFile in(reviewData);
	QJsonObject meta;
	if (in.open(QIODevice::ReadOnly)) {
		meta = QJsonDocument::fromJson(in.readAll()).object();
		in.close();
	}
	meta["title"] = titleEdit->text().trimmed();
	meta["description"] = descriptionEdit->toPlainText();

	QSaveFile out(reviewData);
	if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(meta).toJson()) < 0 || !out.commit()) {
		OnLogMessageReceived("error", QString("Could not save %1").arg(reviewData));
		submitBtn->setEnabled(true);
		return;
	}
	YouTubeWorkflowManager::Instance()->ResumePipeline(reviewState);
}

void YouTubePipelineDock::OnPipelineStateChanged(const QString &bannerText, bool running)
{
	statusBanner->setText(bannerText);
	cancelBtn->setEnabled(running || !reviewGate.isEmpty());
	/* Making new thumbnails or songs keeps the job waiting at its step */
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (running && !mgr->IsRegeneratingThumbnails() && !mgr->IsRegeneratingMusic()) {
		/* A new run (or the next stage) replaces whatever was being reviewed or offered to continue */
		if (!reviewGate.isEmpty() || !continueState.isEmpty()) {
			OnReviewCleared();
		}
		/* A new run replaces the previous upload, so its stats and Submit target are stale. */
		submitBtn->setEnabled(false);
		statsLabel->hide();
		brollReview->hide();
	}
}

void YouTubePipelineDock::OnStepProgressUpdated(const QString &stepName, int percent, const QString &message)
{
	if (!stepRows.contains(stepName)) {
		return;
	}
	StepRow &row = stepRows[stepName];
	row.bar->setValue(percent);
	row.detailLabel->setText(message);
}

void YouTubePipelineDock::OnMetadataGenerated(const QString &title, const QStringList &alternativeTitles,
					      const QString &description, const QString &hookText,
					      const QString &providerBadge)
{
	titleEdit->setText(title);
	descriptionEdit->setPlainText(description);
	providerBadgeLabel->setText(QString("✨ Metadata via %1 | Hook: \"%2\"").arg(providerBadge, hookText));

	alternativeTitlesCombo->clear();
	alternativeTitlesCombo->addItem(QString("Primary #1 (Current): %1").arg(title), title);
	int num = 2;
	for (const QString &alt : alternativeTitles) {
		QString trimmed = alt.trimmed();
		if (!trimmed.isEmpty()) {
			alternativeTitlesCombo->addItem(QString("#%1: %2").arg(num++).arg(trimmed), trimmed);
		}
	}
}

void YouTubePipelineDock::OnThumbnailGenerated(const QString &thumbnailPath, const QString &providerBadge)
{
	currentThumbnailPath = thumbnailPath;
	providerBadgeLabel->setText(providerBadgeLabel->text() + QString(" | Thumbnail: %1").arg(providerBadge));
	if (QFileInfo::exists(thumbnailPath)) {
		QPixmap pix(thumbnailPath);
		if (!pix.isNull()) {
			thumbnailPreview->setPixmap(
				pix.scaled(thumbnailPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
		}
	}
}

void YouTubePipelineDock::OnPipelineCompleted(const QString &videoUrl, const QString &editedVideoPath,
					      const QString &thumbnailPath, const QString &message)
{
	currentEditedVideoPath = editedVideoPath;
	if (!thumbnailPath.isEmpty()) {
		OnThumbnailGenerated(thumbnailPath, "Ready");
	}
	submitBtn->setEnabled(!videoUrl.isEmpty());
	if (!videoUrl.isEmpty()) {
		youtubeUrlLabel->setText(
			QString("🔗 <b>Uploaded (Unlisted):</b> <a style='color:#ff6666;' href='%1'>%1</a>").arg(videoUrl));
	} else {
		youtubeUrlLabel->setText(QString("📁 <b>Local Output Ready:</b> %1").arg(message));
	}
	deleteBtn->setVisible(!YouTubeWorkflowManager::Instance()->GetLastVideoId().isEmpty());
	RefreshEditButtons();
}

void YouTubePipelineDock::RefreshEditButtons()
{
	QString video;
	QList<double> joins;
	bool hasEdit = YouTubeWorkflowManager::Instance()->GetEditInfo(video, joins);
	watchBtn->setEnabled(hasEdit);
	reviewWatchBtn->setEnabled(hasEdit);
	premiereBtn->setEnabled(!YouTubeWorkflowManager::Instance()->LastStatePath().isEmpty());
}

void YouTubePipelineDock::WatchEdit()
{
	QString video;
	QList<double> joins;
	if (!YouTubeWorkflowManager::Instance()->GetEditInfo(video, joins)) {
		OnLogMessageReceived("warning", "There's no rendered edit to watch yet.");
		return;
	}
	if (!preview) {
		preview = new YouTubeVideoPreview(window());
	}
	preview->Load(video, joins, descriptionEdit->toPlainText());
	preview->show();
	preview->raise();
	preview->activateWindow();
}

void YouTubePipelineDock::ShowStartOverDialog()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QString video = mgr->GetLastVideoPath();
	if (video.isEmpty() || !QFileInfo::exists(video)) {
		OnLogMessageReceived("warning", "There's no recording to start over with.");
		return;
	}

	/* Which steps the last run got to (from its saved state) */
	QJsonObject ctx;
	QFile file(mgr->LastStatePath());
	if (file.open(QIODevice::ReadOnly)) {
		ctx = QJsonDocument::fromJson(file.readAll()).object().value("ctx").toObject();
	}
	bool hasCuts = !ctx.value("keep_intervals").toArray().isEmpty();
	bool hasRender = QFileInfo::exists(ctx.value("final_video").toString());
	bool hasMetadata = !ctx.value("metadata").toObject().isEmpty();

	QDialog dialog(this);
	dialog.setWindowTitle("Start over");
	QVBoxLayout *layout = new QVBoxLayout(&dialog);
	layout->addWidget(new QLabel(QString("Redo <b>%1</b> from:").arg(QFileInfo(video).fileName().toHtmlEscaped()),
				     &dialog));
	struct Choice {
		QString from;
		QString label;
		bool enabled;
	};
	const QList<Choice> choices = {
		{"transcribe", "2. Transcription (everything again)", true},
		{"cuts", "3. Cuts (reopens the Cut Editor with your last cuts)", hasCuts},
		{"metadata", "5. Title && description", hasRender},
		{"music", "6. Music", hasMetadata},
		{"thumbnails", "7. Thumbnail", hasMetadata},
	};
	QButtonGroup *group = new QButtonGroup(&dialog);
	for (const Choice &c : choices) {
		QRadioButton *radio = new QRadioButton(c.label, &dialog);
		radio->setEnabled(c.enabled);
		radio->setProperty("from", c.from);
		group->addButton(radio);
		layout->addWidget(radio);
		if (c.enabled) {
			radio->setChecked(true); /* the latest step it got to */
		}
	}

	QCheckBox *deleteCheck = nullptr;
	if (!mgr->GetLastVideoId().isEmpty()) {
		deleteCheck = new QCheckBox(
			QString("Also delete the uploaded video from YouTube (%1)")
				.arg(mgr->GetLastVideoUrl().isEmpty() ? mgr->GetLastVideoId() : mgr->GetLastVideoUrl()),
			&dialog);
		deleteCheck->setChecked(true);
		layout->addWidget(deleteCheck);
	}
	QLabel *note = new QLabel(QString("Your recording %1 is kept.").arg(QFileInfo(video).fileName()), &dialog);
	note->setStyleSheet("color: #bfe3c0;");
	layout->addWidget(note);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	buttons->button(QDialogButtonBox::Ok)->setText("🔄 Start Over");
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	if (dialog.exec() != QDialog::Accepted || !group->checkedButton()) {
		return;
	}
	mgr->RedoPipeline(group->checkedButton()->property("from").toString(), deleteCheck && deleteCheck->isChecked());
}

bool YouTubePipelineDock::ConfirmStopRunningJob()
{
	return QMessageBox::question(this, "A step is still running",
				     "The current job is still working on a step. Stop it?\n\nWhat it finished before is "
				     "saved, so Open Project can continue it later.",
				     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void YouTubePipelineDock::ResetJobView()
{
	OnReviewCleared();
	titleEdit->clear();
	descriptionEdit->clear();
	alternativeTitlesCombo->clear();
	alternativeTitlesCombo->addItem(NO_ALT_TITLES);
	providerBadgeLabel->setText(NO_PROVIDER);
	thumbnailPreview->setText(NO_THUMBNAIL);
	youtubeUrlLabel->setText(NOT_UPLOADED);
	deleteBtn->hide();
	statsLabel->hide();
	submitBtn->setEnabled(false);
	for (StepRow &row : stepRows) {
		row.bar->setValue(0);
		row.detailLabel->setText("Waiting");
	}
	currentEditedVideoPath.clear();
	currentThumbnailPath.clear();
	statusBanner->setText(IDLE_BANNER);
	cancelBtn->setEnabled(YouTubeWorkflowManager::Instance()->IsPipelineRunning());
	RefreshEditButtons();
}

void YouTubePipelineDock::RestoreJobView(const QString &statePath)
{
	ResetJobView();
	QFile file(statePath);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
	QJsonObject ctx = state.value("ctx").toObject();
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	const YouTubeWorkflowConfig &cfg = mgr->GetConfig();
	QFileInfo video(ctx.value("input_video").toString());
	QString next = ctx.value("next_stage").toString("render");

	/* Every step before the job's next stage is done */
	const QStringList left = YouTubeWorkflowManager::StepsLeft(next);
	bool musicOn = state.value("config").toObject().value("music_enabled").toBool(true);
	for (auto it = stepRows.begin(); it != stepRows.end(); ++it) {
		bool done = !left.contains(it.key());
		it->bar->setValue(done ? 100 : 0);
		it->detailLabel->setText(it.key() == "music" && !musicOn ? "Off" : (done ? "Done" : "Waiting"));
	}

	/* Waiting at the title review, the dock shows the copy being reviewed */
	QJsonObject meta = ctx.value("metadata").toObject();
	QString provider = ctx.value("llm_provider").toString();
	bool waiting = !cfg.pendingReviewGate.isEmpty() && cfg.pendingReviewState == statePath;
	if (waiting && cfg.pendingReviewGate == "metadata") {
		QFile metaFile(cfg.pendingReviewData);
		if (metaFile.open(QIODevice::ReadOnly)) {
			meta = QJsonDocument::fromJson(metaFile.readAll()).object();
			provider = meta.value("provider").toString(provider);
		}
	}
	if (!meta.isEmpty()) {
		QStringList alts;
		for (const QJsonValue &v : meta.value("alternative_titles").toArray()) {
			alts.append(v.toString());
		}
		OnMetadataGenerated(meta.value("title").toString(), alts, meta.value("description").toString(),
				    meta.value("thumbnail_hook").toString(), provider);
	}
	QString thumb = video.absoluteDir().filePath(video.completeBaseName() + "_yt_thumb.jpg");
	if (QFileInfo::exists(thumb)) {
		OnThumbnailGenerated(thumb, ctx.value("thumb_provider").toString("Ready"));
	}
	if (QFileInfo::exists(ctx.value("final_video").toString())) {
		currentEditedVideoPath = ctx.value("final_video").toString();
	}
	QString url = mgr->GetLastVideoUrl();
	if (!url.isEmpty()) {
		youtubeUrlLabel->setText(QString("🔗 <b>Uploaded:</b> <a style='color:#ff6666;' href='%1'>%1</a>")
						 .arg(url.toHtmlEscaped()));
		submitBtn->setEnabled(true);
	}
	deleteBtn->setVisible(!mgr->GetLastVideoId().isEmpty());

	statusBanner->setText(QString("%1 — %2").arg(YouTubeWorkflowManager::ProjectStatus(state), video.fileName()));
	if (waiting) {
		if (cfg.pendingReviewGate == "broll") {
			brollReview->LoadPlan(cfg.pendingReviewData, statePath);
		}
		OnReviewReady(cfg.pendingReviewGate, statePath, cfg.pendingReviewData);
	} else if (next != "done" && ctx.value("pending_gate").toString().isEmpty()) {
		continueState = statePath;
		reviewLabel->setText("This job didn't finish: OBS was closed or the job was stopped partway. Continue "
				     "runs the rest from where it stopped, in the Step or Auto mode it was started in.");
		openCutEditorBtn->hide();
		reviewWatchBtn->hide();
		publishRow->hide();
		continueBtn->show();
		reviewPanel->show();
	}
	RefreshEditButtons();
}

void YouTubePipelineDock::ShowOpenProjectDialog()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (mgr->IsPipelineRunning()) {
		if (!ConfirmStopRunningJob()) {
			return;
		}
		mgr->NewProject();
	}

	QString folder = mgr->GetLastVideoPath().isEmpty() ? QString()
							   : QFileInfo(mgr->GetLastVideoPath()).absolutePath();
	if (folder.isEmpty() || !QDir(folder).exists()) {
		folder = QString::fromUtf8(OBSBasic::Get()->GetCurrentOutputPath());
	}

	QDialog dialog(this);
	dialog.setWindowTitle("Open Project");
	dialog.resize(620, 460);
	QVBoxLayout *layout = new QVBoxLayout(&dialog);
	QLabel *folderLabel = new QLabel(&dialog);
	folderLabel->setWordWrap(true);
	layout->addWidget(folderLabel);
	QListWidget *list = new QListWidget(&dialog);
	list->setAlternatingRowColors(true);
	list->setSpacing(2);
	layout->addWidget(list, 1);
	QLabel *hint = new QLabel("A job opens where you left it: at its review, finished, or ready to continue, in "
				  "the Step or Auto mode it ran in. A recording that never went through the pipeline "
				  "starts it.",
				  &dialog);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	layout->addWidget(hint);
	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
	QPushButton *browseBtn = buttons->addButton("📁 Browse folder…", QDialogButtonBox::ActionRole);
	layout->addWidget(buttons);

	/* Saved jobs ({stem}_yt_state.json) and recordings with none yet, newest first */
	auto fill = [&](const QString &dir, const QString &selectStem) {
		folder = dir;
		folderLabel->setText(
			QString("Jobs and recordings in <b>%1</b>").arg(QDir::toNativeSeparators(dir).toHtmlEscaped()));
		list->clear();
		struct Row {
			QDateTime when;
			QString text;
			QString path;
			QString stem;
		};
		QList<Row> rows;
		QSet<QString> started;
		QString current = mgr->LastStatePath();
		QDir d(dir);
		for (const QFileInfo &f : d.entryInfoList({"*_yt_state.json"}, QDir::Files)) {
			QFile file(f.absoluteFilePath());
			if (!file.open(QIODevice::ReadOnly)) {
				continue;
			}
			QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
			QString stem = f.fileName().chopped(QStringLiteral("_yt_state.json").size());
			started.insert(stem);
			QJsonObject meta = state.value("ctx").toObject().value("metadata").toObject();
			QString title = meta.value("title").toString();
			QString status = YouTubeWorkflowManager::ProjectStatus(state);
			if (!current.isEmpty() && QFileInfo(current) == f) {
				status += "  (open now)";
			}
			rows.append({f.lastModified(),
				     QString("%1\n%2  ·  %3  ·  %4")
					     .arg(title.isEmpty() ? stem : title, status, stem,
						  f.lastModified().toString("MMM d, h:mm AP")),
				     f.absoluteFilePath(), stem});
		}
		/* One row per recording, the remuxed MP4 before the MKV it came from */
		QFileInfoList videos = d.entryInfoList({"*.mp4"}, QDir::Files) +
				       d.entryInfoList({"*.mov", "*.mkv"}, QDir::Files);
		for (const QFileInfo &f : videos) {
			QString stem = f.completeBaseName();
			/* The pipeline's own outputs and B-roll screen recordings aren't projects */
			if (stem.contains("_yt_") || stem.endsWith("_broll_screen") || started.contains(stem)) {
				continue;
			}
			started.insert(stem);
			QString when = f.lastModified().toString("MMM d, h:mm AP");
			rows.append({f.lastModified(), QString("%1\n🎥 Not started  ·  %2").arg(f.fileName(), when),
				     f.absoluteFilePath(), stem});
		}
		std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.when > b.when; });
		for (const Row &row : rows) {
			QListWidgetItem *item = new QListWidgetItem(row.text, list);
			item->setData(Qt::UserRole, row.path);
			item->setToolTip(QDir::toNativeSeparators(row.path));
			if (row.stem == selectStem) {
				list->setCurrentItem(item);
			}
		}
		if (!list->currentItem() && list->count() > 0) {
			list->setCurrentRow(0);
		}
		buttons->button(QDialogButtonBox::Open)->setEnabled(list->count() > 0);
	};
	QString lastVideo = mgr->GetLastVideoPath();
	fill(folder, lastVideo.isEmpty() ? QString() : QFileInfo(lastVideo).completeBaseName());

	connect(browseBtn, &QPushButton::clicked, &dialog, [&]() {
		QString dir = QFileDialog::getExistingDirectory(
			&dialog, "Folder with your recordings or a _premiere folder", folder);
		if (dir.isEmpty()) {
			return;
		}
		/* A {stem}_premiere export folder belongs to the recording next to it */
		QFileInfo picked(dir);
		if (picked.fileName().endsWith("_premiere")) {
			fill(picked.absolutePath(), picked.fileName().chopped(QStringLiteral("_premiere").size()));
		} else {
			fill(dir, QString());
		}
	});
	connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	if (dialog.exec() != QDialog::Accepted || !list->currentItem()) {
		return;
	}

	QString path = list->currentItem()->data(Qt::UserRole).toString();
	if (!path.endsWith("_yt_state.json")) {
		QString question = QString("Start the pipeline on %1 in %2 mode?")
					   .arg(QFileInfo(path).fileName(),
						mgr->GetConfig().pipelineMode == "auto" ? "Auto" : "Step");
		if (QMessageBox::question(this, "Start the pipeline", question, QMessageBox::Yes | QMessageBox::Cancel,
					  QMessageBox::Yes) == QMessageBox::Yes) {
			/* Put the current job (and its recording's B-roll) away first */
			mgr->NewProject();
			mgr->StartPipelineForVideo(path);
		}
		return;
	}
	QString error;
	if (!mgr->OpenProject(path, error)) {
		QMessageBox::warning(this, "Open Project", QString("Could not open this project.\n\n%1").arg(error));
		return;
	}
	OnLogMessageReceived("info", QString("Opened %1").arg(QDir::toNativeSeparators(path)));
}

void YouTubePipelineDock::ConfirmDeleteFromYouTube()
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QString url = mgr->GetLastVideoUrl().isEmpty() ? mgr->GetLastVideoId() : mgr->GetLastVideoUrl();
	QMessageBox::StandardButton answer = QMessageBox::warning(
		this, "Delete from YouTube",
		QString("Permanently delete this video from YouTube?\n\n%1\n%2\n\nThis can't be undone. Your recording "
			"and the edited file on this PC are kept.")
			.arg(titleEdit->text(), url),
		QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
	if (answer == QMessageBox::Yes) {
		deleteBtn->setEnabled(false);
		mgr->DeleteUploadedVideo();
	}
}

void YouTubePipelineDock::OnVideoDeleteFinished(bool ok, const QString &message)
{
	deleteBtn->setEnabled(true);
	OnLogMessageReceived(ok ? "info" : "error", message);
	statusBanner->setText(ok ? QString("🗑 %1").arg(message) : QString("❌ %1").arg(message));
	if (ok) {
		youtubeUrlLabel->setText("🗑 <b>Deleted from YouTube</b>");
		submitBtn->setEnabled(false);
		deleteBtn->hide();
	}
}

/* The newest installed Premiere Pro (a release before a beta), or empty */
static QString FindPremierePro()
{
	QDir adobe("C:/Program Files/Adobe");
	QStringList dirs = adobe.entryList({"Adobe Premiere Pro*"}, QDir::Dirs, QDir::Name | QDir::Reversed);
	std::stable_partition(dirs.begin(), dirs.end(), [](const QString &d) { return !d.contains("Beta"); });
	for (const QString &d : dirs) {
		QString exe = adobe.filePath(d + "/Adobe Premiere Pro.exe");
		if (QFileInfo::exists(exe)) {
			return exe;
		}
	}
	return QString();
}

/* Whether a copy of exe is running already */
static bool IsRunning(const QString &exe)
{
#ifdef _WIN32
	std::wstring name = QFileInfo(exe).fileName().toStdWString();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	bool found = false;
	for (BOOL more = Process32FirstW(snap, &entry); more && !found; more = Process32NextW(snap, &entry)) {
		found = _wcsicmp(entry.szExeFile, name.c_str()) == 0;
	}
	CloseHandle(snap);
	return found;
#else
	Q_UNUSED(exe);
	return false;
#endif
}

/* The 8.3 form of a path, so a command line can't split it at its spaces */
static QString ShortPath(const QString &path)
{
	QString native = QDir::toNativeSeparators(path);
#ifdef _WIN32
	std::wstring wide = native.toStdWString();
	DWORD size = GetShortPathNameW(wide.c_str(), nullptr, 0);
	if (size > 0) {
		std::wstring buffer(size, L'\0');
		DWORD len = GetShortPathNameW(wide.c_str(), buffer.data(), size);
		if (len > 0 && len < size) {
			return QString::fromStdWString(buffer.substr(0, len));
		}
	}
#endif
	return native;
}

/* A JavaScript string literal (JSON's escaping) */
static QString JsString(const QString &text)
{
	QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(json.mid(1, json.size() - 2));
}

void YouTubePipelineDock::OnPremiereExported(const QString &xmlPath, bool open)
{
	statusBanner->setText(QString("🎬 Exported for Premiere Pro: %1").arg(xmlPath));
	if (!open) {
		OnLogMessageReceived("info", "Click Export to Premiere to open it in Premiere Pro.");
		return;
	}
	QString exe = FindPremierePro();
	QString nativeXml = QDir::toNativeSeparators(xmlPath);
	if (exe.isEmpty()) {
		OnLogMessageReceived("warning", "Premiere Pro isn't installed in C:\\Program Files\\Adobe. Open " +
							nativeXml + " in it to edit the sequence.");
		return;
	}
	/* Premiere only runs a script it is started with; a running copy ignores a second start */
	if (IsRunning(exe)) {
		OnLogMessageReceived("warning", "Premiere Pro is already open, and it only opens the sequence for OBS "
						"as it starts. Save your work, close Premiere, and click Export to "
						"Premiere again. Or, in Premiere, use File > Import and pick " +
							nativeXml);
		return;
	}

	/* The project goes next to the export, never over one you've already edited */
	QFileInfo xml(xmlPath);
	QString project = xml.absoluteDir().filePath(xml.completeBaseName() + ".prproj");
	for (int n = 2; QFileInfo::exists(project); n++) {
		project = xml.absoluteDir().filePath(QString("%1 %2.prproj").arg(xml.completeBaseName()).arg(n));
	}
	/* Premiere converts the XML into %TEMP%\{its name}.prproj, so a copy with a new name each time keeps an
	 * earlier conversion from getting in the way */
	QDir temp(QDir::temp().filePath("obs_premiere"));
	temp.mkpath(".");
	QString xmlCopy = temp.filePath(QString("%1 %2.xml")
						 .arg(xml.completeBaseName(),
						      QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
	QString scriptPath = temp.filePath("open_in_premiere.jsx");
	QString script = QString("// Written by OBS: opens the exported sequence on Premiere Pro's timeline\n"
				 "var xml = %1;\n"
				 "var project = %2;\n"
				 "if (app.openFCPXML(xml, project)) {\n"
				 "\tvar sequences = app.project.sequences;\n"
				 "\tif (sequences.numSequences > 0) {\n"
				 "\t\tapp.project.openSequence(sequences[0].sequenceID);\n"
				 "\t}\n"
				 "\tapp.project.saveAs(project);\n"
				 "}\n")
				 .arg(JsString(QDir::toNativeSeparators(xmlCopy)),
				      JsString(QDir::toNativeSeparators(project)));
	QFile scriptFile(scriptPath);
	/* With a BOM, ExtendScript reads it as UTF-8 rather than the ANSI code page */
	if (!QFile::copy(xmlPath, xmlCopy) || !scriptFile.open(QIODevice::WriteOnly) ||
	    scriptFile.write("\xEF\xBB\xBF" + script.toUtf8()) < 0) {
		OnLogMessageReceived("error", QString("Could not write the Premiere Pro script in %1")
						      .arg(QDir::toNativeSeparators(temp.absolutePath())));
		return;
	}
	scriptFile.close();

	if (QProcess::startDetached(exe, {"/C", "es.processFile", ShortPath(scriptPath)})) {
		OnLogMessageReceived("info", QString("Opening the sequence on Premiere Pro's timeline (project: %1). Drag "
						     "any cut's edge to widen or tighten it.")
						     .arg(QDir::toNativeSeparators(project)));
	} else {
		OnLogMessageReceived("error", QString("Could not start %1").arg(exe));
	}
}

void YouTubePipelineDock::OnUploadStatsReady(const QString &statsText)
{
	if (statsText.isEmpty()) {
		return;
	}
	statsLabel->setText(QString("📊 Final video stats (file as uploaded)\n%1").arg(statsText));
	statsLabel->show();
}

void YouTubePipelineDock::OnVideoUpdateFinished(bool ok, const QString &message)
{
	/* Re-enable so a failed submit (e.g. needs Google reconnect) can be retried. */
	submitBtn->setEnabled(true);
	OnLogMessageReceived(ok ? "info" : "error", message);
	statusBanner->setText(ok ? QString("✅ %1").arg(message) : QString("❌ %1").arg(message));
}

void YouTubePipelineDock::OnLogMessageReceived(const QString &level, const QString &message)
{
	QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
	QString prefix = (level == "error") ? "[ERR]" : (level == "warning" ? "[WARN]" : "[INFO]");
	consoleLog->appendPlainText(QString("%1 %2 %3").arg(ts, prefix, message));
}

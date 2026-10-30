#include "YouTubePipelineDock.hpp"
#include "YouTubeBrollReview.hpp"
#include "YouTubeCutEditor.hpp"
#include "YouTubeSettingsWidget.hpp"
#include "YouTubeTerminalWidget.hpp"
#include "YouTubeThumbnailPicker.hpp"
#include "YouTubeVideoPreview.hpp"
#include "YouTubeWorkflowManager.hpp"

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
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

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
	statusBanner = new QLabel("🎬 Idle — Click '▶ YT' next to Start Recording to launch workflow", container);
	statusBanner->setWordWrap(true);
	statusBanner->setStyleSheet(
		"QLabel { background-color: #2b1d38; color: #f0e6ff; border: 1px solid #ff3333; "
		"border-radius: 6px; padding: 8px; font-weight: bold; }");
	mainLayout->addWidget(statusBanner);

	// 1b. Step | Auto toggle and AI thumbnail source (both apply to the next run)
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QHBoxLayout *modeRow = new QHBoxLayout();
	stepModeBtn = new QPushButton("👣 Step", container);
	autoModeBtn = new QPushButton("⚡ Auto", container);
	stepModeBtn->setToolTip("Stop for your approval: cuts, B-roll, title & description, thumbnail");
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

	/* "Ready to publish" gate */
	publishRow = new QWidget(reviewPanel);
	QGridLayout *publishGrid = new QGridLayout(publishRow);
	publishGrid->setContentsMargins(0, 0, 0, 0);
	QPushButton *uploadNowBtn = new QPushButton("⬆ Upload to YouTube", publishRow);
	uploadNowBtn->setStyleSheet("font-weight: bold;");
	QPushButton *exportBtn = new QPushButton("🎬 Export to Premiere Pro", publishRow);
	exportBtn->setToolTip("Write a Premiere Pro sequence of the edit; the job keeps waiting so you can upload later");
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
		{"thumbnail", "6. Custom 1280x720 AI Thumbnail"},
		{"upload", "7. YouTube Upload (UNLISTED)"}
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

	// 2c. Thumbnail choice (Step mode)
	thumbnailPicker = new YouTubeThumbnailPicker(container);
	mainLayout->addWidget(thumbnailPicker);

	// 3. AI Metadata & Custom Thumbnail Card
	QGroupBox *metaGroup = new QGroupBox("AI Generated Video Details & Custom Thumbnail", container);
	QVBoxLayout *metaLayout = new QVBoxLayout(metaGroup);
	metaLayout->setSpacing(6);

	providerBadgeLabel = new QLabel("AI Provider: Priority Chain Ready (1 to 4)", metaGroup);
	providerBadgeLabel->setStyleSheet("color: #ffcc44; font-weight: bold; font-size: 11px;");
	metaLayout->addWidget(providerBadgeLabel);

	thumbnailPreview = new QLabel("16:9 Custom Thumbnail Preview\n(1280x720 JPG)", metaGroup);
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
	alternativeTitlesCombo->addItem("— Select a Prospective Alternative Title to Swap —");
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

	youtubeUrlLabel = new QLabel("YouTube Link: <i>Not uploaded yet</i>", metaGroup);
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
	premiereBtn->setToolTip("Write a Premiere Pro sequence (FCP XML) of the last edit, using the original recording");
	openPremiereBtn = new QPushButton("🚀 Open Premiere Pro", container);
	openPremiereBtn->hide();
	rerunBtn = new QPushButton("🔄 Start Over…", container);
	rerunBtn->setToolTip("Redo the last video from a step of your choice; the recording is kept");
	cancelBtn = new QPushButton("⏹ Cancel", container);
	cancelBtn->setEnabled(false);

	btnLayout->addWidget(openFolderBtn);
	btnLayout->addWidget(watchBtn);
	btnLayout->addWidget(premiereBtn);
	btnLayout->addWidget(openPremiereBtn);
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
	connect(openPremiereBtn, &QPushButton::clicked, this, [this]() {
		QString exe = openPremiereBtn->property("exe").toString();
		if (!QProcess::startDetached(exe, {})) {
			OnLogMessageReceived("error", QString("Could not start %1").arg(exe));
		}
	});
	connect(mgr, &YouTubeWorkflowManager::VideoDeleteFinished, this, &YouTubePipelineDock::OnVideoDeleteFinished);
	connect(mgr, &YouTubeWorkflowManager::PremiereExported, this, &YouTubePipelineDock::OnPremiereExported);
	connect(mgr, &YouTubeWorkflowManager::EditReady, this, &YouTubePipelineDock::RefreshEditButtons);
	connect(mgr, &YouTubeWorkflowManager::EditWillChange, this, [this]() {
		if (preview) {
			preview->Unload();
			preview->hide();
		}
	});
	RefreshEditButtons();

	connect(cancelBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);

	/* A run paused before OBS was closed is still waiting */
	const YouTubeWorkflowConfig &cfg = mgr->GetConfig();
	if (mgr->HasPendingReview() && QFileInfo::exists(cfg.pendingReviewState) &&
	    QFileInfo::exists(cfg.pendingReviewData)) {
		QString gate = cfg.pendingReviewGate;
		QString state = cfg.pendingReviewState;
		QString data = cfg.pendingReviewData;
		statusBanner->setText("👀 A YouTube job is still waiting for your review");
		if (gate == "broll") {
			brollReview->LoadPlan(data, state);
		} else if (gate == "metadata") {
			QFile file(data);
			if (file.open(QIODevice::ReadOnly)) {
				QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();
				QStringList alts;
				for (const QJsonValue &v : meta.value("alternative_titles").toArray()) {
					alts.append(v.toString());
				}
				OnMetadataGenerated(meta.value("title").toString(), alts, meta.value("description").toString(),
						    meta.value("thumbnail_hook").toString(), meta.value("provider").toString());
			}
		} else if (gate == "thumbnail" || gate == "publish") {
			QFile file(state);
			if (file.open(QIODevice::ReadOnly)) {
				QJsonObject ctx = QJsonDocument::fromJson(file.readAll()).object().value("ctx").toObject();
				QJsonObject meta = ctx.value("metadata").toObject();
				QStringList alts;
				for (const QJsonValue &v : meta.value("alternative_titles").toArray()) {
					alts.append(v.toString());
				}
				OnMetadataGenerated(meta.value("title").toString(), alts, meta.value("description").toString(),
						    meta.value("thumbnail_hook").toString(), ctx.value("llm_provider").toString());
			}
		}
		OnReviewReady(gate, state, data);
	} else if (mgr->HasPendingReview()) {
		mgr->DiscardPendingReview();
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

	openCutEditorBtn->setVisible(gate == "cuts");
	reviewWatchBtn->setVisible(gate == "metadata" || gate == "thumbnail" || gate == "publish");
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
	reviewPanel->hide();
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
	/* Making new thumbnails keeps the job waiting at the thumbnail step */
	if (running && !YouTubeWorkflowManager::Instance()->IsRegeneratingThumbnails()) {
		/* A new run (or the next stage) replaces whatever was being reviewed */
		if (!reviewGate.isEmpty()) {
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
		{"thumbnails", "6. Thumbnail", hasMetadata},
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

void YouTubePipelineDock::OnPremiereExported(const QString &xmlPath)
{
	OnLogMessageReceived("info", "In Premiere Pro: File → Import, then pick " + QFileInfo(xmlPath).fileName() +
					     ". It opens as a sequence; drag any cut's edge to widen or tighten it.");
	statusBanner->setText(QString("🎬 Exported for Premiere Pro: %1").arg(xmlPath));
	QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(xmlPath)});

	/* The newest installed Premiere Pro (a release before a beta) */
	QDir adobe("C:/Program Files/Adobe");
	QStringList dirs = adobe.entryList({"Adobe Premiere Pro*"}, QDir::Dirs, QDir::Name | QDir::Reversed);
	std::stable_partition(dirs.begin(), dirs.end(), [](const QString &d) { return !d.contains("Beta"); });
	for (const QString &d : dirs) {
		QString exe = adobe.filePath(d + "/Adobe Premiere Pro.exe");
		if (QFileInfo::exists(exe)) {
			openPremiereBtn->setProperty("exe", exe);
			openPremiereBtn->show();
			break;
		}
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

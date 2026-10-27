#include "YouTubePipelineDock.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

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
		{"llm", "4. AI Title & SEO Description (Priority 1-4)"},
		{"thumbnail", "5. Custom 1280x720 AI Thumbnail"},
		{"upload", "6. YouTube Upload (UNLISTED)"}
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
	metaLayout->addWidget(youtubeUrlLabel);

	mainLayout->addWidget(metaGroup);

	// 4. Console Log
	QGroupBox *logGroup = new QGroupBox("Workflow Console Log", container);
	QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
	consoleLog = new QPlainTextEdit(logGroup);
	consoleLog->setReadOnly(true);
	consoleLog->setMaximumHeight(110);
	consoleLog->setStyleSheet(
		"QPlainTextEdit { background-color: #140e1c; color: #d8cce8; font-family: Consolas, monospace; font-size: 11px; }");
	logLayout->addWidget(consoleLog);
	mainLayout->addWidget(logGroup);

	// 5. Action Buttons
	QHBoxLayout *btnLayout = new QHBoxLayout();
	openFolderBtn = new QPushButton("📂 Open Folder", container);
	rerunBtn = new QPushButton("🔄 Re-Run Last Video", container);
	cancelBtn = new QPushButton("⏹ Cancel", container);
	cancelBtn->setEnabled(false);

	btnLayout->addWidget(openFolderBtn);
	btnLayout->addWidget(rerunBtn);
	btnLayout->addWidget(cancelBtn);
	mainLayout->addLayout(btnLayout);

	mainLayout->addStretch();
	scrollArea->setWidget(container);
	outerLayout->addWidget(scrollArea);

	// Connect signals from YouTubeWorkflowManager
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(mgr, &YouTubeWorkflowManager::PipelineStateChanged, this, &YouTubePipelineDock::OnPipelineStateChanged);
	connect(mgr, &YouTubeWorkflowManager::StepProgressUpdated, this, &YouTubePipelineDock::OnStepProgressUpdated);
	connect(mgr, &YouTubeWorkflowManager::MetadataGenerated, this, &YouTubePipelineDock::OnMetadataGenerated);
	connect(mgr, &YouTubeWorkflowManager::ThumbnailGenerated, this, &YouTubePipelineDock::OnThumbnailGenerated);
	connect(mgr, &YouTubeWorkflowManager::PipelineCompleted, this, &YouTubePipelineDock::OnPipelineCompleted);
	connect(mgr, &YouTubeWorkflowManager::LogMessageReceived, this, &YouTubePipelineDock::OnLogMessageReceived);

	connect(openFolderBtn, &QPushButton::clicked, this, [this]() {
		QString target = !currentEditedVideoPath.isEmpty()
					 ? currentEditedVideoPath
					 : YouTubeWorkflowManager::Instance()->GetLastVideoPath();
		if (!target.isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(target).absolutePath()));
		}
	});

	connect(rerunBtn, &QPushButton::clicked, this, []() {
		QString lastVid = YouTubeWorkflowManager::Instance()->GetLastVideoPath();
		if (!lastVid.isEmpty() && QFileInfo::exists(lastVid)) {
			YouTubeWorkflowManager::Instance()->StartPipelineForVideo(lastVid);
		}
	});

	connect(cancelBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);
}

void YouTubePipelineDock::OnPipelineStateChanged(const QString &bannerText, bool running)
{
	statusBanner->setText(bannerText);
	cancelBtn->setEnabled(running);
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
	if (!videoUrl.isEmpty()) {
		youtubeUrlLabel->setText(
			QString("🔗 <b>Uploaded (Unlisted):</b> <a style='color:#ff6666;' href='%1'>%1</a>").arg(videoUrl));
	} else {
		youtubeUrlLabel->setText(QString("📁 <b>Local Output Ready:</b> %1").arg(message));
	}
}

void YouTubePipelineDock::OnLogMessageReceived(const QString &level, const QString &message)
{
	QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
	QString prefix = (level == "error") ? "[ERR]" : (level == "warning" ? "[WARN]" : "[INFO]");
	consoleLog->appendPlainText(QString("%1 %2 %3").arg(ts, prefix, message));
}

#include "YouTubeThumbnailPicker.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kColumns = 4;
const QSize kThumbSize(176, 99);

QJsonObject ReadJson(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

QString Clock(double seconds)
{
	int s = int(seconds + 0.5);
	return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
} // namespace

YouTubeThumbnailPicker::YouTubeThumbnailPicker(QWidget *parent) : QGroupBox("🖼 Pick a Thumbnail", parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	infoLabel = new QLabel(this);
	infoLabel->setWordWrap(true);
	infoLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	layout->addWidget(infoLabel);
	notesLabel = new QLabel(this);
	notesLabel->setWordWrap(true);
	notesLabel->setStyleSheet("color: #ffb070; font-size: 11px;");
	notesLabel->hide();
	layout->addWidget(notesLabel);

	grid = new QGridLayout();
	grid->setSpacing(6);
	layout->addLayout(grid);

	group = new QButtonGroup(this);
	group->setExclusive(true);

	/* Tweak the AI images */
	tweakBox = new QGroupBox("🎨 Tweak the AI images", this);
	QFormLayout *form = new QFormLayout(tweakBox);
	hookEdit = new QLineEdit(tweakBox);
	hookEdit->setPlaceholderText("Leave empty for no text");
	form->addRow("Text on thumbnail:", hookEdit);
	promptEdit = new QPlainTextEdit(tweakBox);
	promptEdit->setPlaceholderText("What the background should show");
	promptEdit->setFixedHeight(48);
	form->addRow("Background idea:", promptEdit);
	avoidEdit = new QLineEdit(tweakBox);
	avoidEdit->setPlaceholderText("e.g. hands, robots, neon");
	avoidEdit->setToolTip("Things the AI should leave out of the background");
	form->addRow("Leave out:", avoidEdit);
	styleCombo = new QComboBox(tweakBox);
	styleCombo->addItem("Mixed (4 different looks)", "mixed");
	styleCombo->addItem("Cinematic", "cinematic");
	styleCombo->addItem("Poster", "poster");
	styleCombo->addItem("Bright", "bright");
	styleCombo->addItem("Dark & moody", "moody");
	form->addRow("Style:", styleCombo);

	QHBoxLayout *personRow = new QHBoxLayout();
	personCombo = new QComboBox(tweakBox);
	personCombo->addItem("None", "none");
	personCombo->addItem("From the video (best frame)", "video");
	personCombo->addItem("My photo", "photo");
	personCombo->setToolTip("Cut you out (background removed, on this PC) and place you on top of the AI image");
	sideCombo = new QComboBox(tweakBox);
	sideCombo->addItem("Right", "right");
	sideCombo->addItem("Left", "left");
	attachButton = new QPushButton("📎 Attach photo", tweakBox);
	photoPreview = new QLabel(tweakBox);
	photoPreview->setFixedSize(40, 40);
	photoPreview->setScaledContents(true);
	personRow->addWidget(personCombo, 1);
	personRow->addWidget(new QLabel("Side:", tweakBox));
	personRow->addWidget(sideCombo);
	personRow->addWidget(attachButton);
	personRow->addWidget(photoPreview);
	form->addRow("You in the thumbnail:", personRow);

	QHBoxLayout *actions = new QHBoxLayout();
	generateButton = new QPushButton(tweakBox);
	generateButton->setToolTip("Make 4 new AI images with these settings (the video frames stay)");
	recomposeButton = new QPushButton("🔁 Apply text && photo", tweakBox);
	recomposeButton->setToolTip("Redraw every thumbnail with the text and photo above, without new AI images (a few seconds)");
	actions->addWidget(generateButton, 1);
	actions->addWidget(recomposeButton, 1);
	form->addRow(actions);

	QHBoxLayout *refineRow = new QHBoxLayout();
	refineButton = new QPushButton(tweakBox);
	refineButton->setToolTip("Make 2 variations of the selected thumbnail's image, steered by the background idea");
	strengthSlider = new QSlider(Qt::Horizontal, tweakBox);
	strengthSlider->setRange(30, 80);
	strengthSlider->setValue(50);
	strengthSlider->setToolTip("How much the refined image may change");
	refineRow->addWidget(refineButton, 1);
	refineRow->addWidget(new QLabel("Change: a little", tweakBox));
	refineRow->addWidget(strengthSlider, 1);
	refineRow->addWidget(new QLabel("a lot", tweakBox));
	form->addRow(refineRow);
	layout->addWidget(tweakBox);

	busyRow = new QWidget(this);
	QHBoxLayout *busyLayout = new QHBoxLayout(busyRow);
	busyLayout->setContentsMargins(0, 0, 0, 0);
	busyBar = new QProgressBar(busyRow);
	busyBar->setRange(0, 100);
	busyBar->setFixedHeight(16);
	busyLabel = new QLabel(busyRow);
	busyLabel->setStyleSheet("color: #ffcc44;");
	QPushButton *stopButton = new QPushButton("⏹ Stop", busyRow);
	busyLayout->addWidget(busyLabel, 1);
	busyLayout->addWidget(busyBar, 1);
	busyLayout->addWidget(stopButton);
	busyRow->hide();
	layout->addWidget(busyRow);

	useButton = new QPushButton("✅ Use this thumbnail", this);
	useButton->setStyleSheet("font-weight: bold;");
	layout->addWidget(useButton);

	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(useButton, &QPushButton::clicked, this, &YouTubeThumbnailPicker::Finish);
	connect(generateButton, &QPushButton::clicked, this, [this]() { Run("generate"); });
	connect(refineButton, &QPushButton::clicked, this, [this]() { Run("refine"); });
	connect(recomposeButton, &QPushButton::clicked, this, [this]() { Run("recompose"); });
	connect(attachButton, &QPushButton::clicked, this, &YouTubeThumbnailPicker::AttachPhoto);
	connect(personCombo, QOverload<int>::of(&QComboBox::activated), this, [this]() {
		if (personCombo->currentData().toString() == "photo" && photoPath.isEmpty()) {
			AttachPhoto();
		}
	});
	connect(strengthSlider, &QSlider::valueChanged, this, &YouTubeThumbnailPicker::UpdateEtaLabels);
	connect(stopButton, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::CancelPipeline);
	connect(mgr, &YouTubeWorkflowManager::ThumbnailRegenStarted, this, [this]() { SetBusy(true); });
	connect(mgr, &YouTubeWorkflowManager::ThumbnailRegenFinished, this, [this]() { SetBusy(false); });
	connect(mgr, &YouTubeWorkflowManager::StepProgressUpdated, this,
		[this](const QString &step, int percent, const QString &message) {
			if (step == "thumbnail" && busyRow->isVisible()) {
				busyBar->setValue(percent);
				busyLabel->setText(message);
			}
		});

	UpdateEtaLabels();
	hide();
}

void YouTubeThumbnailPicker::LoadCandidates(const QString &thumbsPath_, const QString &statePath_)
{
	thumbsPath = thumbsPath_;
	statePath = statePath_;

	for (QAbstractButton *b : group->buttons()) {
		group->removeButton(b);
		b->deleteLater();
	}

	QFile file(thumbsPath);
	if (!file.open(QIODevice::ReadOnly)) {
		emit YouTubeWorkflowManager::Instance()->LogMessageReceived(
			"error", QString("Could not read the thumbnail choices: %1").arg(thumbsPath));
		return;
	}
	QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	QString chosen = root.value("chosen").toString();
	infoLabel->setText(QString("AI images: %1. Click one to choose it, double-click to see it full size.")
				   .arg(root.value("provider").toString()));
	QStringList notes;
	for (const QJsonValue &v : root.value("notes").toArray()) {
		notes.append("⚠ " + v.toString());
	}
	notesLabel->setText(notes.join("\n"));
	notesLabel->setVisible(!notes.isEmpty());

	int index = 0;
	for (const QJsonValue &v : root.value("candidates").toArray()) {
		QJsonObject c = v.toObject();
		QString path = c.value("path").toString();

		QToolButton *button = new QToolButton(this);
		button->setCheckable(true);
		button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		button->setText(c.value("label").toString());
		button->setIcon(QIcon(QPixmap(path).scaled(kThumbSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		button->setIconSize(kThumbSize);
		button->setProperty("candidateId", c.value("id").toString());
		button->setProperty("path", path);
		button->setToolTip(path);
		button->installEventFilter(this);
		button->setChecked(c.value("id").toString() == chosen);
		group->addButton(button);
		grid->addWidget(button, index / kColumns, index % kColumns);
		index++;
	}
	useButton->setEnabled(index > 0);

	/* The tweaks this job was made with, else the AI's own text and idea plus your saved defaults */
	const YouTubeWorkflowConfig &cfg = YouTubeWorkflowManager::Instance()->GetConfig();
	QJsonObject ctx = ReadJson(statePath).value("ctx").toObject();
	QJsonObject meta = ctx.value("metadata").toObject();
	QJsonObject opts = ctx.value("thumb_options").toObject();
	hookEdit->setText(opts.contains("hook") ? opts.value("hook").toString()
						: meta.value("thumbnail_hook").toString());
	QString prompt = opts.value("prompt").toString();
	promptEdit->setPlainText(prompt.isEmpty() ? meta.value("thumbnail_prompt").toString() : prompt);
	avoidEdit->setText(opts.value("avoid").toString(cfg.thumbAvoid));
	styleCombo->setCurrentIndex(std::max(0, styleCombo->findData(opts.value("style").toString("mixed"))));
	personCombo->setCurrentIndex(
		std::max(0, personCombo->findData(opts.value("person").toString(cfg.thumbPersonMode))));
	sideCombo->setCurrentIndex(std::max(0, sideCombo->findData(opts.value("side").toString(cfg.thumbPersonSide))));
	SetPhoto(opts.value("photo_path").toString(cfg.thumbPhotoPath));
	strengthSlider->setValue(int(opts.value("strength").toDouble(0.5) * 100));

	UpdateEtaLabels();
	show();
}

bool YouTubeThumbnailPicker::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonDblClick) {
		QString path = watched->property("path").toString();
		if (!path.isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
			return true;
		}
	}
	return QGroupBox::eventFilter(watched, event);
}

QJsonObject YouTubeThumbnailPicker::Options() const
{
	QJsonObject o;
	o["hook"] = hookEdit->text().trimmed();
	o["prompt"] = promptEdit->toPlainText().trimmed();
	o["avoid"] = avoidEdit->text().trimmed();
	o["style"] = styleCombo->currentData().toString();
	o["person"] = personCombo->currentData().toString();
	o["photo_path"] = photoPath;
	o["side"] = sideCombo->currentData().toString();
	o["strength"] = strengthSlider->value() / 100.0;
	if (QAbstractButton *checked = group->checkedButton()) {
		o["refine_id"] = checked->property("candidateId").toString();
	}
	return o;
}

void YouTubeThumbnailPicker::Run(const QString &action)
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (action == "refine" && !group->checkedButton()) {
		emit mgr->LogMessageReceived("warning", "Click the thumbnail you want to refine first.");
		return;
	}
	if (personCombo->currentData().toString() == "photo" && !QFileInfo::exists(photoPath)) {
		AttachPhoto();
		if (!QFileInfo::exists(photoPath)) {
			return;
		}
	}
	mgr->RegenerateThumbnails(action, Options());
}

void YouTubeThumbnailPicker::AttachPhoto()
{
	QString start = photoPath.isEmpty() ? QDir::homePath() : QFileInfo(photoPath).absolutePath();
	QString path = QFileDialog::getOpenFileName(this, "Photo of you for the thumbnail", start,
						    "Images (*.png *.jpg *.jpeg *.webp)");
	if (path.isEmpty()) {
		if (photoPath.isEmpty()) {
			personCombo->setCurrentIndex(std::max(0, personCombo->findData("none")));
		}
		return;
	}
	SetPhoto(path);
	personCombo->setCurrentIndex(std::max(0, personCombo->findData("photo")));
}

void YouTubeThumbnailPicker::SetPhoto(const QString &path)
{
	photoPath = QFileInfo::exists(path) ? path : QString();
	QPixmap pix(photoPath);
	photoPreview->setPixmap(pix.isNull() ? QPixmap() : pix.scaled(photoPreview->size(), Qt::KeepAspectRatio,
								     Qt::SmoothTransformation));
	photoPreview->setToolTip(photoPath.isEmpty() ? "No photo attached" : photoPath);
	attachButton->setText(photoPath.isEmpty() ? "📎 Attach photo" : "📎 Change photo");
}

void YouTubeThumbnailPicker::SetBusy(bool busy)
{
	tweakBox->setEnabled(!busy);
	useButton->setEnabled(!busy && !group->buttons().isEmpty());
	busyRow->setVisible(busy);
	if (busy) {
		busyBar->setValue(0);
		busyLabel->setText("Starting...");
	}
}

void YouTubeThumbnailPicker::UpdateEtaLabels()
{
	/* How long Stable Diffusion took last time on this PC (written by the pipeline) */
	double load = 15, first = 27, image = 10;
	QJsonObject t = ReadJson(QDir::homePath() + "/OBSYouTubeImageGen/timings.json");
	load = t.value("load_s").toDouble(load);
	first = t.value("first_image_s").toDouble(first);
	image = t.value("image_s").toDouble(image);

	const YouTubeWorkflowConfig &cfg = YouTubeWorkflowManager::Instance()->GetConfig();
	bool sd = cfg.thumbnailImageSource == "sd" || cfg.thumbnailImageSource == "off";
	double strength = strengthSlider->value() / 100.0;
	generateButton->setText(sd ? QString("🎨 Make 4 new AI images (~%1)").arg(Clock(load + first + 3 * image))
				   : QString("🎨 Make 4 new AI images"));
	refineButton->setText(
		QString("✨ Refine selected (~%1)").arg(Clock(load + (first + image) * std::max(0.3, strength))));
}

void YouTubeThumbnailPicker::Finish()
{
	QAbstractButton *checked = group->checkedButton();
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (!checked) {
		emit mgr->LogMessageReceived("warning", "Pick a thumbnail first.");
		return;
	}

	QFile in(thumbsPath);
	if (!in.open(QIODevice::ReadOnly)) {
		emit mgr->LogMessageReceived("error", QString("Could not read %1").arg(thumbsPath));
		return;
	}
	QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
	in.close();
	root["chosen"] = checked->property("candidateId").toString();

	QSaveFile out(thumbsPath);
	if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(root).toJson()) < 0 || !out.commit()) {
		emit mgr->LogMessageReceived("error", QString("Could not save %1").arg(thumbsPath));
		return;
	}

	hide();
	mgr->ResumePipeline(statePath);
}

#include "YouTubeScriptWriter.hpp"
#include "YouTubeHeartbeat.hpp"
#include "YouTubeTeleprompter.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <util/config-file.h>

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kWordsPerMinute = 150; /* as SCRIPT_WPM in youtube_pipeline.py */

struct ModelInfo {
	const char *id;
	const char *name;
	double inPerM;
	double outPerM;
};
/* US$ per million tokens, as CLAUDE_PRICE in youtube_pipeline.py */
const ModelInfo kModels[] = {
	{"claude-opus-5-5", "Claude Opus 5.5", 4.0, 20.0},
	{"claude-sonnet-5-5", "Claude Sonnet 5.5", 2.0, 10.0},
	{"claude-fable-5-1", "Claude Fable 5.1", 10.0, 50.0},
};

struct EffortInfo {
	const char *id;
	const char *name;
	int thinkingTokens; /* a rough guess at the thinking a script takes at this effort */
};
const EffortInfo kEfforts[] = {
	{"low", "Low", 1000},           {"medium", "Medium", 3000}, {"high", "High", 6000},
	{"xhigh", "Extra high", 10000}, {"max", "Max", 16000},
};

QString ModelName(const QString &id)
{
	for (const ModelInfo &m : kModels) {
		if (id == m.id) {
			return m.name;
		}
	}
	return id;
}

int SpokenWords(const QString &script)
{
	static const QRegularExpression cue("^\\[show:.*\\]$", QRegularExpression::CaseInsensitiveOption);
	int words = 0;
	for (const QString &raw : script.split('\n')) {
		const QString line = raw.trimmed();
		if (line.isEmpty() || line.startsWith('#') || cue.match(line).hasMatch()) {
			continue;
		}
		words += line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
	}
	return words;
}

int ConfigInt(const char *name, int fallback)
{
	config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
	return cfg && config_has_user_value(cfg, "YouTubeWorkflow", name)
		       ? (int)config_get_int(cfg, "YouTubeWorkflow", name)
		       : fallback;
}
} // namespace

void YouTubeScriptWriter::FillModels(QComboBox *combo)
{
	for (const ModelInfo &m : kModels) {
		combo->addItem(QString("%1 ($%2 / $%3 per M tokens)").arg(m.name).arg(m.inPerM).arg(m.outPerM), m.id);
	}
}

void YouTubeScriptWriter::FillEfforts(QComboBox *combo)
{
	for (const EffortInfo &e : kEfforts) {
		combo->addItem(e.name, e.id);
	}
}

double YouTubeScriptWriter::EstimateCost(const QString &model, const QString &effort, double minutes)
{
	const ModelInfo *m = &kModels[0];
	for (const ModelInfo &info : kModels) {
		if (model == info.id) {
			m = &info;
		}
	}
	int thinking = kEfforts[2].thinkingTokens;
	for (const EffortInfo &e : kEfforts) {
		if (effort == e.id) {
			thinking = e.thinkingTokens;
		}
	}
	/* About 1.35 tokens per word of script, plus the thinking, and ~1,500 tokens of instructions in */
	const double out = minutes * kWordsPerMinute * 1.35 + thinking;
	return (1500 * m->inPerM + out * m->outPerM) / 1e6;
}

YouTubeScriptWriter::YouTubeScriptWriter(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("📝 Script Writer");
	setAttribute(Qt::WA_DeleteOnClose, false);
	resize(1100, 720);
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	const YouTubeWorkflowConfig &cfg = mgr->GetConfig();

	/* Left: what to write */
	QWidget *left = new QWidget(this);
	QVBoxLayout *leftLayout = new QVBoxLayout(left);
	leftLayout->setContentsMargins(0, 0, 6, 0);

	QFormLayout *form = new QFormLayout();
	topicEdit = new QLineEdit(left);
	topicEdit->setPlaceholderText("What's the video about?");
	form->addRow("Topic:", topicEdit);
	notesEdit = new QPlainTextEdit(left);
	notesEdit->setPlaceholderText("Optional: points to cover, your angle, a story, links you'll mention...");
	notesEdit->setMaximumHeight(110);
	form->addRow("Notes:", notesEdit);
	minutesSpin = new QSpinBox(left);
	minutesSpin->setRange(1, 30);
	minutesSpin->setSuffix(" min");
	minutesSpin->setValue(ConfigInt("ScriptMinutes", 8));
	form->addRow("Length:", minutesSpin);
	toneCombo = new QComboBox(left);
	toneCombo->addItem("Conversational", "conversational");
	toneCombo->addItem("Tutorial", "tutorial");
	toneCombo->addItem("Story", "story");
	toneCombo->addItem("Hype", "hype");
	form->addRow("Tone:", toneCombo);
	modelCombo = new QComboBox(left);
	FillModels(modelCombo);
	modelCombo->setCurrentIndex(std::max(0, modelCombo->findData(cfg.scriptModel)));
	modelCombo->setToolTip("Uses the Claude API key in Settings → YouTube → 2. The default is set in Settings → "
			       "YouTube → 1. Only your topic, notes and past video titles are sent.");
	form->addRow("Model:", modelCombo);
	effortCombo = new QComboBox(left);
	FillEfforts(effortCombo);
	effortCombo->setCurrentIndex(std::max(0, effortCombo->findData(cfg.scriptEffort)));
	effortCombo->setToolTip("How hard Claude thinks before writing. Higher is better on hard topics, slower and "
				"costs more.");
	form->addRow("Effort:", effortCombo);
	leftLayout->addLayout(form);

	ideasBtn = new QPushButton("💡 Suggest topics", left);
	ideasBtn->setToolTip("Claude suggests 5 topics from your notes and the titles of your past videos");
	leftLayout->addWidget(ideasBtn);
	ideasList = new QListWidget(left);
	ideasList->setWordWrap(true);
	ideasList->setToolTip("Click an idea to use it as the topic");
	ideasList->hide();
	leftLayout->addWidget(ideasList, 1);

	writeBtn = new QPushButton(left);
	writeBtn->setStyleSheet("font-weight: bold;");
	leftLayout->addWidget(writeBtn);
	writeHeart = new YouTubeHeartbeat(writeBtn);

	QHBoxLayout *reviseRow = new QHBoxLayout();
	feedbackEdit = new QLineEdit(left);
	feedbackEdit->setPlaceholderText("What to change, e.g. shorter intro, add a story about...");
	reviseBtn = new QPushButton("↻ Revise", left);
	reviseBtn->setToolTip("Claude rewrites the script with your changes; the earlier version is kept");
	reviseRow->addWidget(feedbackEdit, 1);
	reviseRow->addWidget(reviseBtn);
	leftLayout->addLayout(reviseRow);

	QGroupBox *savedBox = new QGroupBox("Saved scripts", left);
	QVBoxLayout *savedLayout = new QVBoxLayout(savedBox);
	savedList = new QListWidget(savedBox);
	savedList->setToolTip("Double-click to open");
	savedLayout->addWidget(savedList);
	QPushButton *folderBtn = new QPushButton("📂 Open folder", savedBox);
	savedLayout->addWidget(folderBtn);
	leftLayout->addWidget(savedBox, 1);

	/* Right: the script */
	QWidget *right = new QWidget(this);
	QVBoxLayout *rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(6, 0, 0, 0);
	scriptEdit = new QPlainTextEdit(right);
	scriptEdit->setPlaceholderText(
		"The script appears here as Claude writes it. You can edit it before reading it.");
	QFont scriptFont = scriptEdit->font();
	scriptFont.setPointSizeF(scriptFont.pointSizeF() * 1.15);
	scriptEdit->setFont(scriptFont);
	rightLayout->addWidget(scriptEdit, 1);
	statusLabel = new QLabel(right);
	statusLabel->setWordWrap(true);
	rightLayout->addWidget(statusLabel);
	QHBoxLayout *actions = new QHBoxLayout();
	stopBtn = new QPushButton("⏹ Stop", right);
	saveBtn = new QPushButton("💾 Save", right);
	promptBtn = new QPushButton("🖥 Teleprompter", right);
	promptBtn->setStyleSheet("font-weight: bold;");
	promptBtn->setToolTip("Save and open this script in the teleprompter");
	promptHeart = new YouTubeHeartbeat(promptBtn);
	actions->addWidget(stopBtn);
	actions->addStretch();
	actions->addWidget(saveBtn);
	actions->addWidget(promptBtn);
	rightLayout->addLayout(actions);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(left);
	splitter->addWidget(right);
	splitter->setStretchFactor(0, 2);
	splitter->setStretchFactor(1, 3);
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(splitter);

	connect(ideasBtn, &QPushButton::clicked, this, &YouTubeScriptWriter::SuggestTopics);
	connect(writeBtn, &QPushButton::clicked, this, [this]() { Write(false); });
	connect(reviseBtn, &QPushButton::clicked, this, [this]() { Write(true); });
	connect(feedbackEdit, &QLineEdit::returnPressed, this, [this]() { Write(true); });
	connect(stopBtn, &QPushButton::clicked, mgr, &YouTubeWorkflowManager::StopScript);
	connect(saveBtn, &QPushButton::clicked, this, &YouTubeScriptWriter::Save);
	connect(promptBtn, &QPushButton::clicked, this, &YouTubeScriptWriter::OpenTeleprompter);
	connect(folderBtn, &QPushButton::clicked, this, []() {
		QDir().mkpath(YouTubeWorkflowManager::ScriptsFolder());
		QDesktopServices::openUrl(QUrl::fromLocalFile(YouTubeWorkflowManager::ScriptsFolder()));
	});
	connect(savedList, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { OpenSaved(item->data(Qt::UserRole).toString()); });
	connect(ideasList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		topicEdit->setText(item->data(Qt::UserRole).toString());
		notesEdit->setPlainText(item->data(Qt::UserRole + 1).toString());
	});
	connect(topicEdit, &QLineEdit::textChanged, this, &YouTubeScriptWriter::UpdateButtons);
	connect(feedbackEdit, &QLineEdit::textChanged, this, &YouTubeScriptWriter::UpdateButtons);
	connect(scriptEdit, &QPlainTextEdit::textChanged, this, [this]() {
		if (!streaming) {
			UpdateButtons();
			UpdateStatus();
		}
	});
	for (QComboBox *combo : {modelCombo, effortCombo}) {
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			&YouTubeScriptWriter::UpdateButtons);
	}
	connect(minutesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int minutes) {
		if (config_t *c = App() ? App()->GetUserConfig() : nullptr) {
			config_set_int(c, "YouTubeWorkflow", "ScriptMinutes", minutes);
		}
		UpdateButtons();
	});

	connect(mgr, &YouTubeWorkflowManager::ScriptProgress, this, [this](int words, const QString &text) {
		streaming = true;
		scriptEdit->setPlainText(text);
		scriptEdit->verticalScrollBar()->setValue(scriptEdit->verticalScrollBar()->maximum());
		statusLabel->setText(QString("✍ %1 is writing... %2 words so far")
					     .arg(ModelName(modelCombo->currentData().toString()))
					     .arg(words));
	});
	connect(mgr, &YouTubeWorkflowManager::ScriptReady, this,
		[this](const QString &path, const QString &, const QString &text, const QString &cost) {
			streaming = false;
			currentPath = path;
			lastCost = cost;
			scriptEdit->setPlainText(text);
			scriptEdit->document()->setModified(false);
			feedbackEdit->clear();
			RefreshSaved();
			UpdateStatus(QString("Saved as %1").arg(QFileInfo(path).fileName()));
		});
	connect(mgr, &YouTubeWorkflowManager::ScriptIdeasReady, this, &YouTubeScriptWriter::ShowIdeas);
	connect(mgr, &YouTubeWorkflowManager::ScriptFailed, this, [this](const QString &message) {
		streaming = false;
		statusLabel->setText(QString("<span style='color:#ff8080;'>⚠ %1</span>").arg(message.toHtmlEscaped()));
	});
	connect(mgr, &YouTubeWorkflowManager::ScriptWriterFinished, this, [this]() {
		streaming = false;
		SetBusy(false);
	});

	RefreshSaved();
	SetBusy(mgr->IsWritingScript());
	UpdateStatus();
}

QJsonArray YouTubeScriptWriter::TitleDirs() const
{
	QJsonArray dirs;
	QString recent = YouTubeWorkflowManager::Instance()->RecentProjectDir();
	QString output = QString::fromUtf8(OBSBasic::Get()->GetCurrentOutputPath());
	for (const QString &dir : {recent, output}) {
		if (!dir.isEmpty() && QDir(dir).exists() && !dirs.contains(dir)) {
			dirs.append(dir);
		}
	}
	return dirs;
}

void YouTubeScriptWriter::SuggestTopics()
{
	QJsonObject options;
	options["action"] = "ideas";
	options["topic"] = topicEdit->text().trimmed();
	options["notes"] = notesEdit->toPlainText().trimmed();
	options["model"] = modelCombo->currentData().toString();
	options["effort"] = effortCombo->currentData().toString();
	options["title_dirs"] = TitleDirs();
	SetBusy(true, QString("💡 %1 is thinking of topics...").arg(ModelName(modelCombo->currentData().toString())));
	YouTubeWorkflowManager::Instance()->WriteScript(options);
}

void YouTubeScriptWriter::ShowIdeas(const QJsonArray &ideas, const QString &cost)
{
	ideasList->clear();
	for (const QJsonValue &v : ideas) {
		QJsonObject idea = v.toObject();
		QListWidgetItem *item = new QListWidgetItem(QString("%1\n%2").arg(idea.value("title").toString(),
										  idea.value("angle").toString()),
							    ideasList);
		item->setToolTip(idea.value("why").toString());
		item->setData(Qt::UserRole, idea.value("title").toString());
		item->setData(Qt::UserRole + 1, idea.value("angle").toString());
	}
	ideasList->setVisible(!ideas.isEmpty());
	statusLabel->setText(QString("💡 %1 ideas (%2). Click one to use it.").arg(ideas.size()).arg(cost));
}

void YouTubeScriptWriter::Write(bool revise)
{
	if (topicEdit->text().trimmed().isEmpty() && notesEdit->toPlainText().trimmed().isEmpty() && !revise) {
		topicEdit->setFocus();
		return;
	}
	if (revise && (scriptEdit->toPlainText().trimmed().isEmpty() || feedbackEdit->text().trimmed().isEmpty())) {
		feedbackEdit->setFocus();
		return;
	}
	if (!revise && !scriptEdit->toPlainText().trimmed().isEmpty() && scriptEdit->document()->isModified() &&
	    QMessageBox::question(this, "Write a new script",
				  "Write a new script? Your edits to this one haven't been saved.",
				  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
		return;
	}
	/* A revision starts from what's in the editor, edits included */
	if (revise && scriptEdit->document()->isModified()) {
		Save();
	}

	QJsonObject options;
	options["action"] = revise ? "revise" : "write";
	options["topic"] = topicEdit->text().trimmed();
	options["notes"] = notesEdit->toPlainText().trimmed();
	options["minutes"] = minutesSpin->value();
	options["tone"] = toneCombo->currentData().toString();
	options["model"] = modelCombo->currentData().toString();
	options["effort"] = effortCombo->currentData().toString();
	options["out_dir"] = YouTubeWorkflowManager::ScriptsFolder();
	if (revise) {
		options["previous"] = scriptEdit->toPlainText();
		options["previous_path"] = currentPath;
		options["feedback"] = feedbackEdit->text().trimmed();
	} else {
		currentPath.clear();
		scriptEdit->clear();
	}
	SetBusy(true, QString("✍ %1 is %2 the script (%3 effort)...")
			      .arg(ModelName(options["model"].toString()), revise ? "revising" : "planning",
				   effortCombo->currentText().toLower()));
	YouTubeWorkflowManager::Instance()->WriteScript(options);
}

bool YouTubeScriptWriter::Save()
{
	QString text = scriptEdit->toPlainText();
	if (text.trimmed().isEmpty()) {
		return false;
	}
	if (currentPath.isEmpty()) {
		static const QRegularExpression titleRe("^#\\s+(.+)$", QRegularExpression::MultilineOption);
		QString title = titleRe.match(text).captured(1).trimmed();
		if (title.isEmpty()) {
			title = topicEdit->text().trimmed().isEmpty() ? "Untitled script" : topicEdit->text().trimmed();
		}
		title.remove(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"));
		QDir dir(YouTubeWorkflowManager::ScriptsFolder());
		dir.mkpath(".");
		QString stem =
			QString("%1 %2").arg(QDate::currentDate().toString("yyyy-MM-dd"), title.left(80).trimmed());
		currentPath = dir.filePath(stem + ".md");
		for (int v = 2; QFileInfo::exists(currentPath); ++v) {
			currentPath = dir.filePath(QString("%1 v%2.md").arg(stem).arg(v));
		}
	}
	QSaveFile file(currentPath);
	if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
		QMessageBox::warning(this, "Save script", QString("Could not save %1").arg(currentPath));
		return false;
	}
	scriptEdit->document()->setModified(false);
	RefreshSaved();
	UpdateStatus(QString("Saved as %1").arg(QFileInfo(currentPath).fileName()));
	return true;
}

void YouTubeScriptWriter::OpenTeleprompter()
{
	if (scriptEdit->document()->isModified() || currentPath.isEmpty()) {
		Save();
	}
	YouTubeTeleprompter *prompter = YouTubeTeleprompter::Get();
	prompter->LoadText(scriptEdit->toPlainText(), currentPath);
	prompter->show();
	prompter->raise();
	prompter->activateWindow();
	UpdateButtons();
}

void YouTubeScriptWriter::OpenSaved(const QString &path)
{
	if (scriptEdit->document()->isModified() &&
	    QMessageBox::question(this, "Open script",
				  "Open another script? Your edits to this one haven't been saved.",
				  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
		return;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	currentPath = path;
	lastCost.clear();
	scriptEdit->setPlainText(QString::fromUtf8(file.readAll()));
	scriptEdit->document()->setModified(false);
	UpdateStatus(QFileInfo(path).fileName());
}

void YouTubeScriptWriter::RefreshSaved()
{
	savedList->clear();
	QDir dir(YouTubeWorkflowManager::ScriptsFolder());
	for (const QFileInfo &f : dir.entryInfoList({"*.md"}, QDir::Files, QDir::Time)) {
		QListWidgetItem *item = new QListWidgetItem(f.completeBaseName(), savedList);
		item->setData(Qt::UserRole, f.absoluteFilePath());
		item->setToolTip(QString("%1\nChanged %2")
					 .arg(QDir::toNativeSeparators(f.absoluteFilePath()),
					      f.lastModified().toString("MMM d, h:mm AP")));
	}
}

void YouTubeScriptWriter::SetBusy(bool busy_, const QString &what)
{
	busy = busy_;
	stopBtn->setVisible(busy);
	for (QWidget *w : {(QWidget *)ideasBtn, (QWidget *)reviseBtn, (QWidget *)savedList}) {
		w->setEnabled(!busy);
	}
	scriptEdit->setReadOnly(busy);
	if (busy && !what.isEmpty()) {
		statusLabel->setText(what);
	}
	UpdateButtons();
}

void YouTubeScriptWriter::UpdateButtons()
{
	const QString model = modelCombo->currentData().toString();
	const QString effort = effortCombo->currentData().toString();
	const double cost = EstimateCost(model, effort, minutesSpin->value());
	writeBtn->setText(QString("✍ Write script (%1 · %2 · ~$%3)")
				  .arg(ModelName(model).remove("Claude "), effortCombo->currentText())
				  .arg(cost, 0, 'f', 2));
	const bool hasScript = !scriptEdit->toPlainText().trimmed().isEmpty();
	const bool hasTopic = !topicEdit->text().trimmed().isEmpty() || !notesEdit->toPlainText().trimmed().isEmpty();
	writeBtn->setEnabled(!busy);
	reviseBtn->setEnabled(!busy && hasScript && !feedbackEdit->text().trimmed().isEmpty());
	saveBtn->setEnabled(!busy && hasScript);
	promptBtn->setEnabled(!busy && hasScript);
	/* The heartbeat is on the next thing to click: Write once there's a topic, then the teleprompter */
	writeHeart->SetWanted(hasTopic && !hasScript);
	promptHeart->SetWanted(hasScript && !YouTubeTeleprompter::Get()->isVisible());
}

void YouTubeScriptWriter::UpdateStatus(const QString &extra)
{
	const int words = SpokenWords(scriptEdit->toPlainText());
	if (words == 0) {
		statusLabel->setText(extra);
		return;
	}
	QStringList parts{
		QString("%1 words · about %2 min to read").arg(words).arg(words / (double)kWordsPerMinute, 0, 'f', 1)};
	if (!lastCost.isEmpty()) {
		parts << QString("cost %1").arg(lastCost);
	}
	if (!extra.isEmpty()) {
		parts << extra;
	}
	statusLabel->setText(parts.join("  ·  "));
}

void YouTubeScriptWriter::closeEvent(QCloseEvent *event)
{
	if (scriptEdit->document()->isModified() && !scriptEdit->toPlainText().trimmed().isEmpty()) {
		QMessageBox::StandardButton answer = QMessageBox::question(
			this, "Script Writer", "Save your edits to this script?",
			QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
		if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !Save())) {
			event->ignore();
			return;
		}
	}
	event->accept();
}

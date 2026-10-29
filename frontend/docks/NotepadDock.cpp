#include "NotepadDock.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontDialog>
#include <QLabel>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextCursor>
#include <QVBoxLayout>

/* Fixed to the local machine: generated text never goes to a cloud service */
#define OLLAMA_URL "http://127.0.0.1:11434"
#define DEFAULT_OLLAMA_MODEL "gemma4:12b"

NotepadDock::NotepadDock(QWidget *parent) : QWidget(parent), network(new QNetworkAccessManager(this))
{
	config_t *config = App()->GetUserConfig();
	config_set_default_bool(config, "Notepad", "WordWrap", true);

	/* Plain buttons rather than a QToolBar: the themes limit dock toolbar
	 * buttons to icon width, which cuts text labels down to "..." */
	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->setContentsMargins(4, 4, 4, 4);
	buttons->setSpacing(6);

	auto makeButton = [&](const char *text, const QKeySequence &key, auto slot) {
		QPushButton *button = new QPushButton(QTStr(text), this);
		button->setFocusPolicy(Qt::NoFocus);
		connect(button, &QPushButton::clicked, this, slot);
		buttons->addWidget(button);

		if (!key.isEmpty()) {
			button->setToolTip(key.toString(QKeySequence::NativeText));

			QAction *action = new QAction(this);
			action->setShortcut(key);
			action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
			connect(action, &QAction::triggered, this, slot);
			addAction(action);
		}
		return button;
	};

	makeButton("Notepad.New", QKeySequence::New, &NotepadDock::New);
	makeButton("Notepad.Open", QKeySequence::Open, &NotepadDock::Open);
	makeButton("Notepad.Save", QKeySequence::Save, [this]() { Save(); });
	makeButton("Notepad.SaveAs", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this]() { SaveAs(); });
	makeButton("Notepad.Font", QKeySequence(), &NotepadDock::ChooseFont);

	wrapButton = new QPushButton(QTStr("Notepad.WordWrap"), this);
	wrapButton->setCheckable(true);
	wrapButton->setFocusPolicy(Qt::NoFocus);
	connect(wrapButton, &QPushButton::toggled, this, &NotepadDock::SetWordWrap);
	buttons->addWidget(wrapButton);

	generateButton = new QPushButton(QTStr("Notepad.Generate"), this);
	generateButton->setFocusPolicy(Qt::NoFocus);
	connect(generateButton, &QPushButton::clicked, this, &NotepadDock::ToggleGenerate);
	buttons->addWidget(generateButton);
	buttons->addStretch();

	edit = new QPlainTextEdit(this);
	status = new QLabel(this);
	status->setContentsMargins(4, 0, 4, 2);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addLayout(buttons);
	layout->addWidget(edit);
	layout->addWidget(status);

	QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	const char *family = config_get_string(config, "Notepad", "FontFamily");
	if (family && *family) {
		font.setFamily(QT_UTF8(family));
	}
	int size = (int)config_get_int(config, "Notepad", "FontSize");
	if (size > 0) {
		font.setPointSize(size);
	}
	edit->setFont(font);

	bool wrap = config_get_bool(config, "Notepad", "WordWrap");
	wrapButton->setChecked(wrap);
	SetWordWrap(wrap);

	autoSaveTimer.setSingleShot(true);
	autoSaveTimer.setInterval(1000);
	connect(&autoSaveTimer, &QTimer::timeout, this, &NotepadDock::SaveScratch);

	connect(edit, &QPlainTextEdit::textChanged, this, [this]() {
		if (loading) {
			return;
		}
		if (currentPath.isEmpty()) {
			autoSaveTimer.start();
		}
	});
	connect(edit->document(), &QTextDocument::modificationChanged, this, &NotepadDock::UpdateStatus);

	const char *lastFile = config_get_string(config, "Notepad", "LastFile");
	if (!(lastFile && *lastFile && QFileInfo::exists(QT_UTF8(lastFile)) && LoadFile(QT_UTF8(lastFile)))) {
		QFile scratch(ScratchPath());
		QString text;
		if (scratch.open(QIODevice::ReadOnly)) {
			text = QString::fromUtf8(scratch.readAll());
		}
		SetDocument(QString(), text);
	}
}

NotepadDock::~NotepadDock()
{
	generating = false;
	if (reply) {
		reply->abort();
	}
	SaveScratch();
}

QString NotepadDock::ScratchPath()
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/notepad.txt") <= 0) {
		return QString();
	}
	return QT_UTF8(path);
}

void NotepadDock::New()
{
	if (!MaybeSave()) {
		return;
	}
	SetDocument(QString(), QString());
	SaveScratch();
}

void NotepadDock::Open()
{
	if (!MaybeSave()) {
		return;
	}

	QString path = QFileDialog::getOpenFileName(this, QTStr("Notepad.Open"), currentPath.isEmpty() ? QString() : QFileInfo(currentPath).absolutePath(),
						    QTStr("Notepad.FileFilter"));
	if (!path.isEmpty()) {
		LoadFile(path);
	}
}

bool NotepadDock::Save()
{
	if (currentPath.isEmpty()) {
		return SaveAs();
	}
	if (!WriteFile(currentPath)) {
		return false;
	}
	edit->document()->setModified(false);
	return true;
}

bool NotepadDock::SaveAs()
{
	QString path = QFileDialog::getSaveFileName(this, QTStr("Notepad.SaveAs"), currentPath,
						    QTStr("Notepad.FileFilter"));
	if (path.isEmpty() || !WriteFile(path)) {
		return false;
	}

	/* The untitled note now lives in a real file, so start the next
	 * untitled note empty */
	if (currentPath.isEmpty()) {
		QFile::remove(ScratchPath());
	}

	currentPath = path;
	config_set_string(App()->GetUserConfig(), "Notepad", "LastFile", QT_TO_UTF8(path));
	edit->document()->setModified(false);
	UpdateStatus();
	return true;
}

void NotepadDock::ChooseFont()
{
	bool ok = false;
	QFont font = QFontDialog::getFont(&ok, edit->font(), this, QTStr("Notepad.Font"));
	if (!ok) {
		return;
	}

	edit->setFont(font);
	config_t *config = App()->GetUserConfig();
	config_set_string(config, "Notepad", "FontFamily", QT_TO_UTF8(font.family()));
	config_set_int(config, "Notepad", "FontSize", font.pointSize());
}

void NotepadDock::SetWordWrap(bool wrap)
{
	edit->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
	config_set_bool(App()->GetUserConfig(), "Notepad", "WordWrap", wrap);
}

bool NotepadDock::HasUnsavedChanges() const
{
	/* An untitled note is only in the scratch file, so any text in it
	 * would be lost by New or Open */
	if (currentPath.isEmpty()) {
		return !edit->document()->isEmpty();
	}
	return edit->document()->isModified();
}

bool NotepadDock::MaybeSave()
{
	if (!HasUnsavedChanges()) {
		return true;
	}

	QMessageBox::StandardButton button =
		OBSMessageBox::question(this, QTStr("Notepad.Unsaved.Title"), QTStr("Notepad.Unsaved.Text"),
					QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
					QMessageBox::Save);

	if (button == QMessageBox::Save) {
		return Save();
	}
	return button == QMessageBox::Discard;
}

bool NotepadDock::LoadFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		OBSMessageBox::warning(this, QTStr("Notepad.Title"), QTStr("Notepad.Error.Open").arg(path));
		return false;
	}

	SetDocument(path, QString::fromUtf8(file.readAll()));
	config_set_string(App()->GetUserConfig(), "Notepad", "LastFile", QT_TO_UTF8(path));
	return true;
}

bool NotepadDock::WriteFile(const QString &path)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(edit->toPlainText().toUtf8()) < 0 || !file.commit()) {
		OBSMessageBox::warning(this, QTStr("Notepad.Title"), QTStr("Notepad.Error.Save").arg(path));
		return false;
	}
	return true;
}

void NotepadDock::SetDocument(const QString &path, const QString &text)
{
	autoSaveTimer.stop();
	currentPath = path;
	if (path.isEmpty()) {
		config_set_string(App()->GetUserConfig(), "Notepad", "LastFile", "");
	}

	loading = true;
	edit->setPlainText(text);
	loading = false;

	edit->document()->setModified(false);
	UpdateStatus();
}

void NotepadDock::SaveScratch()
{
	autoSaveTimer.stop();
	if (!currentPath.isEmpty()) {
		return;
	}

	QString path = ScratchPath();
	if (path.isEmpty()) {
		return;
	}

	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(edit->toPlainText().toUtf8());
		file.commit();
	}
}

void NotepadDock::UpdateStatus()
{
	if (generating) {
		status->setText(generateStatus);
		status->setToolTip(QString());
		return;
	}

	if (currentPath.isEmpty()) {
		status->setText(QTStr("Notepad.Untitled"));
		status->setToolTip(QString());
		return;
	}

	QString name = QFileInfo(currentPath).fileName();
	if (edit->document()->isModified()) {
		name += " *";
	}
	status->setText(name);
	status->setToolTip(currentPath);
}

void NotepadDock::ToggleGenerate()
{
	if (generating) {
		FinishGenerate(QString());
	} else {
		StartGenerate();
	}
}

void NotepadDock::StartGenerate()
{
	const char *model = config_get_string(App()->GetUserConfig(), "YouTubeWorkflow", "OllamaModel");
	generateModel = (model && *model) ? QT_UTF8(model) : QStringLiteral(DEFAULT_OLLAMA_MODEL);

	/* Ollama runs "-cloud" models on its own servers, not this PC */
	if (generateModel.endsWith("-cloud") || generateModel.endsWith(":cloud")) {
		OBSMessageBox::warning(this, QTStr("Notepad.Generate"),
				       QTStr("Notepad.Generate.CloudModel").arg(generateModel));
		return;
	}

	if (!MaybeSave()) {
		return;
	}

	bool ok = false;
	QString topic = QInputDialog::getText(this, QTStr("Notepad.Generate.TopicTitle"),
					      QTStr("Notepad.Generate.TopicText"), QLineEdit::Normal, QString(), &ok)
				.trimmed();
	if (!ok) {
		return;
	}

	if (topic.isEmpty()) {
		topic = "a specific, interesting topic of your choice from anywhere in the world: history, "
			"science, places, food, technology or culture. Pick something different each time";
	}
	generatePrompt = "Write a spoken YouTube video script of about 300 to 500 words about: " + topic +
			 ".\n\nWrite only the words the host says to the camera: plain text paragraphs, no "
			 "title, no headings, no markdown, no stage directions or [brackets].";

	SetDocument(QString(), QString());

	generateId++;
	generating = true;
	ollamaStarted = false;
	pendingLine.clear();
	generateError.clear();
	generateStatus = QTStr("Notepad.Generate.Working").arg(generateModel);
	edit->setReadOnly(true);
	generateButton->setText(QTStr("Notepad.Generate.Stop"));
	UpdateStatus();

	CheckOllama(30);
}

void NotepadDock::CheckOllama(int triesLeft)
{
	QNetworkRequest request(QUrl(OLLAMA_URL "/api/tags"));
	request.setTransferTimeout(2000);
	reply = network->get(request);

	int id = generateId;
	QNetworkReply *r = reply;
	connect(r, &QNetworkReply::finished, this, [this, r, id, triesLeft]() {
		r->deleteLater();
		if (!generating || id != generateId) {
			return;
		}
		if (r->error() == QNetworkReply::NoError) {
			SendGenerate();
			return;
		}

		/* Like the YouTube pipeline: the Ollama tray app can stay open
		 * after its server has died, so start the server ourselves */
		if (!ollamaStarted) {
			ollamaStarted = true;
			if (!StartOllama()) {
				FinishGenerate(QTStr("Notepad.Generate.NoOllama"));
				return;
			}
			generateStatus = QTStr("Notepad.Generate.Starting");
			UpdateStatus();
		}

		if (triesLeft <= 0) {
			FinishGenerate(QTStr("Notepad.Generate.NoOllama"));
			return;
		}
		QTimer::singleShot(1000, this, [this, id, triesLeft]() {
			if (generating && id == generateId) {
				CheckOllama(triesLeft - 1);
			}
		});
	});
}

bool NotepadDock::StartOllama()
{
	QString exe = QStandardPaths::findExecutable("ollama");
	if (exe.isEmpty()) {
		exe = qEnvironmentVariable("LOCALAPPDATA") + "/Programs/Ollama/ollama.exe";
	}
	if (!QFileInfo::exists(exe)) {
		return false;
	}

	QProcess process;
	process.setProgram(exe);
	process.setArguments({"serve"});
	process.setStandardInputFile(QProcess::nullDevice());
	process.setStandardOutputFile(QProcess::nullDevice());
	process.setStandardErrorFile(QProcess::nullDevice());
#ifdef _WIN32
	process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
		args->flags |= 0x08000000; /* CREATE_NO_WINDOW */
	});
#endif
	return process.startDetached();
}

void NotepadDock::SendGenerate()
{
	generateStatus = QTStr("Notepad.Generate.Working").arg(generateModel);
	UpdateStatus();

	QJsonObject options;
	options["temperature"] = 0.9;

	QJsonObject body;
	body["model"] = generateModel;
	body["prompt"] = generatePrompt;
	body["stream"] = true;
	/* Keeps the reasoning of thinking models (qwen3) out of the note */
	body["think"] = false;
	body["options"] = options;

	QNetworkRequest request(QUrl(OLLAMA_URL "/api/generate"));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

	int id = generateId;
	QNetworkReply *r = reply;
	connect(r, &QNetworkReply::readyRead, this, [this, id]() {
		if (generating && id == generateId) {
			ReadGenerated(false);
		}
	});
	connect(r, &QNetworkReply::finished, this, [this, r, id]() {
		r->deleteLater();
		if (!generating || id != generateId) {
			return;
		}
		ReadGenerated(true);

		QString error = generateError;
		if (error.isEmpty() && r->error() != QNetworkReply::NoError) {
			error = r->errorString();
		}
		FinishGenerate(error.isEmpty() ? QString() : QTStr("Notepad.Generate.Error").arg(error));
	});
}

void NotepadDock::ReadGenerated(bool flush)
{
	if (reply) {
		pendingLine += reply->readAll();
	}

	/* Ollama streams one JSON object per line */
	QList<QByteArray> lines;
	qsizetype newline;
	while ((newline = pendingLine.indexOf('\n')) >= 0) {
		lines.append(pendingLine.left(newline));
		pendingLine.remove(0, newline + 1);
	}
	if (flush && !pendingLine.trimmed().isEmpty()) {
		lines.append(pendingLine);
		pendingLine.clear();
	}

	QTextCursor cursor(edit->document());
	cursor.movePosition(QTextCursor::End);
	for (const QByteArray &line : lines) {
		QJsonObject obj = QJsonDocument::fromJson(line).object();
		if (obj.contains("error")) {
			generateError = obj["error"].toString();
			continue;
		}
		QString text = obj["response"].toString();
		if (!text.isEmpty()) {
			cursor.insertText(text);
		}
	}
}

void NotepadDock::FinishGenerate(const QString &error)
{
	generating = false;
	generateId++;
	if (reply) {
		reply->abort();
	}

	edit->setReadOnly(false);
	generateButton->setText(QTStr("Notepad.Generate"));
	UpdateStatus();
	SaveScratch();

	if (!error.isEmpty()) {
		OBSMessageBox::warning(this, QTStr("Notepad.Generate"), error);
	}
}

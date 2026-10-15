#include "OBSSettingsBackup.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <util/config-file.h>

using namespace std;
namespace fs = std::filesystem;
using namespace SettingsBackup;

static QString ToQString(const fs::path &path)
{
	return QDir::toNativeSeparators(QString::fromStdString(path.u8string()));
}

static fs::path ToPath(const QString &path)
{
	return fs::u8path(QDir::fromNativeSeparators(path).toStdString());
}

struct CategoryBoxes {
	QCheckBox *profiles;
	QCheckBox *collections;
	QCheckBox *appSettings;
	QCheckBox *themes;
	QCheckBox *pluginSettings;
};

static QGroupBox *CreateCategoryGroup(QWidget *parent, CategoryBoxes &boxes)
{
	QGroupBox *group = new QGroupBox(QTStr("Backup.Include"), parent);
	boxes.profiles = new QCheckBox(QTStr("Backup.Category.Profiles"), group);
	boxes.collections = new QCheckBox(QTStr("Backup.Category.SceneCollections"), group);
	boxes.appSettings = new QCheckBox(QTStr("Backup.Category.AppSettings"), group);
	boxes.themes = new QCheckBox(QTStr("Backup.Category.Themes"), group);
	boxes.pluginSettings = new QCheckBox(QTStr("Backup.Category.PluginSettings"), group);

	QVBoxLayout *layout = new QVBoxLayout(group);
	for (QCheckBox *box :
	     {boxes.profiles, boxes.collections, boxes.appSettings, boxes.themes, boxes.pluginSettings}) {
		box->setChecked(true);
		layout->addWidget(box);
	}

	return group;
}

static uint32_t CheckedCategories(const CategoryBoxes &boxes)
{
	uint32_t categories = 0;
	if (boxes.profiles->isEnabled() && boxes.profiles->isChecked()) {
		categories |= Profiles;
	}
	if (boxes.collections->isEnabled() && boxes.collections->isChecked()) {
		categories |= SceneCollections;
	}
	if (boxes.appSettings->isEnabled() && boxes.appSettings->isChecked()) {
		categories |= AppSettings;
	}
	if (boxes.themes->isEnabled() && boxes.themes->isChecked()) {
		categories |= Themes;
	}
	if (boxes.pluginSettings->isEnabled() && boxes.pluginSettings->isChecked()) {
		categories |= PluginSettings;
	}
	return categories;
}

/* ------------------------------------------------------------------------- */

OBSBackupSettingsDialog::OBSBackupSettingsDialog(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setWindowTitle(QTStr("Backup.Title"));
	setMinimumWidth(520);

	QLabel *intro = new QLabel(QTStr("Backup.Intro"), this);
	intro->setWordWrap(true);

	CategoryBoxes boxes;
	QGroupBox *categories = CreateCategoryGroup(this, boxes);
	profiles = boxes.profiles;
	collections = boxes.collections;
	appSettings = boxes.appSettings;
	themes = boxes.themes;
	pluginSettings = boxes.pluginSettings;

	includeSecrets = new QCheckBox(QTStr("Backup.IncludeSecrets"), this);
	includeSecrets->setChecked(true);
	secretsWarning = new QLabel(QTStr("Backup.IncludeSecrets.Warning"), this);
	secretsWarning->setWordWrap(true);
	secretsWarning->setIndent(24);

	progress = new QProgressBar(this);
	progress->setVisible(false);

	/* Automatic backups */
	config_t *config = App()->GetUserConfig();
	QGroupBox *autoGroup = new QGroupBox(QTStr("Backup.Auto"), this);
	autoBackup = new QCheckBox(QTStr("Backup.Auto.Enable"), autoGroup);
	autoBackup->setChecked(config_get_bool(config, "Backup", "AutoBackupOnExit"));

	autoKeep = new QSpinBox(autoGroup);
	autoKeep->setRange(1, 100);
	autoKeep->setValue(int(config_get_int(config, "Backup", "AutoBackupKeep")));

	const char *folder = config_get_string(config, "Backup", "AutoBackupFolder");
	autoFolder = new QLineEdit((folder && *folder) ? QString::fromUtf8(folder) : ToQString(DefaultBackupFolder()),
				   autoGroup);
	autoFolder->setReadOnly(true);
	autoBrowse = new QPushButton(QTStr("Browse"), autoGroup);

	QHBoxLayout *keepRow = new QHBoxLayout();
	keepRow->addWidget(new QLabel(QTStr("Backup.Auto.Keep"), autoGroup));
	keepRow->addWidget(autoKeep);
	keepRow->addStretch();

	QHBoxLayout *folderRow = new QHBoxLayout();
	folderRow->addWidget(new QLabel(QTStr("Backup.Auto.Folder"), autoGroup));
	folderRow->addWidget(autoFolder, 1);
	folderRow->addWidget(autoBrowse);

	QVBoxLayout *autoLayout = new QVBoxLayout(autoGroup);
	autoLayout->addWidget(autoBackup);
	autoLayout->addLayout(keepRow);
	autoLayout->addLayout(folderRow);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	backupButton = buttons->addButton(QTStr("Backup.BackUpNow"), QDialogButtonBox::ActionRole);
	backupButton->setDefault(true);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(intro);
	layout->addWidget(categories);
	layout->addWidget(includeSecrets);
	layout->addWidget(secretsWarning);
	layout->addWidget(progress);
	layout->addWidget(autoGroup);
	layout->addStretch();
	layout->addWidget(buttons);

	for (QCheckBox *box : {profiles, collections, appSettings, themes, pluginSettings, includeSecrets}) {
		connect(box, &QCheckBox::toggled, this, &OBSBackupSettingsDialog::UpdateControls);
	}

	connect(autoBackup, &QCheckBox::toggled, this, [this]() {
		UpdateControls();
		SaveAutoBackupSettings();
	});
	connect(autoKeep, &QSpinBox::valueChanged, this, &OBSBackupSettingsDialog::SaveAutoBackupSettings);
	connect(autoBrowse, &QPushButton::clicked, this, [this]() {
		QString dir = SelectDirectory(this, QTStr("Backup.Auto.Folder"), autoFolder->text());
		if (!dir.isEmpty()) {
			autoFolder->setText(QDir::toNativeSeparators(dir));
			SaveAutoBackupSettings();
		}
	});
	connect(backupButton, &QPushButton::clicked, this, &OBSBackupSettingsDialog::BackUp);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	UpdateControls();
}

uint32_t OBSBackupSettingsDialog::SelectedCategories() const
{
	return CheckedCategories({profiles, collections, appSettings, themes, pluginSettings});
}

void OBSBackupSettingsDialog::UpdateControls()
{
	secretsWarning->setVisible(includeSecrets->isChecked());
	backupButton->setEnabled(SelectedCategories() != 0);
	autoKeep->setEnabled(autoBackup->isChecked());
	autoFolder->setEnabled(autoBackup->isChecked());
	autoBrowse->setEnabled(autoBackup->isChecked());
}

void OBSBackupSettingsDialog::SaveAutoBackupSettings()
{
	config_t *config = App()->GetUserConfig();
	config_set_bool(config, "Backup", "AutoBackupOnExit", autoBackup->isChecked());
	config_set_int(config, "Backup", "AutoBackupKeep", autoKeep->value());
	config_set_string(config, "Backup", "AutoBackupFolder",
			  QDir::fromNativeSeparators(autoFolder->text()).toUtf8().constData());
	config_save_safe(config, "tmp", nullptr);
}

void OBSBackupSettingsDialog::BackUp()
{
	error_code ec;
	fs::path folder = DefaultBackupFolder();
	fs::create_directories(folder, ec);

	QString path = SaveFile(this, QTStr("Backup.SaveAs"), ToQString(folder / DefaultBackupFileName()),
				QTStr("Backup.FileFilter") + " (*.zip)");
	if (path.isEmpty()) {
		return;
	}
	if (!path.endsWith(".zip", Qt::CaseInsensitive)) {
		path += ".zip";
	}

	main->SaveForBackup();

	backupButton->setEnabled(false);
	progress->setVisible(true);
	progress->setValue(0);
	QApplication::setOverrideCursor(Qt::WaitCursor);

	BackupResult result = CreateBackup(ToPath(path), {SelectedCategories(), includeSecrets->isChecked()},
					   [this](size_t done, size_t total) {
						   progress->setMaximum(int(max<size_t>(total, 1)));
						   progress->setValue(int(done));
						   QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
					   });

	QApplication::restoreOverrideCursor();
	progress->setVisible(false);
	backupButton->setEnabled(true);

	if (!result.success) {
		OBSMessageBox::critical(this, QTStr("Backup.Title"),
					QTStr("Backup.Failed").arg(QString::fromStdString(result.error)));
		return;
	}

	QMessageBox box(QMessageBox::Information, QTStr("Backup.Title"),
			QTStr("Backup.Done").arg(result.files).arg(QDir::toNativeSeparators(path)), QMessageBox::Ok,
			this);
	if (!result.skipped.empty()) {
		QStringList skipped;
		for (const string &item : result.skipped) {
			skipped << QString::fromStdString(item);
		}
		box.setInformativeText(QTStr("Backup.Skipped").arg(result.skipped.size()));
		box.setDetailedText(skipped.join("\n"));
	}
	QPushButton *show = box.addButton(QTStr("Backup.ShowInFolder"), QMessageBox::ActionRole);
	box.exec();

	if (box.clickedButton() == show) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
	}
}

/* ------------------------------------------------------------------------- */

OBSRestoreSettingsDialog::OBSRestoreSettingsDialog(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setWindowTitle(QTStr("Restore.Title"));
	setMinimumWidth(560);

	QLabel *intro = new QLabel(QTStr("Restore.Intro"), this);
	intro->setWordWrap(true);

	pending = new QLabel(QTStr("Restore.Pending"), this);
	pending->setWordWrap(true);
	cancelPending = new QPushButton(QTStr("Restore.CancelPending"), this);

	QHBoxLayout *pendingRow = new QHBoxLayout();
	pendingRow->addWidget(pending, 1);
	pendingRow->addWidget(cancelPending);

	file = new QLineEdit(this);
	file->setReadOnly(true);
	file->setPlaceholderText(QTStr("Restore.NoFile"));
	QPushButton *browse = new QPushButton(QTStr("Browse"), this);

	QHBoxLayout *fileRow = new QHBoxLayout();
	fileRow->addWidget(file, 1);
	fileRow->addWidget(browse);

	summary = new QLabel(this);
	summary->setWordWrap(true);
	summary->setTextFormat(Qt::RichText);
	summary->setVisible(false);

	CategoryBoxes boxes;
	QGroupBox *categories = CreateCategoryGroup(this, boxes);
	categories->setTitle(QTStr("Restore.Include"));
	profiles = boxes.profiles;
	collections = boxes.collections;
	appSettings = boxes.appSettings;
	themes = boxes.themes;
	pluginSettings = boxes.pluginSettings;

	QGroupBox *modeGroup = new QGroupBox(QTStr("Restore.Mode"), this);
	replaceMode = new QRadioButton(QTStr("Restore.Mode.Replace"), modeGroup);
	mergeMode = new QRadioButton(QTStr("Restore.Mode.Merge"), modeGroup);
	QLabel *replaceDesc = new QLabel(QTStr("Restore.Mode.Replace.Description"), modeGroup);
	QLabel *mergeDesc = new QLabel(QTStr("Restore.Mode.Merge.Description"), modeGroup);
	for (QLabel *label : {replaceDesc, mergeDesc}) {
		label->setWordWrap(true);
		label->setIndent(24);
	}
	replaceMode->setChecked(true);

	QVBoxLayout *modeLayout = new QVBoxLayout(modeGroup);
	modeLayout->addWidget(replaceMode);
	modeLayout->addWidget(replaceDesc);
	modeLayout->addWidget(mergeMode);
	modeLayout->addWidget(mergeDesc);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	restoreButton = buttons->addButton(QTStr("Restore.RestoreButton"), QDialogButtonBox::ActionRole);
	restoreButton->setDefault(true);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(intro);
	layout->addLayout(pendingRow);
	layout->addLayout(fileRow);
	layout->addWidget(summary);
	layout->addWidget(categories);
	layout->addWidget(modeGroup);
	layout->addStretch();
	layout->addWidget(buttons);

	for (QCheckBox *box : {profiles, collections, appSettings, themes, pluginSettings}) {
		connect(box, &QCheckBox::toggled, this, &OBSRestoreSettingsDialog::UpdateControls);
	}

	connect(browse, &QPushButton::clicked, this, &OBSRestoreSettingsDialog::Browse);
	connect(cancelPending, &QPushButton::clicked, this, [this]() {
		CancelPendingRestore();
		UpdatePending();
	});
	connect(restoreButton, &QPushButton::clicked, this, &OBSRestoreSettingsDialog::Restore);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	UpdatePending();
	UpdateControls();
}

uint32_t OBSRestoreSettingsDialog::SelectedCategories() const
{
	return CheckedCategories({profiles, collections, appSettings, themes, pluginSettings});
}

void OBSRestoreSettingsDialog::UpdatePending()
{
	bool hasPending = HasPendingRestore();
	pending->setVisible(hasPending);
	cancelPending->setVisible(hasPending);
}

void OBSRestoreSettingsDialog::UpdateControls()
{
	restoreButton->setEnabled(loaded && SelectedCategories() != 0);
}

void OBSRestoreSettingsDialog::Browse()
{
	error_code ec;
	fs::path folder = DefaultBackupFolder();
	QString start = fs::exists(folder, ec) ? ToQString(folder) : QDir::homePath();

	QString path = OpenFile(this, QTStr("Restore.Open"), start, QTStr("Backup.FileFilter") + " (*.zip)");
	if (!path.isEmpty()) {
		Load(path);
	}
}

void OBSRestoreSettingsDialog::Load(const QString &path)
{
	string error;
	loaded = InspectBackup(ToPath(path), info, error);
	file->setText(QDir::toNativeSeparators(path));

	if (!loaded) {
		summary->setText(QTStr("Restore.Invalid").arg(QString::fromStdString(error).toHtmlEscaped()));
		summary->setVisible(true);
		UpdateControls();
		return;
	}

	auto names = [](const vector<NamedItem> &items) {
		QStringList list;
		for (const NamedItem &item : items) {
			list << QString::fromStdString(item.name).toHtmlEscaped();
		}
		return list.isEmpty() ? QTStr("None") : list.join(", ");
	};

	QString text = QTStr("Restore.Summary")
			       .arg(QString::fromStdString(info.created).replace('T', ' ').toHtmlEscaped(),
				    QString::fromStdString(info.obsVersion).toHtmlEscaped(),
				    QString::fromStdString(info.os).toHtmlEscaped(), names(info.profiles),
				    names(info.collections));
	if (!info.includesSecrets) {
		text += "<br><b>" + QTStr("Restore.NoSecrets") + "</b>";
	}

	summary->setText(text);
	summary->setVisible(true);

	profiles->setEnabled(info.categories & Profiles);
	collections->setEnabled(info.categories & SceneCollections);
	appSettings->setEnabled(info.categories & AppSettings);
	themes->setEnabled(info.categories & Themes);
	pluginSettings->setEnabled(info.categories & PluginSettings);

	UpdateControls();
}

void OBSRestoreSettingsDialog::Restore()
{
	if (main->Active()) {
		OBSMessageBox::warning(this, QTStr("Restore.Title"), QTStr("Restore.Active"));
		return;
	}

	RestoreMode mode = replaceMode->isChecked() ? RestoreMode::Replace : RestoreMode::Merge;
	QString confirm = mode == RestoreMode::Replace ? QTStr("Restore.Confirm.Replace")
						       : QTStr("Restore.Confirm.Merge");
	if (OBSMessageBox::question(this, QTStr("Restore.Title"), confirm) != QMessageBox::Yes) {
		return;
	}

	/* The safety backup taken while staging should include the latest state */
	main->SaveForBackup();

	QApplication::setOverrideCursor(Qt::WaitCursor);
	string error;
	bool staged = StageRestore(ToPath(file->text()), SelectedCategories(), mode, error);
	QApplication::restoreOverrideCursor();

	if (!staged) {
		OBSMessageBox::critical(this, QTStr("Restore.Title"),
					QTStr("Restore.Failed").arg(QString::fromStdString(error)));
		return;
	}

	QMessageBox::StandardButton button =
		OBSMessageBox::question(this, QTStr("Restore.Title"), QTStr("Restore.RestartNow"));
	restartRequested = button == QMessageBox::Yes;
	if (!restartRequested) {
		OBSMessageBox::information(this, QTStr("Restore.Title"), QTStr("Restore.NextStart"));
	}

	accept();
}

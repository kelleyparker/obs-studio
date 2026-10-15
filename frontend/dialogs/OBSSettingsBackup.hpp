#pragma once

#include <utility/SettingsBackup.hpp>

#include <QDialog>

class OBSBasic;
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

class OBSBackupSettingsDialog : public QDialog {
	Q_OBJECT

public:
	OBSBackupSettingsDialog(OBSBasic *main);

private:
	OBSBasic *main;

	QCheckBox *profiles;
	QCheckBox *collections;
	QCheckBox *appSettings;
	QCheckBox *themes;
	QCheckBox *pluginSettings;
	QCheckBox *includeSecrets;
	QLabel *secretsWarning;
	QProgressBar *progress;
	QPushButton *backupButton;

	QCheckBox *autoBackup;
	QSpinBox *autoKeep;
	QLineEdit *autoFolder;
	QPushButton *autoBrowse;

	uint32_t SelectedCategories() const;
	void UpdateControls();
	void SaveAutoBackupSettings();
	void BackUp();
};

class OBSRestoreSettingsDialog : public QDialog {
	Q_OBJECT

public:
	OBSRestoreSettingsDialog(OBSBasic *main);

	bool RestartRequested() const { return restartRequested; }

private:
	OBSBasic *main;
	SettingsBackup::BackupInfo info;
	bool loaded = false;
	bool restartRequested = false;

	QLabel *pending;
	QPushButton *cancelPending;
	QLineEdit *file;
	QLabel *summary;
	QCheckBox *profiles;
	QCheckBox *collections;
	QCheckBox *appSettings;
	QCheckBox *themes;
	QCheckBox *pluginSettings;
	QRadioButton *replaceMode;
	QRadioButton *mergeMode;
	QPushButton *restoreButton;

	uint32_t SelectedCategories() const;
	void Browse();
	void Load(const QString &path);
	void UpdateControls();
	void UpdatePending();
	void Restore();
};

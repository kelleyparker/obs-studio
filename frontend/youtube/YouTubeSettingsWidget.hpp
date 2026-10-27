#pragma once

#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class YouTubeSettingsWidget : public QWidget {
	Q_OBJECT

public:
	explicit YouTubeSettingsWidget(QWidget *parent = nullptr);

	void LoadFromManager();
	void SaveToManager();

signals:
	void SettingsModified();

private slots:
	void OnPreferredDefaultChanged(int index);
	void OnMovePriorityUp();
	void OnMovePriorityDown();
	void OnImportGlobalOpenAIKey();
	void OnBrowseClientSecretJson();
	void OnConnectYouTubeAccount();
	void OnDisconnectYouTubeAccount();

private:
	void RefreshPriorityLabels();
	QStringList CurrentPriorityKeys() const;
	void SetPriorityKeys(const QStringList &keys);

	// Section 1: 1-to-4 Priority & AI Models
	QComboBox *preferredDefaultCombo = nullptr;
	QListWidget *priorityListWidget = nullptr;
	QPushButton *moveUpBtn = nullptr;
	QPushButton *moveDownBtn = nullptr;
	QComboBox *thumbnailStyleCombo = nullptr;
	QComboBox *whisperModelCombo = nullptr;
	QComboBox *ollamaModelCombo = nullptr;

	// Section 2: OBS-Exclusive DPAPI Vault
	QLineEdit *geminiKeyEdit = nullptr;
	QLineEdit *openaiKeyEdit = nullptr;
	QLineEdit *claudeKeyEdit = nullptr;
	QLabel *vaultStatusLabel = nullptr;

	// Section 3: YouTube OAuth & Upload Defaults
	QLabel *oauthStatusLabel = nullptr;
	QLineEdit *oauthClientIdEdit = nullptr;
	QLineEdit *oauthClientSecretEdit = nullptr;
	QLineEdit *clientSecretFileEdit = nullptr;
	QComboBox *privacyCombo = nullptr;
	QComboBox *categoryCombo = nullptr;
	QCheckBox *madeForKidsCheck = nullptr;

	// Section 4: Smart Video Editing Presets
	QComboBox *editingPresetCombo = nullptr;
	QDoubleSpinBox *silenceDbSpin = nullptr;
	QDoubleSpinBox *minSilenceSecSpin = nullptr;
	QDoubleSpinBox *paddingSecSpin = nullptr;
	QCheckBox *cutRetakesCheck = nullptr;

	bool internalUpdating = false;
};

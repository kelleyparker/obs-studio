#pragma once

#include <QList>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;

class YouTubeSettingsWidget : public QWidget {
	Q_OBJECT

public:
	explicit YouTubeSettingsWidget(QWidget *parent = nullptr);

	void LoadFromManager();
	void SaveToManager();

	/* Local Ollama / Gemini / ChatGPT / Claude / Off, shared with the YouTube dock */
	static void FillThumbnailImageSources(QComboBox *combo);
	/* ACE-Step / Lyria, shared with the YouTube dock */
	static void FillMusicSources(QComboBox *combo);
	/* Stable Diffusion / Nano Banana 2 / Nano Banana Pro, shared with the B-roll review */
	static void FillBrollImageSources(QComboBox *combo, bool compact = false);
	static double BrollImagePrice(const QString &source);
	/* The Fewer <-> More dial's steps: Off, Very few ... Lots */
	static QString BrollAmountName(int amount);
	static constexpr int kBrollAmountMax = 5;

signals:
	void SettingsModified();

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

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

	/* Shows only the settings whose label, text, choices or tooltip contain
	 * the text, as the Hotkeys page's filter does. Typed values (API keys)
	 * are never searched. */
	void ApplyFilter(const QString &text);

	QLineEdit *filterEdit = nullptr;
	QLabel *headerLabel = nullptr;
	QLabel *noMatchLabel = nullptr;
	QScrollArea *scrollArea = nullptr;
	QList<QGroupBox *> sections;

	// Section 1: 1-to-4 Priority & AI Models
	QComboBox *preferredDefaultCombo = nullptr;
	QListWidget *priorityListWidget = nullptr;
	QPushButton *moveUpBtn = nullptr;
	QPushButton *moveDownBtn = nullptr;
	QComboBox *thumbnailImageSourceCombo = nullptr;
	QComboBox *thumbnailImageModelCombo = nullptr;
	QCheckBox *cloudImageFallbackCheck = nullptr;
	QCheckBox *musicEnabledCheck = nullptr;
	QComboBox *musicSourceCombo = nullptr;
	QComboBox *autoFinishCombo = nullptr;
	QComboBox *scriptModelCombo = nullptr;
	QComboBox *scriptEffortCombo = nullptr;
	QLabel *scriptCostLabel = nullptr;
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
	QCheckBox *normalizeLoudnessCheck = nullptr;

	// Section 5: Screen B-roll
	QCheckBox *brollEnabledCheck = nullptr;
	QComboBox *brollSourceCombo = nullptr;
	QComboBox *brollStyleCombo = nullptr;
	QDoubleSpinBox *brollCoverageSpin = nullptr;
	QSlider *brollAmountSlider = nullptr;
	QLabel *brollAmountLabel = nullptr;
	QComboBox *brollImageSourceCombo = nullptr;

	void RefreshBrollSources(const QString &selected);

	bool internalUpdating = false;
};

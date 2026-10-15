#pragma once

#include <utility/MultistreamOutput.hpp>

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QWidget;

class MultistreamDestinationDialog : public QDialog {
	Q_OBJECT

public:
	MultistreamDestinationDialog(const MultistreamDestination &dest, MultistreamMode mode, QWidget *parent);

	MultistreamDestination Destination() const { return destination; }

private:
	MultistreamDestination destination;

	QComboBox *platform;
	QComboBox *canvas;
	QLineEdit *name;
	QLineEdit *server;
	QLineEdit *key;
	QPushButton *showKey;
	QLabel *keyLink;
	QComboBox *qualityMode;
	QLabel *encoderLabel;
	QComboBox *encoder;
	QWidget *presetBox;
	QButtonGroup *presetGroup;
	std::vector<std::string> presetIds;
	QLabel *maxSummary;
	QWidget *customQuality;
	QSpinBox *videoBitrate;
	QComboBox *audioBitrate;
	QComboBox *resolution;

	std::string CurrentPlatform() const;
	MultistreamQualityMode SelectedMode() const;
	std::string SelectedPreset() const;
	void PlatformChanged();
	void UpdateEncoders();
	void UpdateModes();
	void UpdatePresets();
	void UpdateQuality();
	void accept() override;
};

class OBSMultistreamSettings : public QDialog {
	Q_OBJECT

public:
	OBSMultistreamSettings(MultistreamOutput *multistream, QWidget *parent);

private:
	MultistreamOutput *multistream;
	std::vector<MultistreamDestination> destinations;

	QCheckBox *enable;
	QRadioButton *sharedMode;
	QRadioButton *separateMode;
	QTableWidget *table;
	QPushButton *addButton;
	QPushButton *editButton;
	QPushButton *removeButton;
	QLabel *bandwidth;
	QLabel *activeWarning;

	MultistreamMode CurrentMode() const;
	void RefreshTable();
	void UpdateBandwidth();
	void UpdateButtons();
	void AddDestination();
	void EditDestination();
	void RemoveDestination();
	void accept() override;
};

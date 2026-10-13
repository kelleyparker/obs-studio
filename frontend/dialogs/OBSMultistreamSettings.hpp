#pragma once

#include <utility/MultistreamOutput.hpp>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;

class MultistreamDestinationDialog : public QDialog {
	Q_OBJECT

public:
	MultistreamDestinationDialog(const MultistreamDestination &dest, MultistreamMode mode, QWidget *parent);

	MultistreamDestination Destination() const { return destination; }

private:
	MultistreamDestination destination;

	QComboBox *platform;
	QLineEdit *name;
	QLineEdit *server;
	QLineEdit *key;
	QPushButton *showKey;
	QLabel *keyLink;
	QSpinBox *videoBitrate;
	QComboBox *audioBitrate;
	QComboBox *encoder;
	QComboBox *resolution;

	void PlatformChanged();
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

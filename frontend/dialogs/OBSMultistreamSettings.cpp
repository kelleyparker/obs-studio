#include "OBSMultistreamSettings.hpp"

#include <utility/audio-encoders.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace std;

static QString PlatformDisplayName(const string &platform)
{
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(platform);
	return preset ? QString::fromUtf8(preset->displayName) : QString::fromStdString(platform);
}

static QString QualitySummary(const MultistreamDestination &dest, MultistreamMode mode)
{
	if (mode == MultistreamMode::Shared) {
		return QTStr("Multistream.Quality.SameAsMain");
	}

	QString res = dest.width && dest.height ? QString("%1x%2").arg(dest.width).arg(dest.height)
						: QTStr("Multistream.Resolution.Output");
	return QString("%1 kbps / %2 kbps, %3").arg(dest.videoBitrate).arg(dest.audioBitrate).arg(res);
}

static bool IsValidStreamUrl(const QString &url)
{
	return url.startsWith("rtmp://", Qt::CaseInsensitive) || url.startsWith("rtmps://", Qt::CaseInsensitive);
}

/* ------------------------------------------------------------------------- */

MultistreamDestinationDialog::MultistreamDestinationDialog(const MultistreamDestination &dest, MultistreamMode mode,
							   QWidget *parent)
	: QDialog(parent),
	  destination(dest)
{
	setWindowTitle(QTStr("Multistream.Destination.Title"));
	setMinimumWidth(520);

	platform = new QComboBox(this);
	for (const auto &preset : GetMultistreamPlatformPresets()) {
		platform->addItem(QString::fromUtf8(preset.displayName), QString::fromUtf8(preset.id));
	}

	name = new QLineEdit(this);
	server = new QLineEdit(this);
	server->setPlaceholderText("rtmp://");

	key = new QLineEdit(this);
	key->setEchoMode(QLineEdit::Password);
	showKey = new QPushButton(QTStr("Show"), this);
	showKey->setCheckable(true);
	connect(showKey, &QPushButton::toggled, this, [this](bool show) {
		key->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
		showKey->setText(show ? QTStr("Hide") : QTStr("Show"));
	});

	QHBoxLayout *keyLayout = new QHBoxLayout();
	keyLayout->setContentsMargins(0, 0, 0, 0);
	keyLayout->addWidget(key);
	keyLayout->addWidget(showKey);

	keyLink = new QLabel(this);
	keyLink->setOpenExternalLinks(true);

	QFormLayout *form = new QFormLayout();
	form->addRow(QTStr("Multistream.Platform"), platform);
	form->addRow(QTStr("Name"), name);
	form->addRow(QTStr("Basic.AutoConfig.StreamPage.Server"), server);
	form->addRow(QTStr("Basic.AutoConfig.StreamPage.StreamKey"), keyLayout);
	form->addRow(QString(), keyLink);

	/* Per-destination quality (separate encoder mode only) */
	videoBitrate = new QSpinBox(this);
	videoBitrate->setRange(200, 100000);
	videoBitrate->setSingleStep(250);
	videoBitrate->setSuffix(" kbps");

	audioBitrate = new QComboBox(this);
	for (const auto &[bitrate, encoderId] : GetSimpleAACEncoderBitrateMap()) {
		audioBitrate->addItem(QString::number(bitrate), bitrate);
	}

	encoder = new QComboBox(this);
	encoder->addItem(QTStr("Multistream.Encoder.SameAsMain"), QString());
	const char *id;
	for (size_t idx = 0; obs_enum_encoder_types(idx, &id); idx++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) {
			continue;
		}
		if (strcmp(obs_get_encoder_codec(id), "h264") != 0) {
			continue;
		}
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) {
			continue;
		}
		encoder->addItem(QString::fromUtf8(obs_encoder_get_display_name(id)), QString::fromUtf8(id));
	}

	resolution = new QComboBox(this);
	resolution->setEditable(true);
	resolution->addItem(QTStr("Multistream.Resolution.Output"), QString());
	resolution->addItem("1920x1080", "1920x1080");
	resolution->addItem("1280x720", "1280x720");
	resolution->addItem("852x480", "852x480");

	QGroupBox *quality = new QGroupBox(QTStr("Multistream.Quality"), this);
	QFormLayout *qualityForm = new QFormLayout(quality);
	qualityForm->addRow(QTStr("Basic.Settings.Output.VideoBitrate"), videoBitrate);
	qualityForm->addRow(QTStr("Basic.Settings.Output.AudioBitrate"), audioBitrate);
	qualityForm->addRow(QTStr("Basic.Settings.Output.Encoder.Video"), encoder);
	qualityForm->addRow(QTStr("Basic.Settings.Video.ScaledResolution"), resolution);

	QLabel *sharedNote = new QLabel(QTStr("Multistream.Quality.SharedNote"), this);
	sharedNote->setWordWrap(true);

	bool separate = mode == MultistreamMode::Separate;
	quality->setVisible(separate);
	sharedNote->setVisible(!separate);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &MultistreamDestinationDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(quality);
	layout->addWidget(sharedNote);
	layout->addStretch();
	layout->addWidget(buttons);

	/* Load values */
	int platformIdx = platform->findData(QString::fromStdString(dest.platform));
	platform->setCurrentIndex(platformIdx >= 0 ? platformIdx : platform->count() - 1);
	name->setText(QString::fromStdString(dest.name));
	server->setText(QString::fromStdString(dest.server));
	key->setText(QString::fromStdString(dest.key));
	videoBitrate->setValue(dest.videoBitrate);

	int audioIdx = audioBitrate->findData(FindClosestAvailableSimpleAACBitrate(dest.audioBitrate));
	audioBitrate->setCurrentIndex(audioIdx >= 0 ? audioIdx : 0);

	int encoderIdx = encoder->findData(QString::fromStdString(dest.encoder));
	encoder->setCurrentIndex(encoderIdx >= 0 ? encoderIdx : 0);

	if (dest.width && dest.height) {
		resolution->setCurrentText(QString("%1x%2").arg(dest.width).arg(dest.height));
	} else {
		resolution->setCurrentIndex(0);
	}

	connect(platform, &QComboBox::currentIndexChanged, this, &MultistreamDestinationDialog::PlatformChanged);
	PlatformChanged();

	/* PlatformChanged() fills in defaults; keep saved values when editing */
	if (!dest.server.empty()) {
		server->setText(QString::fromStdString(dest.server));
	}
	if (!dest.name.empty()) {
		name->setText(QString::fromStdString(dest.name));
	}
}

void MultistreamDestinationDialog::PlatformChanged()
{
	string id = platform->currentData().toString().toStdString();
	const MultistreamPlatformPreset *preset = FindMultistreamPlatformPreset(id);
	if (!preset) {
		return;
	}

	bool isCustom = id == "custom";

	/* Only replace values the user hasn't customized */
	bool nameIsPreset = name->text().isEmpty();
	bool serverIsPreset = server->text().isEmpty();
	for (const auto &other : GetMultistreamPlatformPresets()) {
		if (name->text() == QString::fromUtf8(other.displayName)) {
			nameIsPreset = true;
		}
		if (server->text() == QString::fromUtf8(other.server)) {
			serverIsPreset = true;
		}
	}

	if (nameIsPreset) {
		name->setText(isCustom ? QString() : QString::fromUtf8(preset->displayName));
	}
	if (serverIsPreset) {
		server->setText(QString::fromUtf8(preset->server));
	}
	if (destination.server.empty()) {
		videoBitrate->setValue(preset->defaultVideoBitrate);
	}

	if (*preset->streamKeyUrl) {
		keyLink->setText(
			QString("<a href=\"%1\">%2</a>")
				.arg(QString::fromUtf8(preset->streamKeyUrl),
				     QTStr("Multistream.GetStreamKey").arg(QString::fromUtf8(preset->displayName))));
		keyLink->setVisible(true);
	} else {
		keyLink->setVisible(false);
	}
}

void MultistreamDestinationDialog::accept()
{
	QString serverUrl = server->text().trimmed();
	QString streamKey = key->text().trimmed();
	QString displayName = name->text().trimmed();

	if (displayName.isEmpty()) {
		displayName = platform->currentText();
	}

	if (!IsValidStreamUrl(serverUrl)) {
		OBSMessageBox::warning(this, QTStr("Multistream.Destination.Title"),
				       QTStr("Multistream.Error.InvalidServer"));
		return;
	}
	if (streamKey.isEmpty()) {
		OBSMessageBox::warning(this, QTStr("Multistream.Destination.Title"),
				       QTStr("Multistream.Error.MissingKey"));
		return;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	QString res = resolution->currentText().trimmed();
	QRegularExpressionMatch match = QRegularExpression("^\\s*(\\d+)\\s*[xX]\\s*(\\d+)\\s*$").match(res);
	if (match.hasMatch()) {
		width = match.captured(1).toUInt();
		height = match.captured(2).toUInt();
		if (width < 16 || height < 16 || width > 16384 || height > 16384 || (width % 2) || (height % 2)) {
			OBSMessageBox::warning(this, QTStr("Multistream.Destination.Title"),
					       QTStr("Multistream.Error.InvalidResolution"));
			return;
		}
	} else if (res != resolution->itemText(0) && !res.isEmpty()) {
		OBSMessageBox::warning(this, QTStr("Multistream.Destination.Title"),
				       QTStr("Multistream.Error.InvalidResolution"));
		return;
	}

	destination.name = displayName.toStdString();
	destination.platform = platform->currentData().toString().toStdString();
	destination.server = serverUrl.toStdString();
	destination.key = streamKey.toStdString();
	destination.videoBitrate = videoBitrate->value();
	destination.audioBitrate = audioBitrate->currentData().toInt();
	destination.encoder = encoder->currentData().toString().toStdString();
	destination.width = width;
	destination.height = height;

	if (destination.id.empty()) {
		destination.id = MultistreamOutput::GenerateId();
	}

	QDialog::accept();
}

/* ------------------------------------------------------------------------- */

enum Column {
	ColumnEnabled,
	ColumnName,
	ColumnPlatform,
	ColumnServer,
	ColumnQuality,
	ColumnCount,
};

OBSMultistreamSettings::OBSMultistreamSettings(MultistreamOutput *multistream_, QWidget *parent)
	: QDialog(parent),
	  multistream(multistream_),
	  destinations(multistream_->Destinations())
{
	setWindowTitle(QTStr("Multistream.Title"));
	setMinimumWidth(760);
	resize(860, 620);

	QLabel *intro = new QLabel(QTStr("Multistream.Intro"), this);
	intro->setWordWrap(true);

	enable = new QCheckBox(QTStr("Multistream.Enable"), this);
	enable->setChecked(multistream->Enabled());

	QGroupBox *modeGroup = new QGroupBox(QTStr("Multistream.Mode"), this);
	sharedMode = new QRadioButton(QTStr("Multistream.Mode.Shared"), modeGroup);
	separateMode = new QRadioButton(QTStr("Multistream.Mode.Separate"), modeGroup);
	QLabel *sharedDesc = new QLabel(QTStr("Multistream.Mode.Shared.Description"), modeGroup);
	QLabel *separateDesc = new QLabel(QTStr("Multistream.Mode.Separate.Description"), modeGroup);
	sharedDesc->setWordWrap(true);
	separateDesc->setWordWrap(true);
	sharedDesc->setIndent(24);
	separateDesc->setIndent(24);

	/* Wrapped labels must keep their full height when the table shrinks */
	for (QLabel *label : {intro, sharedDesc, separateDesc}) {
		label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	}

	QButtonGroup *modeButtons = new QButtonGroup(this);
	modeButtons->addButton(sharedMode);
	modeButtons->addButton(separateMode);
	(multistream->Mode() == MultistreamMode::Separate ? separateMode : sharedMode)->setChecked(true);

	QVBoxLayout *modeLayout = new QVBoxLayout(modeGroup);
	modeLayout->addWidget(sharedMode);
	modeLayout->addWidget(sharedDesc);
	modeLayout->addWidget(separateMode);
	modeLayout->addWidget(separateDesc);

	table = new QTableWidget(0, ColumnCount, this);
	table->setMinimumHeight(150);
	table->setHorizontalHeaderLabels({QTStr("Enable"), QTStr("Name"), QTStr("Multistream.Platform"),
					  QTStr("Basic.AutoConfig.StreamPage.Server"), QTStr("Multistream.Quality")});
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->verticalHeader()->setVisible(false);
	table->horizontalHeader()->setSectionResizeMode(ColumnEnabled, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(ColumnServer, QHeaderView::Stretch);
	table->horizontalHeader()->setSectionResizeMode(ColumnQuality, QHeaderView::ResizeToContents);

	addButton = new QPushButton(QTStr("Add"), this);
	editButton = new QPushButton(QTStr("Multistream.Edit"), this);
	removeButton = new QPushButton(QTStr("Remove"), this);

	QHBoxLayout *tableButtons = new QHBoxLayout();
	tableButtons->addWidget(addButton);
	tableButtons->addWidget(editButton);
	tableButtons->addWidget(removeButton);
	tableButtons->addStretch();

	bandwidth = new QLabel(this);
	bandwidth->setWordWrap(true);

	activeWarning = new QLabel(QTStr("Multistream.ActiveWarning"), this);
	activeWarning->setWordWrap(true);
	activeWarning->setProperty("class", "text-warning");
	activeWarning->setVisible(multistream->Active());

	QLabel *primaryNote = new QLabel(QTStr("Multistream.PrimaryNote"), this);
	primaryNote->setWordWrap(true);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &OBSMultistreamSettings::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(intro);
	layout->addWidget(enable);
	layout->addWidget(modeGroup);
	layout->addWidget(primaryNote);
	layout->addWidget(table, 1);
	layout->addLayout(tableButtons);
	layout->addWidget(bandwidth);
	layout->addWidget(activeWarning);
	layout->addWidget(buttons);

	connect(addButton, &QPushButton::clicked, this, &OBSMultistreamSettings::AddDestination);
	connect(editButton, &QPushButton::clicked, this, &OBSMultistreamSettings::EditDestination);
	connect(removeButton, &QPushButton::clicked, this, &OBSMultistreamSettings::RemoveDestination);
	connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int column) {
		if (column != ColumnEnabled) {
			EditDestination();
		}
	});
	connect(table, &QTableWidget::itemSelectionChanged, this, &OBSMultistreamSettings::UpdateButtons);
	connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (item->column() != ColumnEnabled || item->row() >= (int)destinations.size()) {
			return;
		}
		destinations[item->row()].enabled = item->checkState() == Qt::Checked;
		UpdateBandwidth();
	});
	connect(sharedMode, &QRadioButton::toggled, this, [this]() {
		RefreshTable();
		UpdateBandwidth();
	});

	RefreshTable();
	UpdateBandwidth();
	UpdateButtons();
}

MultistreamMode OBSMultistreamSettings::CurrentMode() const
{
	return separateMode->isChecked() ? MultistreamMode::Separate : MultistreamMode::Shared;
}

void OBSMultistreamSettings::RefreshTable()
{
	QSignalBlocker blocker(table);
	int selected = table->currentRow();

	table->setRowCount((int)destinations.size());
	for (int row = 0; row < (int)destinations.size(); row++) {
		const MultistreamDestination &dest = destinations[row];

		QTableWidgetItem *enabledItem = new QTableWidgetItem();
		enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		enabledItem->setCheckState(dest.enabled ? Qt::Checked : Qt::Unchecked);

		table->setItem(row, ColumnEnabled, enabledItem);
		table->setItem(row, ColumnName, new QTableWidgetItem(QString::fromStdString(dest.name)));
		table->setItem(row, ColumnPlatform, new QTableWidgetItem(PlatformDisplayName(dest.platform)));
		table->setItem(row, ColumnServer, new QTableWidgetItem(QString::fromStdString(dest.server)));
		table->setItem(row, ColumnQuality, new QTableWidgetItem(QualitySummary(dest, CurrentMode())));
	}

	if (selected >= 0 && selected < table->rowCount()) {
		table->selectRow(selected);
	}
}

void OBSMultistreamSettings::UpdateBandwidth()
{
	int count = 0;
	int extraKbps = 0;
	for (const auto &dest : destinations) {
		if (dest.enabled) {
			count++;
			extraKbps += dest.videoBitrate + dest.audioBitrate;
		}
	}

	if (CurrentMode() == MultistreamMode::Shared) {
		bandwidth->setText(QTStr("Multistream.Bandwidth.Shared").arg(count + 1));
	} else {
		bandwidth->setText(QTStr("Multistream.Bandwidth.Separate").arg(count).arg(extraKbps));
	}
}

void OBSMultistreamSettings::UpdateButtons()
{
	bool hasSelection = table->currentRow() >= 0 && !table->selectedItems().isEmpty();
	editButton->setEnabled(hasSelection);
	removeButton->setEnabled(hasSelection && !multistream->IsActive(destinations[table->currentRow()].id));
}

void OBSMultistreamSettings::AddDestination()
{
	MultistreamDestination dest;

	/* Suggest the first platform that isn't configured yet */
	for (const auto &preset : GetMultistreamPlatformPresets()) {
		bool used = false;
		for (const auto &existing : destinations) {
			used = used || existing.platform == preset.id;
		}
		if (!used) {
			dest.platform = preset.id;
			break;
		}
	}

	MultistreamDestinationDialog dialog(dest, CurrentMode(), this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	destinations.push_back(dialog.Destination());
	RefreshTable();
	table->selectRow((int)destinations.size() - 1);
	UpdateBandwidth();
}

void OBSMultistreamSettings::EditDestination()
{
	int row = table->currentRow();
	if (row < 0 || row >= (int)destinations.size()) {
		return;
	}

	MultistreamDestinationDialog dialog(destinations[row], CurrentMode(), this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	destinations[row] = dialog.Destination();
	RefreshTable();
	UpdateBandwidth();
}

void OBSMultistreamSettings::RemoveDestination()
{
	int row = table->currentRow();
	if (row < 0 || row >= (int)destinations.size()) {
		return;
	}

	QMessageBox::StandardButton button = OBSMessageBox::question(
		this, QTStr("Multistream.Title"),
		QTStr("Multistream.ConfirmRemove").arg(QString::fromStdString(destinations[row].name)));
	if (button != QMessageBox::Yes) {
		return;
	}

	destinations.erase(destinations.begin() + row);
	RefreshTable();
	UpdateBandwidth();
	UpdateButtons();
}

void OBSMultistreamSettings::accept()
{
	multistream->SetEnabled(enable->isChecked());
	multistream->SetMode(CurrentMode());
	multistream->SetDestinations(destinations);
	multistream->Save();

	QDialog::accept();
}

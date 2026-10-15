#include "VerticalDocks.hpp"

#include <dialogs/NameDialog.hpp>
#include <utility/VerticalCanvas.hpp>
#include <utility/VerticalOutput.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <qt-wrappers.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

using namespace std;

static QPushButton *ToolButton(const QString &text, const QString &tooltip, QWidget *parent)
{
	QPushButton *button = new QPushButton(text, parent);
	button->setToolTip(tooltip);
	button->setAccessibleName(tooltip);
	button->setMaximumWidth(48);
	return button;
}

/* ------------------------------------------------------------------------- */
/* Preview */

VerticalPreviewDock::VerticalPreviewDock(VerticalCanvas *vertical_, VerticalOutput *output_, QWidget *parent)
	: QWidget(parent),
	  vertical(vertical_),
	  output(output_)
{
	display = new OBSQTDisplay(this);
	display->setMinimumSize(90, 160);
	display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(display->GetDisplay(), Render, this);
	};
	connect(display, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);

	QWidget *disabledPage = new QWidget(this);
	status = new QLabel(QTStr("Vertical.Dock.Disabled"), disabledPage);
	status->setWordWrap(true);
	status->setAlignment(Qt::AlignCenter);
	QPushButton *enableButton = new QPushButton(QTStr("Vertical.Dock.OpenSettings"), disabledPage);
	connect(enableButton, &QPushButton::clicked, this, &VerticalPreviewDock::OpenSettings);
	QVBoxLayout *disabledLayout = new QVBoxLayout(disabledPage);
	disabledLayout->addStretch();
	disabledLayout->addWidget(status);
	disabledLayout->addWidget(enableButton, 0, Qt::AlignCenter);
	disabledLayout->addStretch();

	stack = new QStackedWidget(this);
	stack->addWidget(display);
	stack->addWidget(disabledPage);

	recordButton = new QPushButton(this);
	replayButton = new QPushButton(this);
	saveReplayButton = new QPushButton(QTStr("Vertical.Dock.SaveReplay"), this);
	QPushButton *settingsButton = new QPushButton(QTStr("Settings"), this);

	connect(recordButton, &QPushButton::clicked, this, &VerticalPreviewDock::ToggleRecording);
	connect(replayButton, &QPushButton::clicked, this, &VerticalPreviewDock::ToggleReplayBuffer);
	connect(saveReplayButton, &QPushButton::clicked, this, [this]() {
		if (output) {
			output->SaveReplay();
		}
	});
	connect(settingsButton, &QPushButton::clicked, this, &VerticalPreviewDock::OpenSettings);

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(recordButton, 1);
	buttons->addWidget(replayButton, 1);
	buttons->addWidget(saveReplayButton);
	buttons->addWidget(settingsButton);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(stack, 1);
	layout->addLayout(buttons);

	connect(vertical_, &VerticalCanvas::VideoChanged, this, &VerticalPreviewDock::UpdateCanvas);
	connect(vertical_, &VerticalCanvas::EnabledChanged, this, &VerticalPreviewDock::UpdateCanvas);
	connect(output_, &VerticalOutput::RecordingStarted, this, &VerticalPreviewDock::UpdateButtons);
	connect(output_, &VerticalOutput::RecordingStopped, this, [this](int code, const QString &error) {
		UpdateButtons();
		if (code != OBS_OUTPUT_SUCCESS) {
			OBSMessageBox::critical(this, QTStr("Vertical.Error.RecordingStopped"),
						error.isEmpty() ? QTStr("Output.StartFailedGeneric") : error);
		}
	});
	connect(output_, &VerticalOutput::ReplayBufferStarted, this, &VerticalPreviewDock::UpdateButtons);
	connect(output_, &VerticalOutput::ReplayBufferStopped, this, [this](int code, const QString &error) {
		UpdateButtons();
		if (code != OBS_OUTPUT_SUCCESS) {
			OBSMessageBox::critical(this, QTStr("Vertical.Error.ReplayStopped"),
						error.isEmpty() ? QTStr("Output.StartFailedGeneric") : error);
		}
	});
	connect(output_, &VerticalOutput::ReplaySaved, this, [](const QString &path) {
		OBSBasic *main = OBSBasic::Get();
		main->ShowStatusBarMessage(QTStr("Vertical.ReplaySaved").arg(path));
	});

	connect(&timer, &QTimer::timeout, this, &VerticalPreviewDock::UpdateButtons);
	timer.start(1000);

	UpdateCanvas();
}

VerticalPreviewDock::~VerticalPreviewDock()
{
	DestroyDisplay();
}

void VerticalPreviewDock::DestroyDisplay()
{
	if (display->GetDisplay()) {
		obs_display_remove_draw_callback(display->GetDisplay(), Render, this);
	}
	display->DestroyDisplay();
}

void VerticalPreviewDock::UpdateCanvas()
{
	bool hasVideo = vertical && vertical->Enabled() && vertical->HasVideo();

	{
		std::lock_guard<std::mutex> lock(canvasMutex);
		renderCanvas = hasVideo ? OBSGetWeakRef(vertical->GetCanvas()) : OBSWeakCanvas();
	}

	stack->setCurrentIndex(hasVideo ? 0 : 1);
	UpdateButtons();
}

void VerticalPreviewDock::UpdateButtons()
{
	bool hasVideo = vertical && vertical->Enabled() && vertical->HasVideo();
	bool recording = output && output->RecordingActive();
	bool replay = output && output->ReplayBufferActive();

	if (recording) {
		uint64_t seconds = output->RecordingSeconds();
		QString time = QString::asprintf("%02d:%02d:%02d", int(seconds / 3600), int((seconds / 60) % 60),
						 int(seconds % 60));
		recordButton->setText(QTStr("Vertical.Dock.StopRecording").arg(time));
	} else {
		recordButton->setText(QTStr("Vertical.Dock.StartRecording"));
	}
	replayButton->setText(replay ? QTStr("Vertical.Dock.StopReplay") : QTStr("Vertical.Dock.StartReplay"));

	recordButton->setEnabled(hasVideo || recording);
	replayButton->setEnabled(hasVideo || replay);
	saveReplayButton->setEnabled(replay);
}

void VerticalPreviewDock::ToggleRecording()
{
	if (!output) {
		return;
	}

	if (output->RecordingActive()) {
		output->StopRecording();
	} else {
		QString error;
		if (!output->StartRecording(error) && !error.isEmpty()) {
			OBSMessageBox::critical(this, QTStr("Output.StartRecordingFailed"), error);
		}
	}
	UpdateButtons();
}

void VerticalPreviewDock::ToggleReplayBuffer()
{
	if (!output) {
		return;
	}

	if (output->ReplayBufferActive()) {
		output->StopReplayBuffer();
	} else {
		QString error;
		if (!output->StartReplayBuffer(error)) {
			OBSMessageBox::critical(this, QTStr("Output.StartReplayFailed"), error);
		}
	}
	UpdateButtons();
}

void VerticalPreviewDock::Render(void *data, uint32_t cx, uint32_t cy)
{
	VerticalPreviewDock *dock = static_cast<VerticalPreviewDock *>(data);

	OBSCanvas canvas;
	{
		std::lock_guard<std::mutex> lock(dock->canvasMutex);
		canvas = OBSGetStrongRef(dock->renderCanvas);
	}

	obs_video_info ovi;
	if (!canvas || obs_canvas_removed(canvas) || !obs_canvas_get_video_info(canvas, &ovi)) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(int(ovi.base_width), int(ovi.base_height), int(cx), int(cy), x, y, scale);

	int newCX = int(scale * float(ovi.base_width));
	int newCY = int(scale * float(ovi.base_height));

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, newCX, newCY);
	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);

	obs_canvas_render(canvas);

	gs_viewport_pop();
	gs_projection_pop();
}

/* ------------------------------------------------------------------------- */
/* Scenes */

VerticalScenesDock::VerticalScenesDock(VerticalCanvas *vertical_, QWidget *parent)
	: QWidget(parent),
	  vertical(vertical_)
{
	list = new QListWidget(this);
	list->setContextMenuPolicy(Qt::CustomContextMenu);

	QPushButton *addButton = ToolButton("+", QTStr("Vertical.Scenes.Add"), this);
	removeButton = ToolButton(QString::fromUtf8("−"), QTStr("Vertical.Scenes.Remove"), this);
	upButton = ToolButton(QString::fromUtf8("▲"), QTStr("Vertical.MoveUp"), this);
	downButton = ToolButton(QString::fromUtf8("▼"), QTStr("Vertical.MoveDown"), this);

	connect(addButton, &QPushButton::clicked, this, &VerticalScenesDock::AddScene);
	connect(removeButton, &QPushButton::clicked, this, &VerticalScenesDock::RemoveScene);
	connect(upButton, &QPushButton::clicked, this, [this]() {
		if (OBSSource scene = SelectedScene()) {
			vertical->MoveScene(scene, -1);
		}
	});
	connect(downButton, &QPushButton::clicked, this, [this]() {
		if (OBSSource scene = SelectedScene()) {
			vertical->MoveScene(scene, 1);
		}
	});
	connect(list, &QListWidget::currentRowChanged, this, [this]() {
		if (updating) {
			return;
		}
		if (OBSSource scene = SelectedScene()) {
			vertical->SetCurrentScene(scene);
		}
		UpdateSelection();
	});
	connect(list, &QListWidget::itemDoubleClicked, this, &VerticalScenesDock::RenameScene);
	connect(list, &QListWidget::customContextMenuRequested, this, &VerticalScenesDock::ShowContextMenu);

	QHBoxLayout *toolbar = new QHBoxLayout();
	toolbar->addWidget(addButton);
	toolbar->addWidget(removeButton);
	toolbar->addStretch();
	toolbar->addWidget(upButton);
	toolbar->addWidget(downButton);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(list, 1);
	layout->addLayout(toolbar);

	connect(vertical_, &VerticalCanvas::ScenesChanged, this, &VerticalScenesDock::Rebuild);
	connect(vertical_, &VerticalCanvas::CurrentSceneChanged, this, &VerticalScenesDock::Rebuild);
	connect(vertical_, &VerticalCanvas::EnabledChanged, this, &VerticalScenesDock::Rebuild);

	Rebuild();
}

void VerticalScenesDock::Rebuild()
{
	updating = true;
	list->clear();

	OBSSource current = vertical ? vertical->CurrentScene() : OBSSource();
	int currentRow = -1;

	if (vertical) {
		for (const OBSSource &scene : vertical->Scenes()) {
			QListWidgetItem *item =
				new QListWidgetItem(QString::fromUtf8(obs_source_get_name(scene)), list);
			item->setData(Qt::UserRole, QString::fromUtf8(obs_source_get_uuid(scene)));
			if (scene == current) {
				currentRow = list->count() - 1;
			}
		}
	}

	list->setCurrentRow(currentRow);
	updating = false;

	setEnabled(vertical && vertical->Enabled());
	UpdateSelection();
}

void VerticalScenesDock::UpdateSelection()
{
	bool selected = list->currentRow() >= 0;
	removeButton->setEnabled(selected && list->count() > 1);
	upButton->setEnabled(selected && list->currentRow() > 0);
	downButton->setEnabled(selected && list->currentRow() < list->count() - 1);
}

OBSSource VerticalScenesDock::SelectedScene() const
{
	QListWidgetItem *item = list->currentItem();
	if (!item || !vertical) {
		return nullptr;
	}

	QString uuid = item->data(Qt::UserRole).toString();
	for (const OBSSource &scene : vertical->Scenes()) {
		if (uuid == QString::fromUtf8(obs_source_get_uuid(scene))) {
			return scene;
		}
	}
	return nullptr;
}

bool VerticalScenesDock::AskForSceneName(const QString &title, const QString &initial, QString &name)
{
	string input = initial.toStdString();

	for (;;) {
		if (!NameDialog::AskForName(this, title, QTStr("Basic.Main.AddSceneDlg.Text"), input,
					    QString::fromStdString(input))) {
			return false;
		}

		name = QString::fromStdString(input).trimmed();
		if (name.isEmpty()) {
			OBSMessageBox::warning(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}
		if (name != initial && vertical->SceneNameInUse(name)) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}
		return true;
	}
}

void VerticalScenesDock::AddScene()
{
	if (!vertical) {
		return;
	}

	QString name = QTStr("Vertical.DefaultSceneName");
	for (int i = 2; vertical->SceneNameInUse(name); i++) {
		name = QTStr("Vertical.DefaultSceneName") + " " + QString::number(i);
	}

	QString chosen;
	if (!AskForSceneName(QTStr("Basic.Main.AddSceneDlg.Title"), name, chosen)) {
		return;
	}

	OBSSource scene = vertical->AddScene(chosen);
	vertical->SetCurrentScene(scene);
}

void VerticalScenesDock::RemoveScene()
{
	OBSSource scene = SelectedScene();
	if (!scene || list->count() <= 1) {
		return;
	}

	QString text = QTStr("ConfirmRemove.Text").arg(QString::fromUtf8(obs_source_get_name(scene)));
	if (OBSMessageBox::question(this, QTStr("ConfirmRemove.Title"), text) != QMessageBox::Yes) {
		return;
	}

	vertical->RemoveScene(scene);
}

void VerticalScenesDock::RenameScene()
{
	OBSSource scene = SelectedScene();
	if (!scene) {
		return;
	}

	QString oldName = QString::fromUtf8(obs_source_get_name(scene));
	QString name;
	if (!AskForSceneName(QTStr("Rename"), oldName, name) || name == oldName) {
		return;
	}

	obs_source_set_name(scene, name.toUtf8().constData());
	OBSBasic::Get()->SaveProject();
}

void VerticalScenesDock::DuplicateScene()
{
	OBSSource scene = SelectedScene();
	if (!scene) {
		return;
	}

	QString name = QString::fromUtf8(obs_source_get_name(scene)) + " 2";
	for (int i = 3; vertical->SceneNameInUse(name); i++) {
		name = QString::fromUtf8(obs_source_get_name(scene)) + " " + QString::number(i);
	}

	QString chosen;
	if (!AskForSceneName(QTStr("Basic.Main.AddSceneDlg.Title"), name, chosen)) {
		return;
	}

	OBSSource copy = vertical->DuplicateScene(scene, chosen);
	if (copy) {
		vertical->SetCurrentScene(copy);
	}
}

void VerticalScenesDock::ShowContextMenu(const QPoint &pos)
{
	QMenu menu(this);
	menu.addAction(QTStr("Vertical.Scenes.Add"), this, &VerticalScenesDock::AddScene);

	if (list->itemAt(pos)) {
		menu.addAction(QTStr("Duplicate"), this, &VerticalScenesDock::DuplicateScene);
		menu.addAction(QTStr("Rename"), this, &VerticalScenesDock::RenameScene);
		QAction *remove = menu.addAction(QTStr("Remove"), this, &VerticalScenesDock::RemoveScene);
		remove->setEnabled(list->count() > 1);
		menu.addSeparator();
		menu.addAction(QTStr("Filters"), this, [this]() {
			if (OBSSource scene = SelectedScene()) {
				OBSBasic::Get()->CreateFiltersWindow(scene);
			}
		});
	}

	menu.exec(list->mapToGlobal(pos));
}

/* ------------------------------------------------------------------------- */
/* Sources */

VerticalSourcesDock::VerticalSourcesDock(VerticalCanvas *vertical_, QWidget *parent)
	: QWidget(parent),
	  vertical(vertical_)
{
	list = new QListWidget(this);
	list->setContextMenuPolicy(Qt::CustomContextMenu);

	emptyLabel = new QLabel(QTStr("Vertical.Sources.Empty"), this);
	emptyLabel->setWordWrap(true);
	emptyLabel->setAlignment(Qt::AlignCenter);

	addButton = ToolButton("+", QTStr("Vertical.Sources.Add"), this);
	addMenu = new QMenu(this);
	connect(addMenu, &QMenu::aboutToShow, this, &VerticalSourcesDock::BuildAddMenu);
	addButton->setMenu(addMenu);

	removeButton = ToolButton(QString::fromUtf8("−"), QTStr("Vertical.Sources.Remove"), this);
	upButton = ToolButton(QString::fromUtf8("▲"), QTStr("Vertical.MoveUp"), this);
	downButton = ToolButton(QString::fromUtf8("▼"), QTStr("Vertical.MoveDown"), this);
	propertiesButton = new QPushButton(QTStr("Properties"), this);
	filtersButton = new QPushButton(QTStr("Filters"), this);
	transformButton = new QPushButton(QTStr("Vertical.Sources.Transform"), this);
	transformButton->setMenu(CreateTransformMenu(transformButton));

	connect(removeButton, &QPushButton::clicked, this, &VerticalSourcesDock::RemoveItem);
	connect(upButton, &QPushButton::clicked, this, [this]() { MoveItem(OBS_ORDER_MOVE_UP); });
	connect(downButton, &QPushButton::clicked, this, [this]() { MoveItem(OBS_ORDER_MOVE_DOWN); });
	connect(propertiesButton, &QPushButton::clicked, this, [this]() {
		if (OBSSceneItem item = SelectedItem()) {
			OBSBasic::Get()->CreatePropertiesWindow(obs_sceneitem_get_source(item));
		}
	});
	connect(filtersButton, &QPushButton::clicked, this, [this]() {
		if (OBSSceneItem item = SelectedItem()) {
			OBSBasic::Get()->CreateFiltersWindow(obs_sceneitem_get_source(item));
		}
	});
	connect(list, &QListWidget::currentRowChanged, this, &VerticalSourcesDock::UpdateButtons);
	connect(list, &QListWidget::itemChanged, this, &VerticalSourcesDock::ItemChanged);
	connect(list, &QListWidget::itemDoubleClicked, this, [this]() {
		if (OBSSceneItem item = SelectedItem()) {
			OBSBasic::Get()->CreatePropertiesWindow(obs_sceneitem_get_source(item));
		}
	});
	connect(list, &QListWidget::customContextMenuRequested, this, &VerticalSourcesDock::ShowContextMenu);

	QHBoxLayout *toolbar = new QHBoxLayout();
	toolbar->addWidget(addButton);
	toolbar->addWidget(removeButton);
	toolbar->addWidget(propertiesButton);
	toolbar->addWidget(filtersButton);
	toolbar->addWidget(transformButton);
	toolbar->addStretch();
	toolbar->addWidget(upButton);
	toolbar->addWidget(downButton);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(list, 1);
	layout->addWidget(emptyLabel);
	layout->addLayout(toolbar);

	connect(vertical_, &VerticalCanvas::CurrentSceneChanged, this, &VerticalSourcesDock::WatchCurrentScene);
	connect(vertical_, &VerticalCanvas::ScenesChanged, this, &VerticalSourcesDock::WatchCurrentScene);
	connect(vertical_, &VerticalCanvas::EnabledChanged, this, &VerticalSourcesDock::WatchCurrentScene);

	WatchCurrentScene();
}

obs_scene_t *VerticalSourcesDock::CurrentScene() const
{
	OBSSource source = OBSGetStrongRef(watchedScene);
	return source ? obs_scene_from_source(source) : nullptr;
}

void VerticalSourcesDock::WatchCurrentScene()
{
	OBSSource scene = vertical ? vertical->CurrentScene() : OBSSource();

	if (scene != OBSGetStrongRef(watchedScene)) {
		sceneSignals.clear();
		watchedScene = OBSGetWeakRef(scene);

		if (scene) {
			signal_handler_t *handler = obs_source_get_signal_handler(scene);
			for (const char *signal : {"item_add", "item_remove", "reorder", "refresh", "item_visible"}) {
				sceneSignals.emplace_back(handler, signal, SceneChanged, this);
			}
		}
	}

	Rebuild();
}

void VerticalSourcesDock::SceneChanged(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<VerticalSourcesDock *>(data), &VerticalSourcesDock::Rebuild,
				  Qt::QueuedConnection);
}

void VerticalSourcesDock::Rebuild()
{
	int64_t selectedId = 0;
	if (OBSSceneItem selected = SelectedItem()) {
		selectedId = obs_sceneitem_get_id(selected);
	}

	updating = true;
	list->clear();

	obs_scene_t *scene = CurrentScene();
	if (scene) {
		vector<OBSSceneItem> items;
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		};
		obs_scene_enum_items(scene, cb, &items);

		/* The top-most item is listed first, like the main Sources dock */
		OBSBasic *main = OBSBasic::Get();
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_source_t *source = obs_sceneitem_get_source(*it);
			QListWidgetItem *item = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(source)));
			item->setIcon(main->GetSourceIcon(obs_source_get_id(source)));
			item->setData(Qt::UserRole, qlonglong(obs_sceneitem_get_id(*it)));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(obs_sceneitem_visible(*it) ? Qt::Checked : Qt::Unchecked);
			item->setToolTip(QTStr("Vertical.Sources.VisibleTooltip"));
			list->addItem(item);

			if (selectedId && obs_sceneitem_get_id(*it) == selectedId) {
				list->setCurrentItem(item);
			}
		}
	}

	updating = false;

	bool enabled = vertical && vertical->Enabled() && scene;
	setEnabled(enabled);
	list->setVisible(list->count() > 0);
	emptyLabel->setVisible(list->count() == 0);
	UpdateButtons();
}

void VerticalSourcesDock::UpdateButtons()
{
	int row = list->currentRow();
	bool selected = row >= 0;

	removeButton->setEnabled(selected);
	propertiesButton->setEnabled(selected);
	filtersButton->setEnabled(selected);
	transformButton->setEnabled(selected);
	upButton->setEnabled(selected && row > 0);
	downButton->setEnabled(selected && row < list->count() - 1);
	addButton->setEnabled(!!CurrentScene());
}

OBSSceneItem VerticalSourcesDock::SelectedItem() const
{
	QListWidgetItem *item = list->currentItem();
	obs_scene_t *scene = CurrentScene();
	if (!item || !scene) {
		return nullptr;
	}
	return obs_scene_find_sceneitem_by_id(scene, item->data(Qt::UserRole).toLongLong());
}

void VerticalSourcesDock::BuildAddMenu()
{
	addMenu->clear();

	/* New source of any type */
	QMenu *newMenu = addMenu->addMenu(QTStr("Vertical.Sources.AddNew"));
	vector<pair<QString, string>> types;
	const char *id;
	const char *unversionedId;
	for (size_t i = 0; obs_enum_input_types2(i, &id, &unversionedId); i++) {
		uint32_t flags = obs_get_source_output_flags(id);
		if (flags & (OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DEPRECATED)) {
			continue;
		}
		const char *name = obs_source_get_display_name(id);
		if (name && *name) {
			types.emplace_back(QString::fromUtf8(name), id);
		}
	}
	sort(types.begin(), types.end(),
	     [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });

	OBSBasic *main = OBSBasic::Get();
	for (const auto &type : types) {
		string typeId = type.second;
		QAction *action = newMenu->addAction(main->GetSourceIcon(typeId.c_str()), type.first);
		connect(action, &QAction::triggered, this, [this, typeId]() { AddNew(typeId.c_str()); });
	}

	/* Existing sources from the main canvas */
	QMenu *existingMenu = addMenu->addMenu(QTStr("Vertical.Sources.AddExisting"));
	vector<OBSSource> sources;
	auto sourceCb = [](void *param, obs_source_t *source) {
		if (obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT) {
			static_cast<vector<OBSSource> *>(param)->emplace_back(source);
		}
		return true;
	};
	obs_enum_sources(sourceCb, &sources);
	sort(sources.begin(), sources.end(), [](const OBSSource &a, const OBSSource &b) {
		return QString::fromUtf8(obs_source_get_name(a))
			       .localeAwareCompare(QString::fromUtf8(obs_source_get_name(b))) < 0;
	});
	for (const OBSSource &source : sources) {
		QAction *action = existingMenu->addAction(main->GetSourceIcon(obs_source_get_id(source)),
							  QString::fromUtf8(obs_source_get_name(source)));
		OBSWeakSource weak = OBSGetWeakRef(source);
		connect(action, &QAction::triggered, this, [this, weak]() {
			if (OBSSource strong = OBSGetStrongRef(weak)) {
				AddExisting(strong);
			}
		});
	}
	existingMenu->setEnabled(!sources.empty());

	/* Scenes of the main canvas, nested as a source */
	QMenu *scenesMenu = addMenu->addMenu(QTStr("Vertical.Sources.AddMainScene"));
	vector<OBSSource> scenes;
	auto sceneCb = [](void *param, obs_source_t *source) {
		static_cast<vector<OBSSource> *>(param)->emplace_back(source);
		return true;
	};
	obs_enum_scenes(sceneCb, &scenes);
	for (const OBSSource &scene : scenes) {
		if (obs_source_is_group(scene)) {
			continue;
		}
		QAction *action = scenesMenu->addAction(QString::fromUtf8(obs_source_get_name(scene)));
		OBSWeakSource weak = OBSGetWeakRef(scene);
		connect(action, &QAction::triggered, this, [this, weak]() {
			if (OBSSource strong = OBSGetStrongRef(weak)) {
				AddExisting(strong);
			}
		});
	}
	scenesMenu->setEnabled(!scenes.empty());
}

void VerticalSourcesDock::AddExisting(obs_source_t *source)
{
	obs_scene_t *scene = CurrentScene();
	if (!scene || !source) {
		return;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		ApplyTransform(item, Transform::Fit);
	}
	OBSBasic::Get()->SaveProject();
}

void VerticalSourcesDock::AddNew(const char *id)
{
	obs_scene_t *scene = CurrentScene();
	if (!scene) {
		return;
	}

	QString base = QString::fromUtf8(obs_source_get_display_name(id));
	QString suggestion = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(suggestion.toUtf8().constData());
		if (!existing) {
			break;
		}
		suggestion = base + " " + QString::number(i);
	}

	string name = suggestion.toStdString();
	for (;;) {
		if (!NameDialog::AskForName(this, QTStr("Vertical.Sources.AddNew"),
					    QTStr("Vertical.Sources.NamePrompt"), name, QString::fromStdString(name))) {
			return;
		}
		QString trimmed = QString::fromStdString(name).trimmed();
		if (trimmed.isEmpty()) {
			OBSMessageBox::warning(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}
		name = trimmed.toStdString();
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.c_str());
		if (existing) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}
		break;
	}

	OBSSourceAutoRelease source = obs_source_create(id, name.c_str(), nullptr, nullptr);
	if (!source) {
		return;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		ApplyTransform(item, Transform::Fit);
	}
	OBSBasic::Get()->SaveProject();
	OBSBasic::Get()->CreatePropertiesWindow(source);
}

void VerticalSourcesDock::RemoveItem()
{
	OBSSceneItem item = SelectedItem();
	if (!item) {
		return;
	}

	QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
	if (OBSMessageBox::question(this, QTStr("ConfirmRemove.Title"), QTStr("ConfirmRemove.Text").arg(name)) !=
	    QMessageBox::Yes) {
		return;
	}

	obs_sceneitem_remove(item);
	OBSBasic::Get()->SaveProject();
}

void VerticalSourcesDock::MoveItem(obs_order_movement movement)
{
	if (OBSSceneItem item = SelectedItem()) {
		obs_sceneitem_set_order(item, movement);
		OBSBasic::Get()->SaveProject();
	}
}

void VerticalSourcesDock::ApplyTransform(obs_sceneitem_t *item, Transform transform)
{
	if (!vertical) {
		return;
	}

	obs_video_info ovi;
	if (!vertical->GetVideoInfo(ovi)) {
		const VerticalVideoSettings &video = vertical->VideoSettings();
		ovi.base_width = video.baseWidth;
		ovi.base_height = video.baseHeight;
	}

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);

	vec2_set(&info.pos, 0.0f, 0.0f);
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.rot = 0.0f;
	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.crop_to_bounds = false;
	vec2_set(&info.bounds, float(ovi.base_width), float(ovi.base_height));

	switch (transform) {
	case Transform::Fit:
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		break;
	case Transform::Fill:
		info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
		info.crop_to_bounds = true;
		break;
	case Transform::Stretch:
		info.bounds_type = OBS_BOUNDS_STRETCH;
		break;
	case Transform::Center: {
		info.bounds_type = OBS_BOUNDS_NONE;
		obs_source_t *source = obs_sceneitem_get_source(item);
		float cx = float(obs_source_get_width(source));
		float cy = float(obs_source_get_height(source));
		vec2_set(&info.pos, (float(ovi.base_width) - cx) / 2.0f, (float(ovi.base_height) - cy) / 2.0f);
		break;
	}
	case Transform::Reset:
		info.bounds_type = OBS_BOUNDS_NONE;
		vec2_set(&info.bounds, 0.0f, 0.0f);
		break;
	}

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	if (transform == Transform::Reset) {
		obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(item, &crop);
	}
	obs_sceneitem_defer_update_end(item);

	OBSBasic::Get()->SaveProject();
}

QMenu *VerticalSourcesDock::CreateTransformMenu(QWidget *parent)
{
	QMenu *menu = new QMenu(parent);
	auto add = [this, menu](const char *text, Transform transform) {
		menu->addAction(QTStr(text), this, [this, transform]() {
			if (OBSSceneItem item = SelectedItem()) {
				ApplyTransform(item, transform);
			}
		});
	};
	add("Vertical.Transform.Fit", Transform::Fit);
	add("Vertical.Transform.Fill", Transform::Fill);
	add("Vertical.Transform.Stretch", Transform::Stretch);
	add("Vertical.Transform.Center", Transform::Center);
	menu->addSeparator();
	add("Vertical.Transform.Reset", Transform::Reset);
	return menu;
}

void VerticalSourcesDock::ItemChanged(QListWidgetItem *listItem)
{
	if (updating || !listItem) {
		return;
	}

	obs_scene_t *scene = CurrentScene();
	if (!scene) {
		return;
	}

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, listItem->data(Qt::UserRole).toLongLong());
	if (item) {
		bool visible = listItem->checkState() == Qt::Checked;
		if (obs_sceneitem_visible(item) != visible) {
			obs_sceneitem_set_visible(item, visible);
			OBSBasic::Get()->SaveProject();
		}
	}
}

void VerticalSourcesDock::ShowContextMenu(const QPoint &pos)
{
	QMenu menu(this);

	if (list->itemAt(pos)) {
		menu.addAction(QTStr("Properties"), this, [this]() {
			if (OBSSceneItem item = SelectedItem()) {
				OBSBasic::Get()->CreatePropertiesWindow(obs_sceneitem_get_source(item));
			}
		});
		menu.addAction(QTStr("Filters"), this, [this]() {
			if (OBSSceneItem item = SelectedItem()) {
				OBSBasic::Get()->CreateFiltersWindow(obs_sceneitem_get_source(item));
			}
		});
		QMenu *transformMenu = CreateTransformMenu(&menu);
		transformMenu->setTitle(QTStr("Vertical.Sources.Transform"));
		menu.addMenu(transformMenu);
		menu.addSeparator();
		menu.addAction(QTStr("Remove"), this, &VerticalSourcesDock::RemoveItem);
		menu.addSeparator();
	}

	BuildAddMenu();
	for (QAction *action : addMenu->actions()) {
		menu.addAction(action);
	}

	menu.exec(list->mapToGlobal(pos));
}

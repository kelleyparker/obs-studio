#include "OBSBasic.hpp"

#include <dialogs/OBSCanvasSettings.hpp>
#include <docks/CanvasDocks.hpp>
#include <docks/OBSDock.hpp>
#include <utility/MultistreamOutput.hpp>

#include <qt-wrappers.hpp>

#include <QMenu>
#include <QStringList>

void OBSBasic::CreateCanvasSlots()
{
	/* The docks exist from the start, so their layout can be restored;
	 * their widgets are added by SetupCanvasSlots() */
	for (const CanvasSlotId &id : CanvasSlotId::All()) {
		auto slot = std::make_unique<ExtraCanvasSlot>();
		slot->id = id;

		auto createDock = [&](const char *dock, const char *title) {
			OBSDock *widget = new OBSDock();
			widget->setObjectName(id.DockName(dock));
			widget->setWindowTitle(QTStr(title).arg(id.DisplayName()));
			addDockWidget(Qt::RightDockWidgetArea, widget);
			widget->setVisible(false);
			return widget;
		};
		slot->previewDock = createDock("Preview", "Canvas.Dock.Preview");
		slot->scenesDock = createDock("Scenes", "Canvas.Dock.Scenes");
		slot->sourcesDock = createDock("Sources", "Canvas.Dock.Sources");

		canvasSlots.push_back(std::move(slot));
	}
}

void OBSBasic::SetupCanvasSlots()
{
	canvasMenu = new QMenu(QTStr("Canvas.Menu"), this);
	ui->menuTools->insertMenu(ui->idianPlayground, canvasMenu);

	for (const auto &entry : canvasSlots) {
		ExtraCanvasSlot *slot = entry.get();
		ExtraCanvas *canvas = slot->canvas.get();
		CanvasOutput *output = slot->output.get();

		CanvasPreviewDock *preview = new CanvasPreviewDock(canvas, output, slot->previewDock);
		slot->previewDock->setWidget(preview);
		connect(preview, &CanvasPreviewDock::OpenSettings, this, [this, slot]() { OpenCanvasSettings(slot); });
		slot->scenesDock->setWidget(new CanvasScenesDock(canvas, slot->scenesDock));
		slot->sourcesDock->setWidget(new CanvasSourcesDock(canvas, slot->sourcesDock));

		/* Horizontal canvases first, then vertical ones */
		if (slot->id == CanvasSlotId{CanvasKind::Vertical, 1}) {
			canvasMenu->addSeparator();
		}
		slot->menuAction = canvasMenu->addAction(QTStr("Canvas.MenuAction").arg(slot->id.DisplayName()));
		slot->menuAction->setMenuRole(QAction::NoRole);
		slot->menuAction->setCheckable(true);
		connect(slot->menuAction, &QAction::triggered, this, [this, slot]() { OpenCanvasSettings(slot); });
		connect(canvas, &ExtraCanvas::EnabledChanged, this, &OBSBasic::UpdateCanvasMenu);

		/* Docks > lists only the docks of canvases that are turned on */
		auto updateDockActions = [slot]() {
			bool enabled = slot->canvas && slot->canvas->Enabled();
			for (QDockWidget *dock :
			     {slot->previewDock.data(), slot->scenesDock.data(), slot->sourcesDock.data()}) {
				if (dock) {
					dock->toggleViewAction()->setVisible(enabled);
				}
			}
		};
		connect(canvas, &ExtraCanvas::EnabledChanged, this, updateDockActions);
		updateDockActions();

		const std::string key = slot->id.Key();
		connect(canvas, &ExtraCanvas::CanvasClearing, multistream.get(),
			[this, key]() { multistream->StopCanvas(key, true); });

		connect(output, &CanvasOutput::RecordingStarted, this, &OBSBasic::CanvasOutputStarted);
		connect(output, &CanvasOutput::ReplayBufferStarted, this, &OBSBasic::CanvasOutputStarted);
		connect(output, &CanvasOutput::RecordingStopped, this, [this, slot]() {
			slot->startedWithMain = false;
			CanvasOutputStopped();
		});
		connect(output, &CanvasOutput::ReplayBufferStopped, this, &OBSBasic::CanvasOutputStopped);
	}

	UpdateCanvasMenu();
}

void OBSBasic::UpdateCanvasMenu()
{
	for (const auto &slot : canvasSlots) {
		if (slot->menuAction) {
			slot->menuAction->setChecked(slot->canvas && slot->canvas->Enabled());
		}
	}
}

ExtraCanvasSlot *OBSBasic::FindCanvasSlot(const std::string &key) const
{
	for (const auto &slot : canvasSlots) {
		if (slot->id.Key() == key) {
			return slot.get();
		}
	}
	return nullptr;
}

bool OBSBasic::CanvasOutputsActive() const
{
	for (const auto &slot : canvasSlots) {
		if (slot->output && slot->output->Active()) {
			return true;
		}
	}
	return false;
}

void OBSBasic::OpenCanvasSettings(ExtraCanvasSlot *slot)
{
	if (!slot || !slot->canvas) {
		return;
	}

	ExtraCanvas *canvas = slot->canvas.get();
	bool wasEnabled = canvas->Enabled();
	bool outputsActive = slot->output && slot->output->Active();

	OBSCanvasSettings dialog(canvas, outputsActive, this);
	int result = dialog.exec();
	UpdateCanvasMenu();
	if (result != QDialog::Accepted) {
		return;
	}

	/* Show the canvas's docks the first time it is turned on */
	if (!wasEnabled && canvas->Enabled()) {
		ShowCanvasDocks(slot);
	}
}

void OBSBasic::ShowCanvasDocks(ExtraCanvasSlot *slot)
{
	for (QDockWidget *dock : {slot->previewDock.data(), slot->scenesDock.data(), slot->sourcesDock.data()}) {
		if (dock && !dock->isVisible()) {
			dock->setVisible(true);
		}
	}
}

void OBSBasic::StartCanvasRecording(ExtraCanvasSlot *slot)
{
	QString error;
	if (slot->output && !slot->output->StartRecording(error) && !error.isEmpty()) {
		OBSMessageBox::critical(this, QTStr("Output.StartRecordingFailed"), error);
	}
}

void OBSBasic::StopCanvasRecording(ExtraCanvasSlot *slot)
{
	if (slot->output) {
		slot->output->StopRecording();
	}
}

void OBSBasic::StartCanvasReplayBuffer(ExtraCanvasSlot *slot)
{
	QString error;
	if (slot->output && !slot->output->StartReplayBuffer(error)) {
		OBSMessageBox::critical(this, QTStr("Output.StartReplayFailed"), error);
	}
}

void OBSBasic::StopCanvasReplayBuffer(ExtraCanvasSlot *slot)
{
	if (slot->output) {
		slot->output->StopReplayBuffer();
	}
}

void OBSBasic::SaveCanvasReplay(ExtraCanvasSlot *slot)
{
	if (slot->output) {
		slot->output->SaveReplay();
	}
}

void OBSBasic::StartCanvasesWithMainRecording()
{
	QStringList failed;

	for (const auto &slot : canvasSlots) {
		ExtraCanvas *canvas = slot->canvas.get();
		if (!canvas || !slot->output || !canvas->Enabled() || !canvas->RecordWithMain() ||
		    slot->output->RecordingActive()) {
			continue;
		}

		QString error;
		if (slot->output->StartRecording(error)) {
			slot->startedWithMain = true;
		} else if (error.isEmpty()) {
			/* The user has already been told why */
			failed << slot->id.DisplayName();
		} else {
			failed << QString("%1: %2").arg(slot->id.DisplayName(), error);
		}
	}

	/* The main recording keeps running */
	if (!failed.isEmpty()) {
		OBSMessageBox::warning(this, QTStr("Output.StartRecordingFailed"),
				       QTStr("Canvas.Error.StartWithMain") + "\n\n" + failed.join("\n"));
	}
}

void OBSBasic::StopCanvasesWithMainRecording()
{
	/* Canvases the user started themselves keep recording */
	for (const auto &slot : canvasSlots) {
		if (slot->startedWithMain) {
			slot->startedWithMain = false;
			StopCanvasRecording(slot.get());
		}
	}
}

void OBSBasic::CanvasOutputStarted()
{
	OnActivate();
}

void OBSBasic::CanvasOutputStopped()
{
	OnDeactivate();
}

#include "OBSBasic.hpp"

#include <dialogs/OBSVerticalSettings.hpp>

#include <qt-wrappers.hpp>

void OBSBasic::OpenVerticalSettings()
{
	if (!vertical) {
		return;
	}

	bool wasEnabled = vertical->Enabled();
	bool outputsActive = verticalOutput && verticalOutput->Active();

	OBSVerticalSettings dialog(vertical.get(), outputsActive, this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	/* Show the vertical docks the first time vertical is turned on */
	if (!wasEnabled && vertical->Enabled()) {
		ShowVerticalDocks();
	}
}

void OBSBasic::ShowVerticalDocks()
{
	for (QDockWidget *dock : {verticalPreviewDock.data(), verticalScenesDock.data(), verticalSourcesDock.data()}) {
		if (dock && !dock->isVisible()) {
			dock->setVisible(true);
		}
	}
}

void OBSBasic::StartVerticalRecording()
{
	QString error;
	if (verticalOutput && !verticalOutput->StartRecording(error) && !error.isEmpty()) {
		OBSMessageBox::critical(this, QTStr("Output.StartRecordingFailed"), error);
	}
}

void OBSBasic::StopVerticalRecording()
{
	if (verticalOutput) {
		verticalOutput->StopRecording();
	}
}

void OBSBasic::StartVerticalReplayBuffer()
{
	QString error;
	if (verticalOutput && !verticalOutput->StartReplayBuffer(error)) {
		OBSMessageBox::critical(this, QTStr("Output.StartReplayFailed"), error);
	}
}

void OBSBasic::StopVerticalReplayBuffer()
{
	if (verticalOutput) {
		verticalOutput->StopReplayBuffer();
	}
}

void OBSBasic::SaveVerticalReplay()
{
	if (verticalOutput) {
		verticalOutput->SaveReplay();
	}
}

void OBSBasic::VerticalOutputStarted()
{
	OnActivate();
}

void OBSBasic::VerticalOutputStopped()
{
	OnDeactivate();
}

#include "OBSBasic.hpp"

#include <dialogs/OBSSettingsBackup.hpp>

extern bool restart;

/* Writes everything to disk so a backup or restore sees the current state */
void OBSBasic::SaveForBackup()
{
	SaveProjectNow();
	config_save_safe(activeConfiguration, "tmp", nullptr);
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

void OBSBasic::OpenBackupSettings()
{
	OBSBackupSettingsDialog dialog(this);
	dialog.exec();
}

void OBSBasic::OpenRestoreSettings()
{
	OBSRestoreSettingsDialog dialog(this);
	dialog.exec();

	if (dialog.RestartRequested()) {
		restart = true;
		close();
	}
}

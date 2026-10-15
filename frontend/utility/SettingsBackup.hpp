#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/* Backup and restore of the OBS configuration (profiles, scene collections,
 * app settings, themes and plugin settings) as a single zip file.
 *
 * Restoring happens in two steps because OBS keeps its profile and scene
 * collection open while running: the backup is first extracted into a
 * staging folder, and the staged files are moved into place on the next
 * start, before any configuration is loaded. */

namespace SettingsBackup {

enum Category : uint32_t {
	Profiles = 1 << 0,
	SceneCollections = 1 << 1,
	AppSettings = 1 << 2,
	Themes = 1 << 3,
	PluginSettings = 1 << 4,
	AllCategories = Profiles | SceneCollections | AppSettings | Themes | PluginSettings,
};

enum class RestoreMode {
	Replace,
	Merge,
};

/* Folders that hold each part of the configuration */
struct Roots {
	std::filesystem::path user;
	std::filesystem::path profiles;
	std::filesystem::path scenes;
	std::filesystem::path themes;
	std::filesystem::path pluginConfig;
	std::filesystem::path pluginManager;

	static Roots Current();
	static Roots Inside(const std::filesystem::path &dir);
};

struct BackupOptions {
	uint32_t categories = AllCategories;
	bool includeSecrets = true;
};

struct BackupResult {
	bool success = false;
	std::string error;
	size_t files = 0;
	uint64_t bytes = 0;
	std::vector<std::string> skipped;
};

struct NamedItem {
	std::string file;
	std::string name;
};

struct BackupInfo {
	int format = 0;
	std::string obsVersion;
	std::string created;
	std::string os;
	uint32_t categories = 0;
	bool includesSecrets = true;
	size_t files = 0;
	std::vector<NamedItem> profiles;
	std::vector<NamedItem> collections;
};

using ProgressCallback = std::function<void(size_t done, size_t total)>;

/* Folder that holds the app's own files (backups, staged restores) */
std::filesystem::path AppConfigDir();

BackupResult CreateBackup(const std::filesystem::path &file, const BackupOptions &options,
			  const ProgressCallback &progress = nullptr);

bool InspectBackup(const std::filesystem::path &file, BackupInfo &info, std::string &error);

/* Saves a safety backup of the current configuration, then extracts the
 * selected parts of the backup into the staging folder. */
bool StageRestore(const std::filesystem::path &file, uint32_t categories, RestoreMode mode, std::string &error);

bool HasPendingRestore();
void CancelPendingRestore();

/* Moves a staged restore into place. Must run before any user configuration
 * is opened. */
void ApplyPendingRestore(const Roots &roots);

/* Automatic backup on exit, configured in the user config "Backup" section */
std::filesystem::path DefaultBackupFolder();
std::string DefaultBackupFileName();
void RunAutomaticBackup();

} // namespace SettingsBackup

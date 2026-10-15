#include "SettingsBackup.hpp"
#include "ZipArchive.hpp"

#include <OBSApp.hpp>

#include <QDateTime>
#include <QStandardPaths>
#include <QSysInfo>

#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace std;
namespace fs = std::filesystem;

namespace SettingsBackup {

namespace {

constexpr int BackupFormat = 1;
constexpr const char *ManifestName = "manifest.json";
constexpr const char *StagingDirName = ".restore-pending";
constexpr const char *RestoreFileName = "restore.json";
constexpr const char *SafetyPrefix = "pre-restore-";
constexpr const char *AutoPrefix = "OBS Auto Backup ";
constexpr size_t SafetyBackupsToKeep = 5;
constexpr uintmax_t MaxFileSize = 50ull * 1024 * 1024;

struct Area {
	Category category;
	const char *prefix;
	fs::path Roots::*root;
};

const Area Areas[] = {
	{Profiles, "profiles/", &Roots::profiles},
	{SceneCollections, "scenes/", &Roots::scenes},
	{AppSettings, "user/", &Roots::user},
	{Themes, "themes/", &Roots::themes},
	{PluginSettings, "plugin_config/", &Roots::pluginConfig},
	{PluginSettings, "plugin_manager/", &Roots::pluginManager},
};

const char *CategoryName(Category category)
{
	switch (category) {
	case Profiles:
		return "profiles";
	case SceneCollections:
		return "scene_collections";
	case AppSettings:
		return "app_settings";
	case Themes:
		return "themes";
	case PluginSettings:
		return "plugin_settings";
	default:
		return "";
	}
}

/* Browser caches and crash dumps are large and rebuilt automatically */
const set<string> ExcludedFolders = {
	"Cache",        "Code Cache",  "GPUCache",          "GrShaderCache",
	"ShaderCache",  "DawnCache",   "DawnGraphiteCache", "DawnWebGPUCache",
	"CacheStorage", "ScriptCache", "Crashpad",          "blob_storage",
};

bool HasSuffix(const string &str, const string &suffix)
{
	return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const string &str, const string &prefix)
{
	return str.compare(0, prefix.size(), prefix) == 0;
}

bool IsTemporaryFile(const string &name)
{
	/* .DS_Store files are folder view settings that macOS's Finder creates */
	return HasSuffix(name, ".bak") || HasSuffix(name, ".tmp") || HasSuffix(name, ".lock") || name == ".DS_Store";
}

bool ReadFile(const fs::path &path, string &data)
{
	ifstream in(path, ios::binary);
	if (!in.is_open()) {
		return false;
	}

	ostringstream buf;
	buf << in.rdbuf();
	if (in.bad()) {
		return false;
	}

	data = buf.str();
	return true;
}

bool WriteFile(const fs::path &path, const string &data)
{
	error_code ec;
	fs::create_directories(path.parent_path(), ec);

	ofstream out(path, ios::binary | ios::trunc);
	if (!out.is_open()) {
		return false;
	}

	out.write(data.data(), streamsize(data.size()));
	out.close();
	return !out.fail();
}

time_t FileTime(const fs::path &path)
{
	error_code ec;
	auto writeTime = fs::last_write_time(path, ec);
	if (ec) {
		return time(nullptr);
	}

	auto sys = chrono::time_point_cast<chrono::system_clock::duration>(
		writeTime - fs::file_time_type::clock::now() + chrono::system_clock::now());
	return chrono::system_clock::to_time_t(sys);
}

string Timestamp(const char *format)
{
	return QDateTime::currentDateTime().toString(format).toStdString();
}

/* Replaces characters Windows does not allow in file names */
string SafeFileName(const string &name)
{
	string result;
	for (char c : name) {
		if (strchr("<>:\"/\\|?*", c) || uint8_t(c) < 32) {
			result.push_back('_');
		} else {
			result.push_back(c);
		}
	}
	while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
		result.pop_back();
	}
	return result.empty() ? "Restored" : result;
}

/* ------------------------------------------------------------------------- */
/* Config and JSON helpers                                                   */

string IniValue(const string &ini, const char *section, const char *key)
{
	config_t *config = nullptr;
	if (config_open_string(&config, ini.c_str()) != CONFIG_SUCCESS) {
		return "";
	}

	const char *value = config_get_string(config, section, key);
	string result = value ? value : "";
	config_close(config);
	return result;
}

/* libobs can only save configs to files, so edits go through a temp file */
bool EditIni(string &ini, const function<void(config_t *)> &edit)
{
	error_code ec;
	fs::path temp = fs::temp_directory_path(ec) / ("obs-backup-" + to_string(os_gettime_ns()) + ".ini");
	if (ec || !WriteFile(temp, ini)) {
		return false;
	}

	bool success = false;
	config_t *config = nullptr;
	if (config_open(&config, temp.u8string().c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS) {
		edit(config);
		success = config_save(config) == CONFIG_SUCCESS && ReadFile(temp, ini);
		config_close(config);
	}

	fs::remove(temp, ec);
	return success;
}

string JsonName(const string &json)
{
	OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
	const char *name = data ? obs_data_get_string(data, "name") : "";
	return name ? name : "";
}

bool EditJson(string &json, const function<void(obs_data_t *)> &edit)
{
	OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
	if (!data) {
		return false;
	}

	edit(data);
	const char *result = obs_data_get_json_pretty(data);
	if (!result) {
		return false;
	}

	json = result;
	return true;
}

/* Removes stream keys, passwords and login tokens */
void RemoveSecrets(const string &entry, string &data)
{
	if (StartsWith(entry, "profiles/") && HasSuffix(entry, "/service.json")) {
		EditJson(data, [](obs_data_t *json) {
			OBSDataAutoRelease settings = obs_data_get_obj(json, "settings");
			if (!settings) {
				return;
			}
			for (const char *key : {"key", "password", "bearer_token"}) {
				if (obs_data_has_user_value(settings, key)) {
					obs_data_set_string(settings, key, "");
				}
			}
		});
	} else if (StartsWith(entry, "profiles/") && HasSuffix(entry, "/multistream.json")) {
		EditJson(data, [](obs_data_t *json) {
			OBSDataArrayAutoRelease destinations = obs_data_get_array(json, "destinations");
			size_t count = obs_data_array_count(destinations);
			for (size_t i = 0; i < count; i++) {
				OBSDataAutoRelease dest = obs_data_array_item(destinations, i);
				obs_data_set_string(dest, "key", "");
			}
		});
	} else if (StartsWith(entry, "profiles/") && HasSuffix(entry, "/basic.ini")) {
		/* Logins of integrated services (Twitch, YouTube, Restream) */
		EditIni(data, [](config_t *config) {
			size_t count = config_num_sections(config);
			for (size_t i = 0; i < count; i++) {
				const char *section = config_get_section(config, i);
				for (const char *key : {"Token", "RefreshToken", "ExpireTime"}) {
					config_remove_value(config, section, key);
				}
			}
		});
	} else if (entry == "plugin_config/obs-websocket/config.json") {
		EditJson(data, [](obs_data_t *json) {
			obs_data_set_string(json, "server_password", "");
			obs_data_set_bool(json, "auth_required", false);
		});
	}
}

/* ------------------------------------------------------------------------- */
/* Collecting files                                                          */

struct SourceFile {
	string entry;
	fs::path path;
};

void CollectFolder(const fs::path &root, const string &prefix, vector<SourceFile> &files, BackupResult &result)
{
	error_code ec;
	if (!fs::is_directory(root, ec)) {
		return;
	}

	auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const fs::directory_entry &item = *it;
		string name = item.path().filename().u8string();

		if (item.is_directory(ec)) {
			if (ExcludedFolders.count(name) || name == StagingDirName) {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (!item.is_regular_file(ec) || IsTemporaryFile(name)) {
			continue;
		}

		string relative = fs::relative(item.path(), root, ec).generic_u8string();
		if (ec || relative.empty()) {
			continue;
		}

		string entry = prefix + relative;
		if (!IsSafeZipEntryName(entry)) {
			result.skipped.push_back(entry + " (unsupported file name)");
			continue;
		}
		if (item.file_size(ec) > MaxFileSize) {
			result.skipped.push_back(entry + " (larger than 50 MB)");
			continue;
		}

		files.push_back({entry, item.path()});
	}
}

vector<SourceFile> CollectFiles(const Roots &roots, uint32_t categories, BackupResult &result)
{
	vector<SourceFile> files;
	error_code ec;

	if (categories & Profiles) {
		CollectFolder(roots.profiles, "profiles/", files, result);
	}

	if ((categories & SceneCollections) && fs::is_directory(roots.scenes, ec)) {
		for (const auto &item : fs::directory_iterator(roots.scenes, ec)) {
			string name = item.path().filename().u8string();
			if (item.is_regular_file(ec) && HasSuffix(name, ".json") &&
			    IsSafeZipEntryName("scenes/" + name)) {
				files.push_back({"scenes/" + name, item.path()});
			}
		}
	}

	if (categories & AppSettings) {
		fs::path userIni = roots.user / "user.ini";
		if (fs::is_regular_file(userIni, ec)) {
			files.push_back({"user/user.ini", userIni});
		}
	}

	if (categories & Themes) {
		CollectFolder(roots.themes, "themes/", files, result);
	}

	if (categories & PluginSettings) {
		CollectFolder(roots.pluginConfig, "plugin_config/", files, result);
		CollectFolder(roots.pluginManager, "plugin_manager/", files, result);
	}

	return files;
}

/* Lists profiles (folder and name) and scene collections (file and name)
 * from a set of zip entries */
void DescribeEntries(ZipReader &zip, BackupInfo &info)
{
	for (const ZipEntry &entry : zip.Entries()) {
		string data;

		if (StartsWith(entry.name, "profiles/") && HasSuffix(entry.name, "/basic.ini") &&
		    count(entry.name.begin(), entry.name.end(), '/') == 2) {
			string dir = entry.name.substr(9, entry.name.size() - 9 - 10);
			string name = zip.Read(entry, data) ? IniValue(data, "General", "Name") : "";
			info.profiles.push_back({dir, name.empty() ? dir : name});

		} else if (StartsWith(entry.name, "scenes/") && HasSuffix(entry.name, ".json") &&
			   count(entry.name.begin(), entry.name.end(), '/') == 1) {
			string file = entry.name.substr(7);
			string name = zip.Read(entry, data) ? JsonName(data) : "";
			info.collections.push_back({file, name.empty() ? file.substr(0, file.size() - 5) : name});
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Existing configuration                                                    */

map<string, string> ExistingProfiles(const fs::path &root)
{
	map<string, string> profiles; /* name -> folder */
	error_code ec;

	for (const auto &item : fs::directory_iterator(root, ec)) {
		if (!item.is_directory(ec)) {
			continue;
		}

		string dir = item.path().filename().u8string();
		string ini;
		string name = ReadFile(item.path() / "basic.ini", ini) ? IniValue(ini, "General", "Name") : "";
		profiles[name.empty() ? dir : name] = dir;
	}

	return profiles;
}

map<string, string> ExistingCollections(const fs::path &root)
{
	map<string, string> collections; /* name -> file */
	error_code ec;

	for (const auto &item : fs::directory_iterator(root, ec)) {
		string file = item.path().filename().u8string();
		if (!item.is_regular_file(ec) || !HasSuffix(file, ".json")) {
			continue;
		}

		OBSDataAutoRelease data = obs_data_create_from_json_file(item.path().u8string().c_str());
		const char *name = data ? obs_data_get_string(data, "name") : "";
		collections[(name && *name) ? name : file.substr(0, file.size() - 5)] = file;
	}

	return collections;
}

/* Finds a "Name (Restored)" style name and file name that are both free */
pair<string, string> UniqueName(const string &name, const set<string> &usedNames, const set<string> &usedFiles,
				const string &extension)
{
	for (int i = 1;; i++) {
		string candidate = name + " (Restored" + (i > 1 ? " " + to_string(i) : string()) + ")";
		string file = SafeFileName(candidate) + extension;
		if (!usedNames.count(candidate) && !usedFiles.count(file)) {
			return {candidate, file};
		}
	}
}

bool CopyFolder(const fs::path &from, const fs::path &to, string &error)
{
	error_code ec;
	if (!fs::exists(from, ec)) {
		return true;
	}

	fs::create_directories(to, ec);
	fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "Could not copy " + from.u8string() + " to " + to.u8string() + ": " + ec.message();
		return false;
	}
	return true;
}

bool ClearFolder(const fs::path &dir, string &error)
{
	error_code ec;
	if (!fs::exists(dir, ec)) {
		return true;
	}

	for (const auto &item : fs::directory_iterator(dir, ec)) {
		fs::remove_all(item.path(), ec);
		if (ec) {
			error = "Could not remove " + item.path().u8string() + ": " + ec.message();
			return false;
		}
	}
	return true;
}

/* Points the user config at a profile and scene collection that exist */
void FixCurrentSelection(const Roots &roots)
{
	fs::path userIni = roots.user / "user.ini";
	ConfigFile config;
	if (config.Open(userIni.u8string().c_str(), CONFIG_OPEN_EXISTING) != CONFIG_SUCCESS) {
		return;
	}

	bool changed = false;

	map<string, string> profiles = ExistingProfiles(roots.profiles);
	const char *profile = config_get_string(config, "Basic", "Profile");
	if (!profiles.empty() && (!profile || !profiles.count(profile))) {
		config_set_string(config, "Basic", "Profile", profiles.begin()->first.c_str());
		config_set_string(config, "Basic", "ProfileDir", profiles.begin()->second.c_str());
		changed = true;
	}

	map<string, string> collections = ExistingCollections(roots.scenes);
	const char *collection = config_get_string(config, "Basic", "SceneCollection");
	if (!collections.empty() && (!collection || !collections.count(collection))) {
		config_set_string(config, "Basic", "SceneCollection", collections.begin()->first.c_str());
		config_set_string(config, "Basic", "SceneCollectionFile", collections.begin()->second.c_str());
		changed = true;
	}

	if (changed) {
		config.SaveSafe("tmp");
		blog(LOG_INFO, "Settings restore: switched to profile/scene collection that exists after restore");
	}
}

/* Extracts the selected categories of a backup into the given folders.
 * With merge, existing profiles, collections and files are never touched. */
bool Extract(ZipReader &zip, const Roots &target, const Roots &current, uint32_t categories, RestoreMode mode,
	     string &error)
{
	map<string, string> profileDirs;     /* backup folder -> new folder */
	map<string, string> profileNames;    /* new folder -> new name */
	map<string, string> collectionFiles; /* backup file -> new file */
	map<string, string> collectionNames; /* new file -> new name */

	if (mode == RestoreMode::Merge) {
		BackupInfo info;
		DescribeEntries(zip, info);

		map<string, string> profiles = ExistingProfiles(current.profiles);
		set<string> usedNames, usedDirs;
		for (const auto &[name, dir] : profiles) {
			usedNames.insert(name);
			usedDirs.insert(dir);
		}
		for (const NamedItem &profile : info.profiles) {
			if (usedNames.count(profile.name) || usedDirs.count(profile.file)) {
				auto [name, dir] = UniqueName(profile.name, usedNames, usedDirs, "");
				profileDirs[profile.file] = dir;
				profileNames[dir] = name;
				usedNames.insert(name);
				usedDirs.insert(dir);
			} else {
				usedNames.insert(profile.name);
				usedDirs.insert(profile.file);
			}
		}

		map<string, string> collections = ExistingCollections(current.scenes);
		usedNames.clear();
		set<string> usedFiles;
		for (const auto &[name, file] : collections) {
			usedNames.insert(name);
			usedFiles.insert(file);
		}
		for (const NamedItem &collection : info.collections) {
			if (usedNames.count(collection.name) || usedFiles.count(collection.file)) {
				auto [name, file] = UniqueName(collection.name, usedNames, usedFiles, ".json");
				collectionFiles[collection.file] = file;
				collectionNames[file] = name;
				usedNames.insert(name);
				usedFiles.insert(file);
			} else {
				usedNames.insert(collection.name);
				usedFiles.insert(collection.file);
			}
		}
	}

	for (const ZipEntry &entry : zip.Entries()) {
		const Area *area = nullptr;
		for (const Area &candidate : Areas) {
			if (StartsWith(entry.name, candidate.prefix)) {
				area = &candidate;
				break;
			}
		}
		if (!area || !(categories & area->category)) {
			continue;
		}

		/* App settings cannot be merged, the current ones are kept */
		if (mode == RestoreMode::Merge && area->category == AppSettings) {
			continue;
		}

		string relative = entry.name.substr(strlen(area->prefix));
		string newName;

		if (mode == RestoreMode::Merge && area->category == Profiles) {
			size_t slash = relative.find('/');
			string dir = relative.substr(0, slash);
			auto renamed = profileDirs.find(dir);
			if (renamed != profileDirs.end()) {
				relative = renamed->second + relative.substr(slash);
				if (relative == renamed->second + "/basic.ini") {
					newName = profileNames[renamed->second];
				}
			}
		} else if (mode == RestoreMode::Merge && area->category == SceneCollections) {
			auto renamed = collectionFiles.find(relative);
			if (renamed != collectionFiles.end()) {
				relative = renamed->second;
				newName = collectionNames[relative];
			}
		}

		fs::path destination = target.*(area->root) / fs::u8path(relative);

		if (mode == RestoreMode::Merge && (area->category == Themes || area->category == PluginSettings)) {
			error_code ec;
			if (fs::exists(current.*(area->root) / fs::u8path(relative), ec)) {
				continue;
			}
		}

		string data;
		if (!zip.Read(entry, data)) {
			error = zip.Error();
			return false;
		}

		if (!newName.empty() && area->category == Profiles) {
			EditIni(data, [&](config_t *config) {
				config_set_string(config, "General", "Name", newName.c_str());
			});
		} else if (!newName.empty() && area->category == SceneCollections) {
			EditJson(data, [&](obs_data_t *json) { obs_data_set_string(json, "name", newName.c_str()); });
		}

		if (!WriteFile(destination, data)) {
			error = "Could not write " + destination.u8string();
			return false;
		}
	}

	return true;
}

void PruneBackups(const fs::path &folder, const string &prefix, size_t keep)
{
	vector<fs::path> backups;
	error_code ec;

	for (const auto &item : fs::directory_iterator(folder, ec)) {
		string name = item.path().filename().u8string();
		if (item.is_regular_file(ec) && StartsWith(name, prefix) && HasSuffix(name, ".zip")) {
			backups.push_back(item.path());
		}
	}

	/* Names contain a sortable timestamp, newest last */
	sort(backups.begin(), backups.end());
	while (backups.size() > keep) {
		fs::remove(backups.front(), ec);
		backups.erase(backups.begin());
	}
}

fs::path StagingDir()
{
	return AppConfigDir() / StagingDirName;
}

} // namespace

/* ------------------------------------------------------------------------- */

fs::path AppConfigDir()
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "obs-studio") <= 0) {
		return {};
	}
	return fs::u8path(path);
}

Roots Roots::Current()
{
	OBSApp *app = App();
	fs::path appDir = AppConfigDir();

	Roots roots;
	roots.user = app->userConfigLocation / "obs-studio";
	roots.profiles = app->userProfilesLocation / "obs-studio/basic/profiles";
	roots.scenes = app->userScenesLocation / "obs-studio/basic/scenes";
	roots.themes = appDir / "themes";
	roots.pluginConfig = appDir / "plugin_config";
	roots.pluginManager = app->userPluginManagerSettingsLocation / "obs-studio/plugin_manager";
	return roots;
}

Roots Roots::Inside(const fs::path &dir)
{
	Roots roots;
	roots.user = dir / "user";
	roots.profiles = dir / "profiles";
	roots.scenes = dir / "scenes";
	roots.themes = dir / "themes";
	roots.pluginConfig = dir / "plugin_config";
	roots.pluginManager = dir / "plugin_manager";
	return roots;
}

BackupResult CreateBackup(const fs::path &file, const BackupOptions &options, const ProgressCallback &progress)
{
	BackupResult result;
	vector<SourceFile> files = CollectFiles(Roots::Current(), options.categories, result);

	ZipWriter zip;
	if (!zip.Open(file)) {
		result.error = zip.Error();
		return result;
	}

	for (size_t i = 0; i < files.size(); i++) {
		if (progress) {
			progress(i, files.size());
		}

		string data;
		if (!ReadFile(files[i].path, data)) {
			/* Files kept open by OBS or a plugin (e.g. browser cookies) */
			result.skipped.push_back(files[i].entry + " (could not be read)");
			continue;
		}

		if (!options.includeSecrets) {
			RemoveSecrets(files[i].entry, data);
		}

		if (!zip.AddFile(files[i].entry, data, FileTime(files[i].path))) {
			result.error = zip.Error();
			zip.Abort();
			return result;
		}

		result.files++;
		result.bytes += data.size();
	}

	OBSDataAutoRelease manifest = obs_data_create();
	obs_data_set_int(manifest, "format", BackupFormat);
	obs_data_set_string(manifest, "obs_version", obs_get_version_string());
	obs_data_set_string(manifest, "created", Timestamp("yyyy-MM-dd'T'HH:mm:ss").c_str());
	obs_data_set_string(manifest, "os", QSysInfo::prettyProductName().toUtf8().constData());
	obs_data_set_bool(manifest, "portable", App()->IsPortableMode());
	obs_data_set_bool(manifest, "includes_secrets", options.includeSecrets);
	obs_data_set_int(manifest, "files", int64_t(result.files));

	OBSDataArrayAutoRelease categories = obs_data_array_create();
	for (Category category : {Profiles, SceneCollections, AppSettings, Themes, PluginSettings}) {
		if (!(options.categories & category)) {
			continue;
		}
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", CategoryName(category));
		obs_data_array_push_back(categories, item);
	}
	obs_data_set_array(manifest, "categories", categories);

	if (!zip.AddFile(ManifestName, obs_data_get_json_pretty(manifest), time(nullptr)) || !zip.Close()) {
		result.error = zip.Error();
		zip.Abort();
		return result;
	}

	if (progress) {
		progress(files.size(), files.size());
	}

	blog(LOG_INFO, "Settings backup: saved %zu files (%llu bytes) to %s, skipped %zu", result.files,
	     (unsigned long long)result.bytes, file.u8string().c_str(), result.skipped.size());
	for (const string &skipped : result.skipped) {
		blog(LOG_INFO, "Settings backup: skipped %s", skipped.c_str());
	}

	result.success = true;
	return result;
}

bool InspectBackup(const fs::path &file, BackupInfo &info, string &error)
{
	ZipReader zip;
	if (!zip.Open(file)) {
		error = zip.Error();
		return false;
	}

	const ZipEntry *manifestEntry = zip.Find(ManifestName);
	string manifestJson;
	if (!manifestEntry || !zip.Read(*manifestEntry, manifestJson)) {
		error = "This file is not an OBS settings backup";
		return false;
	}

	OBSDataAutoRelease manifest = obs_data_create_from_json(manifestJson.c_str());
	if (!manifest || obs_data_get_int(manifest, "format") < 1) {
		error = "This file is not an OBS settings backup";
		return false;
	}
	if (obs_data_get_int(manifest, "format") > BackupFormat) {
		error = "This backup was made by a newer version of OBS";
		return false;
	}

	info = {};
	info.format = int(obs_data_get_int(manifest, "format"));
	info.obsVersion = obs_data_get_string(manifest, "obs_version");
	info.created = obs_data_get_string(manifest, "created");
	info.os = obs_data_get_string(manifest, "os");
	info.includesSecrets = obs_data_get_bool(manifest, "includes_secrets");
	info.files = size_t(obs_data_get_int(manifest, "files"));

	for (const ZipEntry &entry : zip.Entries()) {
		for (const Area &area : Areas) {
			if (StartsWith(entry.name, area.prefix)) {
				info.categories |= area.category;
			}
		}
	}

	DescribeEntries(zip, info);
	return true;
}

bool StageRestore(const fs::path &file, uint32_t categories, RestoreMode mode, string &error)
{
	ZipReader zip;
	if (!zip.Open(file)) {
		error = zip.Error();
		return false;
	}

	/* A safety backup of everything, so the restore can be undone */
	fs::path backupDir = AppConfigDir() / "backups";
	error_code ec;
	fs::create_directories(backupDir, ec);

	fs::path safetyBackup = backupDir / (SafetyPrefix + Timestamp("yyyy-MM-dd_HHmmss") + ".zip");
	BackupResult safety = CreateBackup(safetyBackup, {AllCategories, true});
	if (!safety.success) {
		error = "Could not save a backup of the current settings: " + safety.error;
		return false;
	}
	PruneBackups(backupDir, SafetyPrefix, SafetyBackupsToKeep);

	fs::path staging = StagingDir();
	fs::remove_all(staging, ec);

	if (!Extract(zip, Roots::Inside(staging / "files"), Roots::Current(), categories, mode, error)) {
		fs::remove_all(staging, ec);
		return false;
	}

	OBSDataAutoRelease restore = obs_data_create();
	obs_data_set_string(restore, "mode", mode == RestoreMode::Replace ? "replace" : "merge");
	obs_data_set_int(restore, "categories", categories);
	obs_data_set_string(restore, "source", file.u8string().c_str());
	obs_data_set_string(restore, "safety_backup", safetyBackup.u8string().c_str());

	if (!WriteFile(staging / RestoreFileName, obs_data_get_json_pretty(restore))) {
		error = "Could not write " + (staging / RestoreFileName).u8string();
		fs::remove_all(staging, ec);
		return false;
	}

	blog(LOG_INFO, "Settings restore: staged %s (%s), applied on next start", file.u8string().c_str(),
	     mode == RestoreMode::Replace ? "replace" : "merge");
	return true;
}

bool HasPendingRestore()
{
	error_code ec;
	return fs::exists(StagingDir() / RestoreFileName, ec);
}

void CancelPendingRestore()
{
	error_code ec;
	fs::remove_all(StagingDir(), ec);
}

void ApplyPendingRestore(const Roots &roots)
{
	fs::path staging = StagingDir();
	error_code ec;
	if (!fs::exists(staging / RestoreFileName, ec)) {
		return;
	}

	OBSDataAutoRelease restore = obs_data_create_from_json_file((staging / RestoreFileName).u8string().c_str());
	if (!restore) {
		blog(LOG_ERROR, "Settings restore: %s is unreadable, restore skipped", RestoreFileName);
		fs::remove_all(staging, ec);
		return;
	}

	bool replace = strcmp(obs_data_get_string(restore, "mode"), "replace") == 0;
	uint32_t categories = uint32_t(obs_data_get_int(restore, "categories"));
	fs::path safetyBackup = fs::u8path(obs_data_get_string(restore, "safety_backup"));
	Roots staged = Roots::Inside(staging / "files");

	blog(LOG_INFO, "Settings restore: applying %s restore from %s", replace ? "replace" : "merge",
	     obs_data_get_string(restore, "source"));

	string error;
	bool success = true;

	for (const Area &area : Areas) {
		if (!(categories & area.category)) {
			continue;
		}

		/* Replacing profiles and scene collections removes the ones that
		 * are not in the backup, but only if the backup has any */
		bool clear = replace && (area.category == Profiles || area.category == SceneCollections) &&
			     !fs::is_empty(staged.*(area.root), ec) && !ec;

		if ((clear && !ClearFolder(roots.*(area.root), error)) ||
		    !CopyFolder(staged.*(area.root), roots.*(area.root), error)) {
			success = false;
			break;
		}
	}

	if (!success) {
		blog(LOG_ERROR, "Settings restore failed: %s", error.c_str());

		ZipReader zip;
		string rollbackError;
		if (zip.Open(safetyBackup) && ClearFolder(roots.profiles, rollbackError) &&
		    ClearFolder(roots.scenes, rollbackError) &&
		    Extract(zip, roots, roots, AllCategories, RestoreMode::Replace, rollbackError)) {
			blog(LOG_INFO, "Settings restore: previous settings put back from %s",
			     safetyBackup.u8string().c_str());
		} else {
			blog(LOG_ERROR,
			     "Settings restore: could not put back previous settings (%s), they are saved in %s",
			     rollbackError.empty() ? zip.Error().c_str() : rollbackError.c_str(),
			     safetyBackup.u8string().c_str());
		}
	} else {
		blog(LOG_INFO, "Settings restore: done");
	}

	FixCurrentSelection(roots);
	fs::remove_all(staging, ec);
}

/* ------------------------------------------------------------------------- */

fs::path DefaultBackupFolder()
{
	QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	return fs::u8path(documents.toStdString()) / "OBS Backups";
}

string DefaultBackupFileName()
{
	return "OBS Backup " + Timestamp("yyyy-MM-dd HHmm") + ".zip";
}

void RunAutomaticBackup()
{
	config_t *config = App()->GetUserConfig();
	if (!config_get_bool(config, "Backup", "AutoBackupOnExit")) {
		return;
	}

	const char *folderSetting = config_get_string(config, "Backup", "AutoBackupFolder");
	fs::path folder = (folderSetting && *folderSetting) ? fs::u8path(folderSetting) : DefaultBackupFolder();
	int keep = max(1, int(config_get_int(config, "Backup", "AutoBackupKeep")));

	error_code ec;
	fs::create_directories(folder, ec);

	fs::path file = folder / (AutoPrefix + Timestamp("yyyy-MM-dd HHmmss") + ".zip");
	BackupResult result = CreateBackup(file, {AllCategories, true});
	if (!result.success) {
		blog(LOG_WARNING, "Automatic settings backup failed: %s", result.error.c_str());
		return;
	}

	PruneBackups(folder, AutoPrefix, size_t(keep));
}

} // namespace SettingsBackup

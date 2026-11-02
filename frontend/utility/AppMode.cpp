#include "AppMode.hpp"

#include <OBSApp.hpp>

#include <util/config-file.h>
#include <util/dstr.h>

#include <string>

/* The mode named on the command line, or empty */
extern std::string opt_app_mode;

static const AppMode allModes[] = {AppMode::Vanilla, AppMode::YouTube, AppMode::Cliffgoat};

static bool ModeFromKey(const char *key, AppMode &mode)
{
	for (AppMode m : allModes) {
		if (key && astrcmpi(key, AppModeKey(m)) == 0) {
			mode = m;
			return true;
		}
	}
	return false;
}

AppMode CurrentAppMode()
{
	static const AppMode mode = []() {
		config_t *cfg = App() ? App()->GetUserConfig() : nullptr;
		AppMode m = AppMode::YouTube;
		if (ModeFromKey(opt_app_mode.c_str(), m)) {
			if (cfg) {
				config_set_string(cfg, "General", "AppMode", AppModeKey(m));
			}
			return m;
		}
		if (cfg) {
			ModeFromKey(config_get_string(cfg, "General", "AppMode"), m);
		}
		return m;
	}();
	return mode;
}

const char *AppModeKey(AppMode mode)
{
	switch (mode) {
	case AppMode::Vanilla:
		return "vanilla";
	case AppMode::Cliffgoat:
		return "cliffgoat";
	case AppMode::YouTube:
		break;
	}
	return "youtube";
}

QString AppModeName(AppMode mode)
{
	switch (mode) {
	case AppMode::Vanilla:
		return QStringLiteral("Vanilla");
	case AppMode::Cliffgoat:
		return QStringLiteral("cliffgoat");
	case AppMode::YouTube:
		break;
	}
	return QStringLiteral("YouTube");
}

const char *AppModeDefaultTheme(AppMode mode)
{
	switch (mode) {
	case AppMode::Vanilla:
		/* What OBS Studio opens with out of the box */
		return "com.obsproject.Yami.Original";
	case AppMode::Cliffgoat:
		return "com.obsproject.Themes.CliffgoatGold.Default";
	case AppMode::YouTube:
		break;
	}
	return "com.obsproject.Themes.FiretruckRed.Default";
}

const char *AppModeDockStateKey()
{
	switch (CurrentAppMode()) {
	case AppMode::Vanilla:
		return "VanillaDockState";
	case AppMode::Cliffgoat:
		return "CliffgoatDockState";
	case AppMode::YouTube:
		break;
	}
	/* YouTube mode keeps the layout saved before there were modes */
	return "DockState";
}

const char *AppModeThemeKey()
{
	switch (CurrentAppMode()) {
	case AppMode::Vanilla:
		return "VanillaTheme";
	case AppMode::Cliffgoat:
		return "CliffgoatTheme";
	case AppMode::YouTube:
		break;
	}
	return "YouTubeTheme";
}

bool YouTubePipelineEnabled()
{
	return CurrentAppMode() != AppMode::Vanilla;
}

#pragma once

#include <QString>

/* OBS opens in one of three modes, picked with --vanilla, --youtube or
 * --cliffgoat and remembered; without a flag it opens in the last one.
 *
 * Vanilla leaves out the YouTube pipeline (its dock, the ▶ YT button, the
 * YouTube Automation settings page and its hotkeys). YouTube and cliffgoat
 * have it. Each mode keeps its own dock layout and theme. The mode is fixed
 * for the session; switching restarts OBS. */
enum class AppMode { Vanilla, YouTube, Cliffgoat };

AppMode CurrentAppMode();

/* "vanilla", "youtube" or "cliffgoat": the command-line flag without "--" */
const char *AppModeKey(AppMode mode);
/* "Vanilla", "YouTube" or "cliffgoat", for menus and the title bar */
QString AppModeName(AppMode mode);
/* The theme a mode opens with until another is picked in Settings */
const char *AppModeDefaultTheme(AppMode mode);
/* The user config keys (in BasicWindow and Appearance) for the current
 * mode's dock layout and theme */
const char *AppModeDockStateKey();
const char *AppModeThemeKey();

bool YouTubePipelineEnabled();

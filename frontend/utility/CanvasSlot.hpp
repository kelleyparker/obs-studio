#pragma once

#include <QString>

#include <string>
#include <vector>

enum class CanvasKind {
	Horizontal,
	Vertical,
};

/* Identifies one of the extra canvases next to the main canvas: Horizontal
 * 2-4 and Vertical 1-4 (the main canvas counts as Horizontal 1).
 *
 * Vertical 1 keeps every name the single vertical canvas used before (profile
 * section, scene collection key, hotkeys, dock names, multistream canvas), so
 * existing profiles and scene collections carry over unchanged. */
struct CanvasSlotId {
	CanvasKind kind = CanvasKind::Vertical;
	int number = 1;

	static constexpr int MaxPerKind = 4;

	/* Every extra slot, in menu order */
	static std::vector<CanvasSlotId> All();
	/* Parses Key(); returns false for "main" or anything unknown */
	static bool FromKey(const std::string &key, CanvasSlotId &id);

	bool operator==(const CanvasSlotId &other) const { return kind == other.kind && number == other.number; }

	bool IsVertical() const { return kind == CanvasKind::Vertical; }
	bool IsLegacyVertical() const { return kind == CanvasKind::Vertical && number == 1; }

	/* "horizontal2", "vertical1" */
	std::string Key() const;
	/* Localized, e.g. "Horizontal 2" */
	QString DisplayName() const;
	/* libobs canvas name, also used in log messages */
	std::string CanvasName() const;
	/* Profile (basic.ini) section */
	std::string ConfigSection() const;
	/* Key of this canvas's data in the scene collection */
	std::string CollectionKey() const;
	/* e.g. HotkeyName("Start", "Recording") */
	std::string HotkeyName(const char *verb, const char *noun) const;
	/* Object name of the dock, e.g. DockName("Preview") */
	QString DockName(const char *dock) const;
	/* Default filename suffix of recordings */
	std::string DefaultSuffix() const;
	/* Unique prefix for encoder and output names */
	std::string OutputPrefix() const;
};

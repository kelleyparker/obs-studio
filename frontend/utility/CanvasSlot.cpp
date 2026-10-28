#include "CanvasSlot.hpp"

#include <OBSApp.hpp>

using namespace std;

vector<CanvasSlotId> CanvasSlotId::All()
{
	vector<CanvasSlotId> ids;
	for (int i = 2; i <= MaxPerKind; i++) {
		ids.push_back({CanvasKind::Horizontal, i});
	}
	for (int i = 1; i <= MaxPerKind; i++) {
		ids.push_back({CanvasKind::Vertical, i});
	}
	return ids;
}

bool CanvasSlotId::FromKey(const string &key, CanvasSlotId &id)
{
	for (const CanvasSlotId &slot : All()) {
		if (slot.Key() == key) {
			id = slot;
			return true;
		}
	}
	return false;
}

string CanvasSlotId::Key() const
{
	return (IsVertical() ? "vertical" : "horizontal") + to_string(number);
}

QString CanvasSlotId::DisplayName() const
{
	return QTStr(IsVertical() ? "Canvas.Name.Vertical" : "Canvas.Name.Horizontal").arg(number);
}

string CanvasSlotId::CanvasName() const
{
	if (IsLegacyVertical()) {
		return "Vertical";
	}
	return (IsVertical() ? "Vertical " : "Horizontal ") + to_string(number);
}

string CanvasSlotId::ConfigSection() const
{
	return IsLegacyVertical() ? "Vertical" : "Canvas." + Key();
}

string CanvasSlotId::CollectionKey() const
{
	return IsLegacyVertical() ? "vertical_canvas" : "canvas_" + Key();
}

string CanvasSlotId::HotkeyName(const char *verb, const char *noun) const
{
	if (IsLegacyVertical()) {
		return string("OBSBasic.") + verb + "Vertical" + noun;
	}
	return string("OBSBasic.") + verb + "Canvas" + noun + "." + Key();
}

QString CanvasSlotId::DockName(const char *dock) const
{
	if (IsLegacyVertical()) {
		return QString("vertical%1Dock").arg(dock);
	}
	return QString("canvas%1Dock_%2").arg(dock, QString::fromStdString(Key()));
}

string CanvasSlotId::DefaultSuffix() const
{
	if (IsLegacyVertical()) {
		return "Vertical";
	}
	return (IsVertical() ? "Vertical" : "Horizontal") + to_string(number);
}

string CanvasSlotId::OutputPrefix() const
{
	return IsLegacyVertical() ? "vertical" : "canvas_" + Key();
}

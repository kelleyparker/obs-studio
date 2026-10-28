#include "ExtraCanvas.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <util/config-file.h>
#include <util/dstr.h>

#include <algorithm>
#include <cstring>

using namespace std;

static const char *ExtraCanvasName = "Vertical";
static const char *VerticalDataKey = "vertical_canvas";

static obs_scale_type ScaleTypeFromString(const char *scaleType)
{
	if (astrcmpi(scaleType, "bilinear") == 0) {
		return OBS_SCALE_BILINEAR;
	} else if (astrcmpi(scaleType, "lanczos") == 0) {
		return OBS_SCALE_LANCZOS;
	} else if (astrcmpi(scaleType, "area") == 0) {
		return OBS_SCALE_AREA;
	}
	return OBS_SCALE_BICUBIC;
}

static const char *ScaleTypeToString(obs_scale_type scaleType)
{
	switch (scaleType) {
	case OBS_SCALE_BILINEAR:
		return "bilinear";
	case OBS_SCALE_LANCZOS:
		return "lanczos";
	case OBS_SCALE_AREA:
		return "area";
	default:
		break;
	}
	return "bicubic";
}

static uint32_t ClampSize(uint64_t size, uint32_t fallback)
{
	if (size < 32 || size > 16384) {
		return fallback;
	}
	/* Encoders need even sizes */
	return uint32_t(size) & ~1u;
}

ExtraCanvas::ExtraCanvas(QObject *parent) : QObject(parent) {}

ExtraCanvas::~ExtraCanvas()
{
	canvasSignals.clear();
}

void ExtraCanvas::LoadProfile()
{
	config_t *config = OBSBasic::Get()->Config();

	config_set_default_bool(config, ConfigSection, "Enabled", false);
	config_set_default_uint(config, ConfigSection, "BaseCX", 1080);
	config_set_default_uint(config, ConfigSection, "BaseCY", 1920);
	config_set_default_uint(config, ConfigSection, "OutputCX", 1080);
	config_set_default_uint(config, ConfigSection, "OutputCY", 1920);
	config_set_default_string(config, ConfigSection, "ScaleType", "bicubic");

	enabled = config_get_bool(config, ConfigSection, "Enabled");
	video.baseWidth = ClampSize(config_get_uint(config, ConfigSection, "BaseCX"), 1080);
	video.baseHeight = ClampSize(config_get_uint(config, ConfigSection, "BaseCY"), 1920);
	video.outputWidth = ClampSize(config_get_uint(config, ConfigSection, "OutputCX"), video.baseWidth);
	video.outputHeight = ClampSize(config_get_uint(config, ConfigSection, "OutputCY"), video.baseHeight);
	video.scaleType = ScaleTypeFromString(config_get_string(config, ConfigSection, "ScaleType"));

	/* Before the first scene collection is loaded there is no canvas yet;
	 * LoadCollection() applies the settings then. */
	if (GetCanvas() && !VideoActive()) {
		if (enabled) {
			EnsureScene();
			ApplyVideo();
		} else if (HasVideo()) {
			emit CanvasClearing();
			obs_canvas_clear_video(GetCanvas());
		}
	} else if (enabled && collectionLoaded) {
		EnsureCanvas();
		EnsureScene();
		ApplyVideo();
	}

	emit EnabledChanged(enabled);
	emit VideoChanged();
}

void ExtraCanvas::SaveProfile() const
{
	config_t *config = OBSBasic::Get()->Config();

	config_set_bool(config, ConfigSection, "Enabled", enabled);
	config_set_uint(config, ConfigSection, "BaseCX", video.baseWidth);
	config_set_uint(config, ConfigSection, "BaseCY", video.baseHeight);
	config_set_uint(config, ConfigSection, "OutputCX", video.outputWidth);
	config_set_uint(config, ConfigSection, "OutputCY", video.outputHeight);
	config_set_string(config, ConfigSection, "ScaleType", ScaleTypeToString(video.scaleType));
	config_save_safe(config, "tmp", nullptr);
}

bool ExtraCanvas::SetEnabled(bool enable)
{
	if (enable == enabled) {
		return true;
	}
	if (VideoActive()) {
		return false;
	}

	enabled = enable;
	SaveProfile();

	if (enabled) {
		EnsureCanvas();
		EnsureScene();
		ApplyVideo();
	} else if (OBSCanvas canvas = GetCanvas()) {
		emit CanvasClearing();
		obs_canvas_clear_video(canvas);
	}

	blog(LOG_INFO, "Vertical canvas %s", enabled ? "enabled" : "disabled");

	emit EnabledChanged(enabled);
	emit VideoChanged();
	return true;
}

bool ExtraCanvas::SetVideoSettings(const CanvasVideoSettings &settings)
{
	if (VideoActive()) {
		return false;
	}

	video = settings;
	video.baseWidth = ClampSize(video.baseWidth, 1080);
	video.baseHeight = ClampSize(video.baseHeight, 1920);
	video.outputWidth = ClampSize(video.outputWidth, video.baseWidth);
	video.outputHeight = ClampSize(video.outputHeight, video.baseHeight);
	SaveProfile();

	if (enabled) {
		ApplyVideo();
	}

	emit VideoChanged();
	return true;
}

void ExtraCanvas::MainVideoReset()
{
	/* obs_reset_video() already recreated the mix with the new frame
	 * rate; this also applies the new color format, space and range. */
	if (enabled && !VideoActive()) {
		ApplyVideo();
	}
}

bool ExtraCanvas::MakeVideoInfo(obs_video_info &ovi) const
{
	if (!obs_get_video_info(&ovi)) {
		return false;
	}

	ovi.base_width = video.baseWidth;
	ovi.base_height = video.baseHeight;
	ovi.output_width = video.outputWidth;
	ovi.output_height = video.outputHeight;
	ovi.scale_type = video.scaleType;
	return true;
}

bool ExtraCanvas::ApplyVideo()
{
	OBSCanvas canvas = GetCanvas();
	obs_video_info ovi;

	if (!canvas || !MakeVideoInfo(ovi)) {
		return false;
	}

	if (!obs_canvas_reset_video(canvas, &ovi)) {
		blog(LOG_WARNING, "Failed to reset vertical canvas video (%ux%u)", ovi.base_width, ovi.base_height);
		return false;
	}

	blog(LOG_INFO, "Vertical canvas video: base %ux%u, output %ux%u", ovi.base_width, ovi.base_height,
	     ovi.output_width, ovi.output_height);

	emit VideoChanged();
	return true;
}

OBSCanvas ExtraCanvas::GetCanvas() const
{
	OBSCanvas canvas = OBSGetStrongRef(weakCanvas);
	if (canvas && obs_canvas_removed(canvas)) {
		return nullptr;
	}
	return canvas;
}

bool ExtraCanvas::HasVideo() const
{
	OBSCanvas canvas = GetCanvas();
	return canvas && obs_canvas_has_video(canvas);
}

video_t *ExtraCanvas::GetVideo() const
{
	OBSCanvas canvas = GetCanvas();
	return canvas ? obs_canvas_get_video(canvas) : nullptr;
}

bool ExtraCanvas::GetVideoInfo(obs_video_info &ovi) const
{
	OBSCanvas canvas = GetCanvas();
	return canvas && obs_canvas_get_video_info(canvas, &ovi);
}

bool ExtraCanvas::VideoActive() const
{
	OBSCanvas canvas = GetCanvas();
	return canvas && obs_canvas_video_active(canvas);
}

OBSCanvas ExtraCanvas::EnsureCanvas()
{
	if (OBSCanvas canvas = GetCanvas()) {
		return canvas;
	}

	/* No MIX_AUDIO: a source used in both canvases would otherwise be
	 * heard twice. Vertical outputs record the same audio mix as the main
	 * outputs. */
	const OBS::Canvas &canvas = OBSBasic::Get()->AddCanvas(ExtraCanvasName, nullptr, ACTIVATE | SCENE_REF);
	AttachCanvas(canvas);
	return GetCanvas();
}

void ExtraCanvas::AttachCanvas(obs_canvas_t *canvas)
{
	DetachCanvas();

	weakCanvas = OBSGetWeakRef(canvas);

	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	canvasSignals.emplace_back(handler, "source_add", CanvasSourceChanged, this);
	canvasSignals.emplace_back(handler, "source_remove", CanvasSourceChanged, this);
	canvasSignals.emplace_back(handler, "source_rename", CanvasSourceChanged, this);
	canvasSignals.emplace_back(handler, "remove", CanvasRemoved, this);

	RefreshScenes();
}

void ExtraCanvas::DetachCanvas()
{
	if (!weakCanvas && sceneOrder.empty()) {
		return;
	}

	canvasSignals.clear();
	weakCanvas = nullptr;
	sceneOrder.clear();

	emit ScenesChanged();
	emit CurrentSceneChanged();
	emit VideoChanged();
}

void ExtraCanvas::CanvasSourceChanged(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<ExtraCanvas *>(data), &ExtraCanvas::RefreshScenes, Qt::QueuedConnection);
}

void ExtraCanvas::CanvasRemoved(void *data, calldata_t *)
{
	ExtraCanvas *self = static_cast<ExtraCanvas *>(data);
	QMetaObject::invokeMethod(
		self,
		[self]() {
			if (self->weakCanvas && !self->GetCanvas()) {
				self->DetachCanvas();
			}
		},
		Qt::QueuedConnection);
}

void ExtraCanvas::RefreshScenes()
{
	OBSCanvas canvas = GetCanvas();
	vector<OBSSource> present;

	if (canvas) {
		auto cb = [](void *param, obs_source_t *source) {
			if (!obs_source_is_group(source) && !obs_source_removed(source)) {
				static_cast<vector<OBSSource> *>(param)->emplace_back(source);
			}
			return true;
		};
		obs_canvas_enum_scenes(canvas, cb, &present);
	}

	vector<OBSWeakSource> newOrder;
	for (const OBSWeakSource &weak : sceneOrder) {
		OBSSource source = OBSGetStrongRef(weak);
		if (source && find(present.begin(), present.end(), source) != present.end()) {
			newOrder.push_back(weak);
		}
	}
	for (const OBSSource &source : present) {
		bool known = any_of(newOrder.begin(), newOrder.end(),
				    [&](const OBSWeakSource &weak) { return OBSGetStrongRef(weak) == source; });
		if (!known) {
			newOrder.push_back(OBSGetWeakRef(source));
		}
	}
	sceneOrder = std::move(newOrder);

	/* The current scene may have been removed */
	if (canvas) {
		OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
		if (current && obs_source_removed(current)) {
			vector<OBSSource> scenes = Scenes();
			obs_canvas_set_channel(canvas, 0, scenes.empty() ? nullptr : scenes.front().Get());
			emit CurrentSceneChanged();
		}
	}

	emit ScenesChanged();
}

vector<OBSSource> ExtraCanvas::Scenes() const
{
	vector<OBSSource> scenes;
	for (const OBSWeakSource &weak : sceneOrder) {
		OBSSource source = OBSGetStrongRef(weak);
		if (source && !obs_source_removed(source)) {
			scenes.push_back(source);
		}
	}
	return scenes;
}

OBSSource ExtraCanvas::CurrentScene() const
{
	OBSCanvas canvas = GetCanvas();
	if (!canvas) {
		return nullptr;
	}
	OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
	return current.Get();
}

void ExtraCanvas::SetCurrentScene(obs_source_t *scene)
{
	OBSCanvas canvas = GetCanvas();
	if (!canvas) {
		return;
	}

	OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
	if (current == scene) {
		return;
	}

	obs_canvas_set_channel(canvas, 0, scene);
	emit CurrentSceneChanged();
	OBSBasic::Get()->SaveProject();
}

bool ExtraCanvas::SceneNameInUse(const QString &name) const
{
	const string utf8 = name.toStdString();

	if (OBSCanvas canvas = GetCanvas()) {
		OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, utf8.c_str());
		if (source) {
			return true;
		}
	}

	OBSSourceAutoRelease source = obs_get_source_by_name(utf8.c_str());
	return !!source;
}

OBSSource ExtraCanvas::AddScene(const QString &name)
{
	OBSCanvas canvas = EnsureCanvas();
	if (!canvas) {
		return nullptr;
	}

	OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, name.toUtf8().constData());
	OBSSource source = obs_scene_get_source(scene);

	RefreshScenes();

	if (!CurrentScene()) {
		SetCurrentScene(source);
	}

	OBSBasic::Get()->SaveProject();
	return source;
}

OBSSource ExtraCanvas::DuplicateScene(obs_source_t *scene, const QString &name)
{
	obs_scene_t *original = obs_scene_from_source(scene);
	if (!original) {
		return nullptr;
	}

	/* The copy is created on the same canvas as the original */
	OBSSceneAutoRelease copy = obs_scene_duplicate(original, name.toUtf8().constData(), OBS_SCENE_DUP_REFS);
	OBSSource source = obs_scene_get_source(copy);

	RefreshScenes();
	OBSBasic::Get()->SaveProject();
	return source;
}

void ExtraCanvas::RemoveScene(obs_source_t *scene)
{
	OBSCanvas canvas = GetCanvas();
	if (!canvas || !scene) {
		return;
	}

	if (CurrentScene() == scene) {
		OBSSource next;
		for (const OBSSource &other : Scenes()) {
			if (other != scene) {
				next = other;
				break;
			}
		}
		obs_canvas_set_channel(canvas, 0, next);
		emit CurrentSceneChanged();
	}

	obs_source_remove(scene);
	RefreshScenes();
	OBSBasic::Get()->SaveProject();
}

void ExtraCanvas::MoveScene(obs_source_t *scene, int offset)
{
	auto it = find_if(sceneOrder.begin(), sceneOrder.end(),
			  [&](const OBSWeakSource &weak) { return OBSGetStrongRef(weak) == scene; });
	if (it == sceneOrder.end()) {
		return;
	}

	int from = int(it - sceneOrder.begin());
	int to = std::clamp(from + offset, 0, int(sceneOrder.size()) - 1);
	if (from == to) {
		return;
	}

	OBSWeakSource weak = *it;
	sceneOrder.erase(it);
	sceneOrder.insert(sceneOrder.begin() + to, weak);

	emit ScenesChanged();
	OBSBasic::Get()->SaveProject();
}

void ExtraCanvas::EnsureScene()
{
	if (!GetCanvas() || !Scenes().empty()) {
		return;
	}

	QString name = QTStr("Vertical.DefaultSceneName");
	for (int i = 2; SceneNameInUse(name); i++) {
		name = QTStr("Vertical.DefaultSceneName") + " " + QString::number(i);
	}

	AddScene(name);
}

void ExtraCanvas::SaveCollection(obs_data_t *saveData) const
{
	OBSCanvas canvas = GetCanvas();
	if (!canvas) {
		return;
	}

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "canvas_uuid", obs_canvas_get_uuid(canvas));

	OBSSource current = CurrentScene();
	obs_data_set_string(data, "current_scene", current ? obs_source_get_name(current) : "");

	OBSDataArrayAutoRelease order = obs_data_array_create();
	for (const OBSSource &scene : Scenes()) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", obs_source_get_name(scene));
		obs_data_array_push_back(order, item);
	}
	obs_data_set_array(data, "scene_order", order);

	obs_data_set_obj(saveData, VerticalDataKey, data);
}

void ExtraCanvas::LoadCollection(obs_data_t *collectionData)
{
	DetachCanvas();
	collectionLoaded = true;

	OBSDataAutoRelease data = collectionData ? obs_data_get_obj(collectionData, VerticalDataKey) : nullptr;

	if (data) {
		const char *uuid = obs_data_get_string(data, "canvas_uuid");
		for (const OBS::Canvas &canvas : OBSBasic::Get()->GetCanvases()) {
			if (strcmp(obs_canvas_get_uuid(canvas), uuid) == 0) {
				AttachCanvas(canvas);
				break;
			}
		}
	}

	if (OBSCanvas canvas = GetCanvas()) {
		/* Restore the saved scene order */
		vector<OBSWeakSource> ordered;
		OBSDataArrayAutoRelease order = obs_data_get_array(data, "scene_order");
		size_t count = obs_data_array_count(order);
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(order, i);
			OBSSourceAutoRelease scene =
				obs_canvas_get_source_by_name(canvas, obs_data_get_string(item, "name"));
			if (scene && obs_source_is_scene(scene)) {
				ordered.push_back(OBSGetWeakRef(scene));
			}
		}
		sceneOrder = std::move(ordered);
		RefreshScenes();

		OBSSourceAutoRelease current =
			obs_canvas_get_source_by_name(canvas, obs_data_get_string(data, "current_scene"));
		vector<OBSSource> scenes = Scenes();
		if (current && obs_source_is_scene(current)) {
			obs_canvas_set_channel(canvas, 0, current);
		} else if (!scenes.empty()) {
			obs_canvas_set_channel(canvas, 0, scenes.front());
		}
	}

	if (enabled) {
		EnsureCanvas();
		EnsureScene();
		ApplyVideo();
	}

	emit ScenesChanged();
	emit CurrentSceneChanged();
	emit VideoChanged();
}

void ExtraCanvas::CollectionCleared()
{
	emit CanvasClearing();

	if (OBSCanvas canvas = GetCanvas()) {
		obs_canvas_set_channel(canvas, 0, nullptr);
	}

	collectionLoaded = false;
	DetachCanvas();
}

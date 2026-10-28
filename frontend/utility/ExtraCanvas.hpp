#pragma once

#include <obs.hpp>
#include <utility/CanvasSlot.hpp>

#include <QObject>
#include <QString>

#include <string>
#include <vector>

struct CanvasVideoSettings {
	uint32_t baseWidth = 1080;
	uint32_t baseHeight = 1920;
	uint32_t outputWidth = 1080;
	uint32_t outputHeight = 1920;
	obs_scale_type scaleType = OBS_SCALE_BICUBIC;
};

/* An extra video mix with its own scenes, next to the main canvas: one of
 * Horizontal 2-4 or Vertical 1-4 (see CanvasSlotId). The canvas itself is one
 * of OBSBasic's canvases, so its scenes are saved with the scene collection.
 * Whether it is enabled and its resolution are profile settings (the slot's
 * section in basic.ini).
 *
 * While it is disabled the canvas (and its scenes) is kept, but it has no
 * video mix, so it costs nothing. */
class ExtraCanvas : public QObject {
	Q_OBJECT

public:
	explicit ExtraCanvas(const CanvasSlotId &slot, QObject *parent = nullptr);
	~ExtraCanvas();

	const CanvasSlotId &Slot() const { return slot; }
	const char *ConfigSection() const { return section.c_str(); }

	/* Sets the slot's defaults in the active profile and reads the
	 * profile's settings. Call on startup and after a profile switch. */
	void LoadProfile();

	bool Enabled() const { return enabled; }
	/* Saves the setting to the profile and creates or removes the video mix.
	 * Fails (returns false) while this canvas's outputs are active. */
	bool SetEnabled(bool enable);

	const CanvasVideoSettings &VideoSettings() const { return video; }
	/* Saves the settings to the profile and resets the video mix. Fails
	 * (returns false) while this canvas's outputs are active. */
	bool SetVideoSettings(const CanvasVideoSettings &settings);

	/* Whether recording starts and stops with the main recording */
	bool RecordWithMain() const { return recordWithMain; }
	void SetRecordWithMain(bool enable);

	/* Re-applies the main canvas's frame rate and color settings, e.g.
	 * after Settings > Video changed them. */
	void MainVideoReset();

	/* The canvas, or nullptr if the scene collection has none yet */
	OBSCanvas GetCanvas() const;
	/* True when the canvas exists and has a video mix */
	bool HasVideo() const;
	video_t *GetVideo() const;
	bool GetVideoInfo(obs_video_info &ovi) const;
	bool VideoActive() const;

	/* Scenes in the user's order */
	std::vector<OBSSource> Scenes() const;
	OBSSource CurrentScene() const;
	void SetCurrentScene(obs_source_t *scene);
	OBSSource AddScene(const QString &name);
	OBSSource DuplicateScene(obs_source_t *scene, const QString &name);
	void RemoveScene(obs_source_t *scene);
	void MoveScene(obs_source_t *scene, int offset);
	bool SceneNameInUse(const QString &name) const;

	/* Scene collection hooks, called by OBSBasic */
	void SaveCollection(obs_data_t *saveData) const;
	/* data is nullptr for a new, empty scene collection */
	void LoadCollection(obs_data_t *data);
	/* Called before the scene collection's canvases are removed */
	void CollectionCleared();

signals:
	void EnabledChanged(bool enabled);
	void VideoChanged();
	void ScenesChanged();
	void CurrentSceneChanged();
	/* Sent before the canvas or its video mix is removed, so that outputs
	 * using it can be stopped first */
	void CanvasClearing();

private:
	CanvasSlotId slot;
	std::string section;
	bool enabled = false;
	bool recordWithMain = false;
	bool collectionLoaded = false;
	CanvasVideoSettings video;

	OBSWeakCanvas weakCanvas;
	std::vector<OBSWeakSource> sceneOrder;
	std::vector<OBSSignal> canvasSignals;

	void DefaultVideoSize(uint32_t &baseCX, uint32_t &baseCY, uint32_t &outputCX, uint32_t &outputCY) const;
	void SaveProfile() const;
	bool MakeVideoInfo(obs_video_info &ovi) const;
	bool ApplyVideo();
	OBSCanvas EnsureCanvas();
	void AttachCanvas(obs_canvas_t *canvas);
	void DetachCanvas();
	void RefreshScenes();
	void EnsureScene();

	static void CanvasSourceChanged(void *data, calldata_t *params);
	static void CanvasRemoved(void *data, calldata_t *params);
};

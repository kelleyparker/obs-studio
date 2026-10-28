#pragma once

#include <obs.hpp>

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

/* A second, vertical (9:16) video mix with its own scenes, like the Aitum
 * Vertical plugin. The canvas itself is one of OBSBasic's canvases, so its
 * scenes are saved with the scene collection. Whether vertical is enabled and
 * its resolution are profile settings ([Vertical] in basic.ini).
 *
 * While vertical is disabled the canvas (and its scenes) is kept, but it has
 * no video mix, so it costs nothing. */
class ExtraCanvas : public QObject {
	Q_OBJECT

public:
	static constexpr const char *ConfigSection = "Vertical";

	explicit ExtraCanvas(QObject *parent = nullptr);
	~ExtraCanvas();

	/* Sets the [Vertical] defaults in the active profile and reads the
	 * profile's settings. Call on startup and after a profile switch. */
	void LoadProfile();

	bool Enabled() const { return enabled; }
	/* Saves the setting to the profile and creates or removes the video mix.
	 * Fails (returns false) while vertical outputs are active. */
	bool SetEnabled(bool enable);

	const CanvasVideoSettings &VideoSettings() const { return video; }
	/* Saves the settings to the profile and resets the video mix. Fails
	 * (returns false) while vertical outputs are active. */
	bool SetVideoSettings(const CanvasVideoSettings &settings);

	/* Re-applies the main canvas's frame rate and color settings, e.g.
	 * after Settings > Video changed them. */
	void MainVideoReset();

	/* The vertical canvas, or nullptr if the scene collection has none yet */
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
	bool enabled = false;
	bool collectionLoaded = false;
	CanvasVideoSettings video;

	OBSWeakCanvas weakCanvas;
	std::vector<OBSWeakSource> sceneOrder;
	std::vector<OBSSignal> canvasSignals;

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

#pragma once

#include <obs.hpp>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <mutex>
#include <vector>

class OBSQTDisplay;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QStackedWidget;
class VerticalCanvas;
class VerticalOutput;

/* Preview of the vertical canvas with recording and replay buffer controls */
class VerticalPreviewDock : public QWidget {
	Q_OBJECT

public:
	VerticalPreviewDock(VerticalCanvas *vertical, VerticalOutput *output, QWidget *parent = nullptr);
	~VerticalPreviewDock();

	/* Called when OBS closes, before the docks are deleted */
	void DestroyDisplay();

signals:
	void OpenSettings();

private:
	QPointer<VerticalCanvas> vertical;
	QPointer<VerticalOutput> output;

	QStackedWidget *stack;
	OBSQTDisplay *display;
	QLabel *status;
	QPushButton *recordButton;
	QPushButton *replayButton;
	QPushButton *saveReplayButton;
	QTimer timer;

	/* Read by the render callback on the graphics thread */
	std::mutex canvasMutex;
	OBSWeakCanvas renderCanvas;

	void UpdateCanvas();
	void UpdateButtons();
	void ToggleRecording();
	void ToggleReplayBuffer();

	static void Render(void *data, uint32_t cx, uint32_t cy);
};

/* Scene list of the vertical canvas */
class VerticalScenesDock : public QWidget {
	Q_OBJECT

public:
	VerticalScenesDock(VerticalCanvas *vertical, QWidget *parent = nullptr);

private:
	QPointer<VerticalCanvas> vertical;
	QListWidget *list;
	QPushButton *removeButton;
	QPushButton *upButton;
	QPushButton *downButton;
	bool updating = false;

	void Rebuild();
	void UpdateSelection();
	OBSSource SelectedScene() const;
	bool AskForSceneName(const QString &title, const QString &initial, QString &name);

	void AddScene();
	void RemoveScene();
	void RenameScene();
	void DuplicateScene();
	void ShowContextMenu(const QPoint &pos);
};

/* Sources of the vertical canvas's current scene */
class VerticalSourcesDock : public QWidget {
	Q_OBJECT

public:
	VerticalSourcesDock(VerticalCanvas *vertical, QWidget *parent = nullptr);

private:
	QPointer<VerticalCanvas> vertical;
	QListWidget *list;
	QPushButton *addButton;
	QMenu *addMenu;
	QPushButton *removeButton;
	QPushButton *upButton;
	QPushButton *downButton;
	QPushButton *propertiesButton;
	QPushButton *filtersButton;
	QPushButton *transformButton;
	QLabel *emptyLabel;

	OBSWeakSource watchedScene;
	std::vector<OBSSignal> sceneSignals;
	bool updating = false;

	enum class Transform {
		Fit,
		Fill,
		Stretch,
		Center,
		Reset,
	};

	void WatchCurrentScene();
	void Rebuild();
	void UpdateButtons();
	obs_scene_t *CurrentScene() const;
	OBSSceneItem SelectedItem() const;

	void BuildAddMenu();
	void AddExisting(obs_source_t *source);
	void AddNew(const char *id);
	void RemoveItem();
	void MoveItem(obs_order_movement movement);
	void ApplyTransform(obs_sceneitem_t *item, Transform transform);
	void ItemChanged(QListWidgetItem *item);
	void ShowContextMenu(const QPoint &pos);
	QMenu *CreateTransformMenu(QWidget *parent);

	static void SceneChanged(void *data, calldata_t *params);
};

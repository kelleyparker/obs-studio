#pragma once

#include <QDialog>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QAudioOutput;
class QCheckBox;
class QDial;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QTableWidget;
class QVideoWidget;

/* One proposed cut from {stem}_yt_cuts.json. start/end are the user's per-cut edges; the
 * "All cuts" dial is added on top when the effective edges are worked out. */
struct YouTubeCut {
	QString id;
	QString kind; /* "silence" or "retake" */
	QString reason;
	double origStart = 0.0;
	double origEnd = 0.0;
	double start = 0.0;
	double end = 0.0;
	bool enabled = true;
};

/* Timeline of the whole recording with the cuts drawn as blocks. Click to seek, drag a
 * block's edge to move it, mouse wheel to zoom. */
class CutTimelineWidget : public QWidget {
	Q_OBJECT

public:
	explicit CutTimelineWidget(QWidget *parent = nullptr);

	void SetData(QVector<YouTubeCut> *cuts, double duration);
	void SetGlobalOffset(double seconds);
	void SetPlayhead(double seconds);
	void SetSelected(int index);

	/* The edges actually used: per-cut edges grown or shrunk by the "All cuts" dial */
	double EffectiveStart(const YouTubeCut &cut) const;
	double EffectiveEnd(const YouTubeCut &cut) const;

signals:
	void SeekRequested(double seconds);
	void CutSelected(int index);
	void CutEdited(int index);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	QVector<YouTubeCut> *cuts = nullptr;
	double duration = 0.0;
	double globalOffset = 0.0;
	double playhead = 0.0;
	double viewStart = 0.0;
	double viewEnd = 0.0;
	int selected = -1;

	int dragCut = -1;
	bool dragStartEdge = false;

	double XToTime(double x) const;
	double TimeToX(double t) const;
	int EdgeAt(const QPoint &pos, bool *startEdge) const;
	int CutAt(double t) const;
};

/* Step mode: review the dead-air and retake cuts on the original recording before anything
 * is rendered. "Preview edit" skips the cuts while playing, so the edit can be heard live. */
class YouTubeCutEditor : public QDialog {
	Q_OBJECT

public:
	explicit YouTubeCutEditor(QWidget *parent = nullptr);
	~YouTubeCutEditor() override;

	bool Load(const QString &cutsPath, const QString &statePath);
	QString StatePath() const { return statePath; }

signals:
	void Approved(const QString &statePath);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	QMediaPlayer *player;
	QAudioOutput *audio;
	QVideoWidget *video;
	CutTimelineWidget *timeline;
	QTableWidget *table;
	QLabel *timeLabel;
	QLabel *lengthLabel;
	QLabel *cutInfoLabel;
	QLabel *startDialLabel;
	QLabel *endDialLabel;
	QLabel *globalDialLabel;
	QDial *startDial;
	QDial *endDial;
	QDial *globalDial;
	QPushButton *playButton;
	QCheckBox *previewCheck;
	QTimer previewTimer;

	QString cutsPath;
	QString statePath;
	QVector<YouTubeCut> cuts;
	double duration = 0.0;
	int selected = -1;
	bool updating = false;

	double Position() const;
	void Seek(double seconds);
	void SelectCut(int index, bool seek);
	void StepCut(int direction);
	void ToggleSelectedCut();
	void ResetSelectedCut();
	void OnDialsChanged();
	void OnGlobalDialChanged();
	void ClampCut(int index);
	void RefreshTable();
	void RefreshRow(int row);
	void RefreshSelectedInfo();
	void RefreshLength();
	void SkipCutsWhilePlaying();
	bool Save();
};

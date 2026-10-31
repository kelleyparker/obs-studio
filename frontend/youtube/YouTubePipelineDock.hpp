#pragma once

#include <QMap>
#include <QPointer>
#include <QString>
#include <QWidget>

class QGroupBox;
class QLabel;
class YouTubeBrollReview;
class YouTubeCutEditor;
class YouTubeTerminalWidget;
class YouTubeMusicPicker;
class YouTubeThumbnailPicker;
class YouTubeVideoPreview;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class YouTubePipelineDock : public QWidget {
	Q_OBJECT

public:
	explicit YouTubePipelineDock(QWidget *parent = nullptr);

private slots:
	void OnPipelineStateChanged(const QString &bannerText, bool running);
	void OnStepProgressUpdated(const QString &stepName, int percent, const QString &message);
	void OnMetadataGenerated(const QString &title, const QStringList &alternativeTitles,
				 const QString &description, const QString &hookText,
				 const QString &providerBadge);
	void OnThumbnailGenerated(const QString &thumbnailPath, const QString &providerBadge);
	void OnPipelineCompleted(const QString &videoUrl, const QString &editedVideoPath, const QString &thumbnailPath,
				 const QString &message);
	void OnLogMessageReceived(const QString &level, const QString &message);
	void OnUploadStatsReady(const QString &statsText);
	void OnVideoUpdateFinished(bool ok, const QString &message);
	void OnReviewReady(const QString &gate, const QString &statePath, const QString &dataPath);
	void OnReviewCleared();
	void OnVideoDeleteFinished(bool ok, const QString &message);
	void OnPremiereExported(const QString &xmlPath, bool open);

private:
	void OpenCutEditor(bool reload);
	void ApproveMetadata();
	void SetModeButtons(const QString &mode);
	void ShowStartOverDialog();
	void ConfirmDeleteFromYouTube();
	void WatchEdit();
	void RefreshEditButtons();

	struct StepRow {
		QLabel *titleLabel = nullptr;
		QLabel *detailLabel = nullptr;
		QProgressBar *bar = nullptr;
	};

	QLabel *statusBanner = nullptr;
	QMap<QString, StepRow> stepRows;

	QLabel *thumbnailPreview = nullptr;
	QLabel *providerBadgeLabel = nullptr;
	QLineEdit *titleEdit = nullptr;
	QComboBox *alternativeTitlesCombo = nullptr;
	QPlainTextEdit *descriptionEdit = nullptr;
	QLabel *youtubeUrlLabel = nullptr;
	QLabel *statsLabel = nullptr;
	QPushButton *submitBtn = nullptr;
	QPlainTextEdit *consoleLog = nullptr;
	YouTubeBrollReview *brollReview = nullptr;
	YouTubeTerminalWidget *terminal = nullptr;

	/* Step | Auto toggle and the AI thumbnail source, applied to the next run */
	QPushButton *stepModeBtn = nullptr;
	QPushButton *autoModeBtn = nullptr;
	QComboBox *imageSourceCombo = nullptr;

	/* Step mode review gates */
	QGroupBox *reviewPanel = nullptr;
	QLabel *reviewLabel = nullptr;
	QPushButton *openCutEditorBtn = nullptr;
	QPushButton *reviewWatchBtn = nullptr;
	QWidget *publishRow = nullptr;
	YouTubeMusicPicker *musicPicker = nullptr;
	YouTubeThumbnailPicker *thumbnailPicker = nullptr;
	QPointer<YouTubeCutEditor> cutEditor;
	QString reviewGate;
	QString reviewState;
	QString reviewData;

	QPushButton *openFolderBtn = nullptr;
	QPushButton *rerunBtn = nullptr;
	QPushButton *watchBtn = nullptr;
	QPushButton *premiereBtn = nullptr;
	QPushButton *deleteBtn = nullptr;
	QPointer<YouTubeVideoPreview> preview;
	QPushButton *cancelBtn = nullptr;

	QString currentEditedVideoPath;
	QString currentThumbnailPath;
};

#pragma once

#include <QMap>
#include <QString>
#include <QWidget>

class QLabel;
class YouTubeBrollReview;
class YouTubeTerminalWidget;
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

private:
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

	QPushButton *openFolderBtn = nullptr;
	QPushButton *rerunBtn = nullptr;
	QPushButton *cancelBtn = nullptr;

	QString currentEditedVideoPath;
	QString currentThumbnailPath;
};

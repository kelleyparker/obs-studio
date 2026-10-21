#pragma once

#include <utility/StreamInfo.hpp>

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

/* Developer credentials for reading viewers, title and category from the
 * Twitch, YouTube and Kick APIs */
class OBSPlatformAccounts : public QDialog {
	Q_OBJECT

public:
	explicit OBSPlatformAccounts(QWidget *parent);

private:
	QLineEdit *twitchClientId;
	QLineEdit *twitchClientSecret;
	QLineEdit *youtubeApiKey;
	QLineEdit *kickClientId;
	QLineEdit *kickClientSecret;

	PlatformCredentials Current() const;
	void Test(StreamPlatform platform, QPushButton *button, QLabel *result);
	void accept() override;
};

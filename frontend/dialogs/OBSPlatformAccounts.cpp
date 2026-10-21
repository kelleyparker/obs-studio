#include "OBSPlatformAccounts.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QThreadPool>
#include <QVBoxLayout>

static QLineEdit *CreateSecretEdit(QWidget *parent, QHBoxLayout *&layout)
{
	QLineEdit *edit = new QLineEdit(parent);
	edit->setEchoMode(QLineEdit::Password);

	QPushButton *show = new QPushButton(QTStr("Show"), parent);
	show->setCheckable(true);
	QObject::connect(show, &QPushButton::toggled, edit, [edit, show](bool visible) {
		edit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
		show->setText(visible ? QTStr("Hide") : QTStr("Show"));
	});

	layout = new QHBoxLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(edit);
	layout->addWidget(show);
	return edit;
}

static QLabel *CreateHelp(const char *textKey, const char *url, QWidget *parent)
{
	QString link =
		QString(" <a href=\"%1\">%2</a>").arg(QString::fromUtf8(url), QTStr("PlatformAccounts.GetCredentials"));
	QLabel *help = new QLabel(QTStr(textKey).toHtmlEscaped() + link, parent);
	help->setWordWrap(true);
	help->setOpenExternalLinks(true);
	help->setTextFormat(Qt::RichText);
	return help;
}

OBSPlatformAccounts::OBSPlatformAccounts(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QTStr("PlatformAccounts.Title"));
	setMinimumWidth(560);

	QLabel *intro = new QLabel(QTStr("PlatformAccounts.Intro"), this);
	intro->setWordWrap(true);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(intro);

	/* A group per platform: how to get the credentials, the fields, and a
	 * button that checks them */
	auto addGroup = [this, layout](StreamPlatform platform, const char *helpKey, const char *url) {
		QGroupBox *group = new QGroupBox(StreamPlatformName(platform), this);
		QFormLayout *form = new QFormLayout(group);
		form->addRow(CreateHelp(helpKey, url, group));
		layout->addWidget(group);
		return form;
	};
	auto addTest = [this](StreamPlatform platform, QFormLayout *form) {
		QPushButton *test = new QPushButton(QTStr("PlatformAccounts.Test"), this);
		QLabel *result = new QLabel(this);
		result->setWordWrap(true);
		result->setTextFormat(Qt::PlainText);

		QHBoxLayout *testLayout = new QHBoxLayout();
		testLayout->addWidget(test);
		testLayout->addWidget(result, 1);
		form->addRow(testLayout);

		connect(test, &QPushButton::clicked, this,
			[this, platform, test, result]() { Test(platform, test, result); });
	};

	QHBoxLayout *secretLayout;

	QFormLayout *twitch =
		addGroup(StreamPlatform::Twitch, "PlatformAccounts.Twitch.Help", "https://dev.twitch.tv/console/apps");
	twitchClientId = new QLineEdit(this);
	twitchClientSecret = CreateSecretEdit(this, secretLayout);
	twitch->addRow(QTStr("PlatformAccounts.ClientId"), twitchClientId);
	twitch->addRow(QTStr("PlatformAccounts.ClientSecret"), secretLayout);
	addTest(StreamPlatform::Twitch, twitch);

	QFormLayout *youtube = addGroup(StreamPlatform::YouTube, "PlatformAccounts.YouTube.Help",
					"https://console.cloud.google.com/apis/library/youtube.googleapis.com");
	youtubeApiKey = CreateSecretEdit(this, secretLayout);
	youtube->addRow(QTStr("PlatformAccounts.ApiKey"), secretLayout);
	addTest(StreamPlatform::YouTube, youtube);

	QFormLayout *kick =
		addGroup(StreamPlatform::Kick, "PlatformAccounts.Kick.Help", "https://kick.com/settings/developer");
	kickClientId = new QLineEdit(this);
	kickClientSecret = CreateSecretEdit(this, secretLayout);
	kick->addRow(QTStr("PlatformAccounts.ClientId"), kickClientId);
	kick->addRow(QTStr("PlatformAccounts.ClientSecret"), secretLayout);
	addTest(StreamPlatform::Kick, kick);

	PlatformCredentials credentials = PlatformCredentials::Load();

	twitchClientId->setText(QString::fromStdString(credentials.twitchClientId));
	twitchClientSecret->setText(QString::fromStdString(credentials.twitchClientSecret));
	youtubeApiKey->setText(QString::fromStdString(credentials.youtubeApiKey));
	kickClientId->setText(QString::fromStdString(credentials.kickClientId));
	kickClientSecret->setText(QString::fromStdString(credentials.kickClientSecret));

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &OBSPlatformAccounts::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addStretch();
	layout->addWidget(buttons);
}

PlatformCredentials OBSPlatformAccounts::Current() const
{
	PlatformCredentials credentials;
	credentials.twitchClientId = twitchClientId->text().trimmed().toStdString();
	credentials.twitchClientSecret = twitchClientSecret->text().trimmed().toStdString();
	credentials.youtubeApiKey = youtubeApiKey->text().trimmed().toStdString();
	credentials.kickClientId = kickClientId->text().trimmed().toStdString();
	credentials.kickClientSecret = kickClientSecret->text().trimmed().toStdString();
	return credentials;
}

void OBSPlatformAccounts::Test(StreamPlatform platform, QPushButton *button, QLabel *result)
{
	button->setEnabled(false);
	result->setProperty("class", "");
	result->setText(QTStr("PlatformAccounts.Test.Running"));

	/* The request blocks, so it runs off the UI thread */
	PlatformCredentials credentials = Current();
	QPointer<QPushButton> buttonPtr(button);
	QPointer<QLabel> resultPtr(result);
	QThreadPool::globalInstance()->start([platform, credentials, buttonPtr, resultPtr]() {
		QString error = TestPlatformCredentials(platform, credentials);
		QMetaObject::invokeMethod(
			qApp,
			[error, buttonPtr, resultPtr]() {
				if (!buttonPtr || !resultPtr) {
					return;
				}
				buttonPtr->setEnabled(true);
				resultPtr->setText(error.isEmpty() ? QTStr("PlatformAccounts.Test.Ok") : error);
				resultPtr->setProperty("class", error.isEmpty() ? "text-success" : "text-danger");
				resultPtr->style()->unpolish(resultPtr);
				resultPtr->style()->polish(resultPtr);
			},
			Qt::QueuedConnection);
	});
}

void OBSPlatformAccounts::accept()
{
	Current().Save();
	if (StreamInfoFetcher *streamInfo = OBSBasic::Get()->GetStreamInfo()) {
		streamInfo->ReloadCredentials();
	}
	QDialog::accept();
}

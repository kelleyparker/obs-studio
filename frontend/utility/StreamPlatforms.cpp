#include "StreamPlatforms.hpp"

#include <utility/MultistreamOutput.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QRegularExpression>
#include <QUrl>

#include <cstring>

using namespace std;

const char *const kPrimaryStreamChannelId = "primary";

StreamPlatform StreamPlatformFromId(const string &id)
{
	if (id == "twitch") {
		return StreamPlatform::Twitch;
	}
	if (id == "youtube") {
		return StreamPlatform::YouTube;
	}
	if (id == "kick") {
		return StreamPlatform::Kick;
	}
	return StreamPlatform::None;
}

const char *StreamPlatformId(StreamPlatform platform)
{
	switch (platform) {
	case StreamPlatform::Twitch:
		return "twitch";
	case StreamPlatform::YouTube:
		return "youtube";
	case StreamPlatform::Kick:
		return "kick";
	case StreamPlatform::None:
		break;
	}
	return "";
}

QString StreamPlatformName(StreamPlatform platform)
{
	switch (platform) {
	case StreamPlatform::Twitch:
		return QStringLiteral("Twitch");
	case StreamPlatform::YouTube:
		return QStringLiteral("YouTube");
	case StreamPlatform::Kick:
		return QStringLiteral("Kick");
	case StreamPlatform::None:
		break;
	}
	return QString();
}

static bool IsYouTubeChannelId(const QString &text)
{
	static const QRegularExpression re("^UC[A-Za-z0-9_-]{22}$");
	return re.match(text).hasMatch();
}

string NormalizeStreamChannel(StreamPlatform platform, const string &channel)
{
	QString text = QString::fromStdString(channel).trimmed();

	/* Accept a pasted channel URL */
	QRegularExpressionMatch url = QRegularExpression("^(?:https?://)?(?:www\\.|m\\.)?"
							 "(?:twitch\\.tv|kick\\.com|youtube\\.com)/(.*)$",
							 QRegularExpression::CaseInsensitiveOption)
					      .match(text);
	if (url.hasMatch()) {
		text = url.captured(1);
		if (platform == StreamPlatform::YouTube && text.startsWith("channel/")) {
			text = text.mid(8);
		}
		text = text.section('/', 0, 0).section('?', 0, 0);
	}

	switch (platform) {
	case StreamPlatform::Twitch:
	case StreamPlatform::Kick: {
		if (text.startsWith('@')) {
			text = text.mid(1);
		}
		static const QRegularExpression re("^[A-Za-z0-9_-]+$");
		return re.match(text).hasMatch() ? text.toLower().toStdString() : string();
	}
	case StreamPlatform::YouTube: {
		if (IsYouTubeChannelId(text)) {
			return text.toStdString();
		}
		if (!text.startsWith('@')) {
			text.prepend('@');
		}
		static const QRegularExpression re("^@[A-Za-z0-9._-]+$");
		return re.match(text).hasMatch() ? text.toStdString() : string();
	}
	case StreamPlatform::None:
		break;
	}
	return string();
}

QString StreamChannelPlaceholder(StreamPlatform platform)
{
	switch (platform) {
	case StreamPlatform::Twitch:
	case StreamPlatform::Kick:
		return QTStr("Multistream.Channel.Placeholder.Username");
	case StreamPlatform::YouTube:
		return QTStr("Multistream.Channel.Placeholder.YouTube");
	case StreamPlatform::None:
		break;
	}
	return QString();
}

static QString Encode(const string &text)
{
	return QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(text), "@"));
}

QString StreamChatUrl(StreamPlatform platform, const string &channel, const string &youtubeVideoId)
{
	switch (platform) {
	case StreamPlatform::Twitch:
		if (!channel.empty()) {
			return QString("https://www.twitch.tv/popout/%1/chat?popout=").arg(Encode(channel));
		}
		break;
	case StreamPlatform::Kick:
		if (!channel.empty()) {
			return QString("https://kick.com/popout/%1/chat").arg(Encode(channel));
		}
		break;
	case StreamPlatform::YouTube:
		if (!youtubeVideoId.empty()) {
			return QString("https://www.youtube.com/live_chat?is_popout=1&v=%1").arg(Encode(youtubeVideoId));
		}
		break;
	case StreamPlatform::None:
		break;
	}
	return QString();
}

QString StreamDashboardUrl(StreamPlatform platform, const string &channel, const string &youtubeVideoId)
{
	switch (platform) {
	case StreamPlatform::Twitch:
		if (!channel.empty()) {
			return QString("https://dashboard.twitch.tv/u/%1/stream-manager").arg(Encode(channel));
		}
		return QStringLiteral("https://dashboard.twitch.tv/");
	case StreamPlatform::YouTube:
		if (!youtubeVideoId.empty()) {
			return QString("https://studio.youtube.com/video/%1/livestreaming").arg(Encode(youtubeVideoId));
		}
		if (IsYouTubeChannelId(QString::fromStdString(channel))) {
			return QString("https://studio.youtube.com/channel/%1/livestreaming").arg(Encode(channel));
		}
		/* Redirects to the signed-in channel's Live Control Room */
		return QStringLiteral("https://www.youtube.com/live_dashboard");
	case StreamPlatform::Kick:
		return QStringLiteral("https://dashboard.kick.com/stream");
	case StreamPlatform::None:
		break;
	}
	return QString();
}

QString YouTubeLivePageUrl(const string &channel)
{
	if (channel.empty()) {
		return QString();
	}
	if (channel[0] == '@') {
		return QString("https://www.youtube.com/%1/live").arg(Encode(channel));
	}
	return QString("https://www.youtube.com/channel/%1/live").arg(Encode(channel));
}

StreamPlatform ResolvePrimaryStreamPlatform(const MultistreamOutput *multistream)
{
	if (multistream && !multistream->PrimaryPlatform().empty()) {
		return StreamPlatformFromId(multistream->PrimaryPlatform());
	}
	return DetectPrimaryStreamPlatform();
}

StreamPlatform DetectPrimaryStreamPlatform()
{
	OBSBasic *main = OBSBasic::Get();
	obs_service_t *service = main ? main->GetService() : nullptr;
	if (!service) {
		return StreamPlatform::None;
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString name = QString::fromUtf8(obs_data_get_string(settings, "service"));
	if (strcmp(obs_service_get_id(service), "rtmp_common") == 0) {
		if (name == "Twitch") {
			return StreamPlatform::Twitch;
		}
		if (name.startsWith("YouTube")) {
			return StreamPlatform::YouTube;
		}
		return StreamPlatform::None;
	}

	/* Custom server: guess from the ingest address. Kick and Twitch both
	 * use Amazon IVS ingest servers (*.global-contribute.live-video.net),
	 * where Twitch's is "ingest.". */
	QString server = QString::fromUtf8(obs_data_get_string(settings, "server")).toLower();
	if (server.contains("twitch.tv") || server.contains("://ingest.global-contribute.live-video.net")) {
		return StreamPlatform::Twitch;
	}
	if (server.contains("youtube.com")) {
		return StreamPlatform::YouTube;
	}
	if (server.contains("live-video.net") || server.contains("kick.com")) {
		return StreamPlatform::Kick;
	}
	return StreamPlatform::None;
}

vector<StreamChannel> GetStreamChannels(const MultistreamOutput *multistream)
{
	vector<StreamChannel> channels;
	if (!multistream) {
		return channels;
	}

	StreamPlatform primary = ResolvePrimaryStreamPlatform(multistream);
	if (primary != StreamPlatform::None) {
		StreamChannel channel;
		channel.id = kPrimaryStreamChannelId;
		channel.name = QTStr("Multistream.Channel.PrimaryName").arg(StreamPlatformName(primary));
		channel.platform = primary;
		channel.channel = NormalizeStreamChannel(primary, multistream->PrimaryChannel());
		channels.push_back(std::move(channel));
	}

	for (const auto &dest : multistream->Destinations()) {
		StreamPlatform platform = StreamPlatformFromId(dest.platform);
		if (platform == StreamPlatform::None) {
			continue;
		}
		StreamChannel channel;
		channel.id = dest.id;
		channel.name = QString::fromStdString(dest.name);
		channel.platform = platform;
		channel.channel = NormalizeStreamChannel(platform, dest.channel);
		channels.push_back(std::move(channel));
	}
	return channels;
}

bool IsStreamChannelLive(const MultistreamOutput *multistream, const string &id)
{
	if (!multistream) {
		return false;
	}
	if (id == kPrimaryStreamChannelId) {
		return multistream->PrimaryLive();
	}
	MultistreamState state = multistream->GetStatus(id).state;
	return state == MultistreamState::Live || state == MultistreamState::Reconnecting;
}

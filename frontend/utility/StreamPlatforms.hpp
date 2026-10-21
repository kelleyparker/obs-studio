#pragma once

#include <QString>

#include <string>
#include <vector>

class MultistreamOutput;

/* Platforms with chat docks, dashboard links and stream info */
enum class StreamPlatform {
	None,
	Twitch,
	YouTube,
	Kick,
};

/* "twitch", "youtube" or "kick" (multistream platform ids); None otherwise */
StreamPlatform StreamPlatformFromId(const std::string &id);
const char *StreamPlatformId(StreamPlatform platform);
QString StreamPlatformName(StreamPlatform platform);

/* Accepts a username, @handle, channel ID or channel URL and returns the
 * channel as it is used in URLs: the Twitch/Kick username, or the YouTube
 * @handle or UC... channel ID. Empty when nothing usable was entered. */
std::string NormalizeStreamChannel(StreamPlatform platform, const std::string &channel);

/* Placeholder text for a channel field */
QString StreamChannelPlaceholder(StreamPlatform platform);

/* The platform's pop-out chat. YouTube needs the live video's ID; the URL is
 * empty until it is known. */
QString StreamChatUrl(StreamPlatform platform, const std::string &channel, const std::string &youtubeVideoId);
/* The platform's stream dashboard, opened in the system browser */
QString StreamDashboardUrl(StreamPlatform platform, const std::string &channel, const std::string &youtubeVideoId);
/* The channel's "live" page, which redirects to its current live video */
QString YouTubeLivePageUrl(const std::string &channel);

/* The main stream's or a multistream destination's channel */
struct StreamChannel {
	std::string id; /* destination id, or kPrimaryStreamChannelId */
	QString name;
	StreamPlatform platform = StreamPlatform::None;
	std::string channel; /* normalized; may be empty */
};

extern const char *const kPrimaryStreamChannelId;

/* The main stream's platform, detected from the stream service in
 * Settings > Stream */
StreamPlatform DetectPrimaryStreamPlatform();
/* The main stream's platform: the configured one, or else the detected one */
StreamPlatform ResolvePrimaryStreamPlatform(const MultistreamOutput *multistream);

/* The main stream (when its platform is known) and every destination on a
 * supported platform, whether or not a channel is set */
std::vector<StreamChannel> GetStreamChannels(const MultistreamOutput *multistream);

/* Whether the main stream or a destination is live */
bool IsStreamChannelLive(const MultistreamOutput *multistream, const std::string &id);

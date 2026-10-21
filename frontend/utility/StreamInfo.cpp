#include "StreamInfo.hpp"

#include <OBSApp.hpp>
#include <utility/MultistreamOutput.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <memory>
#include <vector>

using namespace std;

static constexpr int kPollIntervalMs = 60 * 1000;
/* A platform needs a few seconds to list a stream that just went live */
static constexpr int kSoonDelayMs = 10 * 1000;
static constexpr int kRequestTimeoutSec = 10;

static const char *const kSection = "PlatformAccounts";

/* ------------------------------------------------------------------------- */

PlatformCredentials PlatformCredentials::Load()
{
	config_t *config = App()->GetUserConfig();
	auto get = [config](const char *name) {
		const char *value = config_get_string(config, kSection, name);
		return string(value ? value : "");
	};

	PlatformCredentials credentials;
	credentials.twitchClientId = get("TwitchClientId");
	credentials.twitchClientSecret = get("TwitchClientSecret");
	credentials.youtubeApiKey = get("YouTubeApiKey");
	credentials.kickClientId = get("KickClientId");
	credentials.kickClientSecret = get("KickClientSecret");
	return credentials;
}

void PlatformCredentials::Save() const
{
	config_t *config = App()->GetUserConfig();
	config_set_string(config, kSection, "TwitchClientId", twitchClientId.c_str());
	config_set_string(config, kSection, "TwitchClientSecret", twitchClientSecret.c_str());
	config_set_string(config, kSection, "YouTubeApiKey", youtubeApiKey.c_str());
	config_set_string(config, kSection, "KickClientId", kickClientId.c_str());
	config_set_string(config, kSection, "KickClientSecret", kickClientSecret.c_str());
	config_save_safe(config, "tmp", nullptr);
}

bool PlatformCredentials::Has(StreamPlatform platform) const
{
	switch (platform) {
	case StreamPlatform::Twitch:
		return !twitchClientId.empty() && !twitchClientSecret.empty();
	case StreamPlatform::YouTube:
		return !youtubeApiKey.empty();
	case StreamPlatform::Kick:
		return !kickClientId.empty() && !kickClientSecret.empty();
	case StreamPlatform::None:
		break;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Requests. These run on the fetcher's thread pool. */

struct Response {
	bool ok = false;
	long code = 0;
	string body;
	QString error;
	QJsonObject json;
};

static QString ApiErrorMessage(const QJsonObject &json)
{
	/* Google: {"error": {"message": ...}}; Twitch and Kick: {"message": ...} */
	QJsonValue error = json.value("error");
	if (error.isObject()) {
		return error.toObject().value("message").toString();
	}
	QString message = json.value("message").toString();
	if (message.isEmpty() && error.isString()) {
		message = error.toString();
	}
	return message;
}

static Response Fetch(const QString &url, vector<string> headers = {}, const string &postData = string(),
		      const char *contentType = nullptr, bool parseJson = true)
{
	Response response;
	string error;
	string urlString = url.toStdString();
	response.ok = GetRemoteFile(urlString.c_str(), response.body, error, &response.code, contentType,
				    postData.empty() ? string() : string("POST"),
				    postData.empty() ? nullptr : postData.c_str(), std::move(headers), nullptr,
				    kRequestTimeoutSec, false);

	if (parseJson && !response.body.empty()) {
		response.json = QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
	}

	if (!response.ok) {
		response.error = QString::fromStdString(error);
	} else if (response.code < 200 || response.code >= 300) {
		response.ok = false;
		QString message = ApiErrorMessage(response.json);
		response.error = message.isEmpty() ? QString("HTTP %1").arg(response.code)
						   : QString("HTTP %1: %2").arg(response.code).arg(message);
	}
	return response;
}

static string Encode(const string &text)
{
	return QUrl::toPercentEncoding(QString::fromStdString(text)).toStdString();
}

/* Client credentials grant (Twitch and Kick) */
static bool FetchAppToken(const char *url, const string &clientId, const string &clientSecret, string &token,
			  QString &error)
{
	string post = "client_id=" + Encode(clientId) + "&client_secret=" + Encode(clientSecret) +
		      "&grant_type=client_credentials";
	Response response = Fetch(url, {}, post, "application/x-www-form-urlencoded");
	if (!response.ok) {
		error = response.error;
		return false;
	}

	token = response.json.value("access_token").toString().toStdString();
	if (token.empty()) {
		error = QStringLiteral("No access token in the response");
		return false;
	}
	return true;
}

static const char *const kTwitchTokenUrl = "https://id.twitch.tv/oauth2/token";
static const char *const kKickTokenUrl = "https://id.kick.com/oauth/token";

/* Makes an authorized request, getting a new app token first if there is
 * none or the old one was rejected */
static Response FetchWithToken(const QString &url, const char *tokenUrl, const string &clientId,
			       const string &clientSecret, string &token, bool sendClientId)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		if (token.empty()) {
			Response failed;
			if (!FetchAppToken(tokenUrl, clientId, clientSecret, token, failed.error)) {
				return failed;
			}
		}

		vector<string> headers = {"Authorization: Bearer " + token, "Accept: application/json"};
		if (sendClientId) {
			headers.push_back("Client-Id: " + clientId);
		}

		Response response = Fetch(url, std::move(headers));
		if (response.code != 401) {
			return response;
		}
		token.clear();
	}

	Response failed;
	failed.error = QStringLiteral("The platform rejected the app credentials");
	return failed;
}

static bool FetchTwitchInfo(const PlatformCredentials &credentials, string &token, const string &login,
			    StreamInfo &info, QString &error)
{
	QString url =
		QString("https://api.twitch.tv/helix/streams?user_login=%1").arg(QString::fromStdString(Encode(login)));
	Response response = FetchWithToken(url, kTwitchTokenUrl, credentials.twitchClientId,
					   credentials.twitchClientSecret, token, true);
	if (!response.ok) {
		error = response.error;
		return false;
	}

	/* No entry while the channel isn't live yet */
	QJsonArray data = response.json.value("data").toArray();
	if (data.isEmpty()) {
		return true;
	}
	QJsonObject stream = data.first().toObject();
	info.viewers = stream.value("viewer_count").toInteger(-1);
	info.title = stream.value("title").toString();
	info.category = stream.value("game_name").toString();
	return true;
}

static bool FetchKickInfo(const PlatformCredentials &credentials, string &token, const string &slug, StreamInfo &info,
			  QString &error)
{
	QString url =
		QString("https://api.kick.com/public/v1/channels?slug=%1").arg(QString::fromStdString(Encode(slug)));
	Response response = FetchWithToken(url, kKickTokenUrl, credentials.kickClientId, credentials.kickClientSecret,
					   token, false);
	if (!response.ok) {
		error = response.error;
		return false;
	}

	QJsonArray data = response.json.value("data").toArray();
	if (data.isEmpty()) {
		error = QStringLiteral("Kick has no channel named \"%1\"").arg(QString::fromStdString(slug));
		return false;
	}
	QJsonObject channel = data.first().toObject();
	QJsonObject stream = channel.value("stream").toObject();
	info.title = channel.value("stream_title").toString();
	info.category = channel.value("category").toObject().value("name").toString();
	if (stream.value("is_live").toBool()) {
		info.viewers = stream.value("viewer_count").toInteger(-1);
	}
	return true;
}

static const char *const kYouTubeApi = "https://www.googleapis.com/youtube/v3/";

static QString YouTubeApiUrl(const char *method, const QString &query, const string &apiKey)
{
	return QString("%1%2?%3&key=%4")
		.arg(QString::fromUtf8(kYouTubeApi), QString::fromUtf8(method), query,
		     QString::fromStdString(Encode(apiKey)));
}

/* The channel's live video, read from its "live" page. With an API key, the
 * API is asked when the page can't be read. */
static string FindYouTubeLiveVideo(const string &channel, const string &apiKey, QString &error)
{
	static const QRegularExpression canonical(
		"<link rel=\"canonical\" href=\"https://www\\.youtube\\.com/watch\\?v=([A-Za-z0-9_-]{11})\"");

	/* SOCS skips the cookie consent page served in some regions */
	Response page = Fetch(YouTubeLivePageUrl(channel), {"Accept-Language: en-US,en;q=0.9", "Cookie: SOCS=CAI"},
			      string(), nullptr, false);
	if (page.ok) {
		QRegularExpressionMatch match = canonical.match(QString::fromStdString(page.body));
		/* Without a watch link the channel isn't live (yet) */
		return match.hasMatch() ? match.captured(1).toStdString() : string();
	}
	if (apiKey.empty()) {
		error = page.error;
		return string();
	}

	string channelId = channel;
	if (!channel.empty() && channel[0] == '@') {
		Response handle = Fetch(YouTubeApiUrl(
			"channels", QString("part=id&forHandle=%1").arg(QString::fromStdString(Encode(channel))),
			apiKey));
		QJsonArray items = handle.json.value("items").toArray();
		if (!handle.ok || items.isEmpty()) {
			error = handle.ok ? QStringLiteral("YouTube has no channel %1")
						    .arg(QString::fromStdString(channel))
					  : handle.error;
			return string();
		}
		channelId = items.first().toObject().value("id").toString().toStdString();
	}

	Response search = Fetch(YouTubeApiUrl(
		"search",
		QString("part=id&type=video&eventType=live&channelId=%1").arg(QString::fromStdString(Encode(channelId))),
		apiKey));
	if (!search.ok) {
		error = search.error;
		return string();
	}
	QJsonArray items = search.json.value("items").toArray();
	if (items.isEmpty()) {
		return string();
	}
	return items.first().toObject().value("id").toObject().value("videoId").toString().toStdString();
}

static bool FetchYouTubeInfo(const string &apiKey, const string &videoId, map<string, QString> &categories,
			     StreamInfo &info, QString &error)
{
	Response response = Fetch(YouTubeApiUrl(
		"videos",
		QString("part=snippet,liveStreamingDetails&id=%1").arg(QString::fromStdString(Encode(videoId))),
		apiKey));
	if (!response.ok) {
		error = response.error;
		return false;
	}

	QJsonArray items = response.json.value("items").toArray();
	if (items.isEmpty()) {
		error = QStringLiteral("YouTube has no video %1").arg(QString::fromStdString(videoId));
		return false;
	}
	QJsonObject video = items.first().toObject();
	QJsonObject snippet = video.value("snippet").toObject();
	info.title = snippet.value("title").toString();

	/* A string; missing when the channel hides its viewer count */
	QString viewers = video.value("liveStreamingDetails").toObject().value("concurrentViewers").toString();
	bool ok = false;
	qint64 count = viewers.toLongLong(&ok);
	info.viewers = ok ? count : -1;

	string categoryId = snippet.value("categoryId").toString().toStdString();
	if (categoryId.empty()) {
		return true;
	}
	auto it = categories.find(categoryId);
	if (it == categories.end()) {
		Response category = Fetch(YouTubeApiUrl(
			"videoCategories",
			QString("part=snippet&id=%1").arg(QString::fromStdString(Encode(categoryId))), apiKey));
		QJsonArray categoryItems = category.json.value("items").toArray();
		if (!category.ok || categoryItems.isEmpty()) {
			return true;
		}
		QString name = categoryItems.first().toObject().value("snippet").toObject().value("title").toString();
		it = categories.emplace(categoryId, name).first;
	}
	info.category = it->second;
	return true;
}

QString TestPlatformCredentials(StreamPlatform platform, const PlatformCredentials &credentials)
{
	if (!credentials.Has(platform)) {
		return QTStr("PlatformAccounts.Test.Missing");
	}

	string token;
	QString error;
	switch (platform) {
	case StreamPlatform::Twitch:
		FetchAppToken(kTwitchTokenUrl, credentials.twitchClientId, credentials.twitchClientSecret, token,
			      error);
		break;
	case StreamPlatform::Kick:
		FetchAppToken(kKickTokenUrl, credentials.kickClientId, credentials.kickClientSecret, token, error);
		break;
	case StreamPlatform::YouTube: {
		/* Costs 1 unit of the key's daily quota */
		Response response = Fetch(
			YouTubeApiUrl("videoCategories", "part=snippet&regionCode=US", credentials.youtubeApiKey));
		error = response.error;
		break;
	}
	case StreamPlatform::None:
		break;
	}
	return error;
}

/* ------------------------------------------------------------------------- */

struct StreamInfoFetcher::Job {
	string id;
	StreamPlatform platform = StreamPlatform::None;
	string channel;
	string videoId; /* in: known video; out: found video */

	StreamInfo info;
	QString error;
};

struct StreamInfoFetcher::Batch {
	PlatformCredentials credentials;
	uint64_t credentialsSerial = 0;
	string twitchToken;
	string kickToken;
	map<string, QString> youtubeCategories;
	vector<Job> jobs;
};

void StreamInfoFetcher::RunBatch(Batch &batch)
{
	for (Job &job : batch.jobs) {
		job.info.live = true;

		switch (job.platform) {
		case StreamPlatform::Twitch:
			if (batch.credentials.Has(StreamPlatform::Twitch)) {
				FetchTwitchInfo(batch.credentials, batch.twitchToken, job.channel, job.info, job.error);
			}
			break;
		case StreamPlatform::Kick:
			if (batch.credentials.Has(StreamPlatform::Kick)) {
				FetchKickInfo(batch.credentials, batch.kickToken, job.channel, job.info, job.error);
			}
			break;
		case StreamPlatform::YouTube:
			if (job.videoId.empty()) {
				job.videoId =
					FindYouTubeLiveVideo(job.channel, batch.credentials.youtubeApiKey, job.error);
			}
			if (!job.videoId.empty() && batch.credentials.Has(StreamPlatform::YouTube)) {
				FetchYouTubeInfo(batch.credentials.youtubeApiKey, job.videoId, batch.youtubeCategories,
						 job.info, job.error);
			}
			break;
		case StreamPlatform::None:
			break;
		}
	}
}

StreamInfoFetcher::StreamInfoFetcher(MultistreamOutput *multistream_, QObject *parent)
	: QObject(parent),
	  multistream(multistream_),
	  credentials(PlatformCredentials::Load())
{
	pool.setMaxThreadCount(1);

	pollTimer.setInterval(kPollIntervalMs);
	connect(&pollTimer, &QTimer::timeout, this, &StreamInfoFetcher::Poll);
	pollTimer.start();

	soonTimer.setSingleShot(true);
	soonTimer.setInterval(kSoonDelayMs);
	connect(&soonTimer, &QTimer::timeout, this, &StreamInfoFetcher::Poll);

	if (multistream) {
		connect(multistream, &MultistreamOutput::DestinationsChanged, this, &StreamInfoFetcher::UpdateChannels);
		connect(multistream, &MultistreamOutput::StatusChanged, this, &StreamInfoFetcher::UpdateChannels);
		connect(multistream, &MultistreamOutput::PrimaryStateChanged, this, &StreamInfoFetcher::UpdateChannels);
	}
	UpdateChannels();
}

StreamInfoFetcher::~StreamInfoFetcher()
{
	/* Requests time out after a few seconds */
	pool.clear();
	pool.waitForDone();
}

StreamInfo StreamInfoFetcher::Info(const string &id) const
{
	auto it = channels.find(id);
	return it != channels.end() ? it->second.info : StreamInfo();
}

string StreamInfoFetcher::YouTubeVideoId(const string &id) const
{
	auto it = channels.find(id);
	return it != channels.end() ? it->second.videoId : string();
}

void StreamInfoFetcher::ReloadCredentials()
{
	credentials = PlatformCredentials::Load();
	credentialsSerial++;
	twitchToken.clear();
	kickToken.clear();

	/* Info that came from removed credentials is out of date */
	for (auto &[id, channel] : channels) {
		bool hadInfo = channel.info.viewers >= 0 || !channel.info.title.isEmpty();
		if (hadInfo && !credentials.Has(channel.platform)) {
			channel.info = StreamInfo();
			channel.info.live = channel.live;
			emit InfoChanged(QString::fromStdString(id));
		}
	}
	Poll();
}

static bool SameInfo(const StreamInfo &a, const StreamInfo &b)
{
	return a.live == b.live && a.viewers == b.viewers && a.title == b.title && a.category == b.category;
}

void StreamInfoFetcher::UpdateChannels()
{
	map<string, Channel> updated;
	vector<string> changed;
	bool wentLive = false;

	for (const StreamChannel &streamChannel : GetStreamChannels(multistream)) {
		Channel channel;
		channel.platform = streamChannel.platform;
		channel.channel = streamChannel.channel;
		channel.live = IsStreamChannelLive(multistream, streamChannel.id);

		auto old = channels.find(streamChannel.id);
		bool same = old != channels.end() && old->second.platform == channel.platform &&
			    old->second.channel == channel.channel;
		if (same && old->second.live == channel.live) {
			updated.emplace(streamChannel.id, std::move(old->second));
			continue;
		}

		/* New, changed, or went live or offline: start over */
		channel.info.live = channel.live;
		wentLive = wentLive || channel.live;
		changed.push_back(streamChannel.id);
		updated.emplace(streamChannel.id, std::move(channel));
	}

	for (const auto &[id, channel] : channels) {
		if (updated.find(id) == updated.end()) {
			changed.push_back(id);
		}
	}

	channels = std::move(updated);
	for (const string &id : changed) {
		emit InfoChanged(QString::fromStdString(id));
	}
	if (wentLive) {
		soonTimer.start();
	}
}

void StreamInfoFetcher::Poll()
{
	if (busy) {
		pollAgain = true;
		return;
	}

	auto batch = make_shared<Batch>();
	batch->credentials = credentials;
	batch->credentialsSerial = credentialsSerial;
	batch->twitchToken = twitchToken;
	batch->kickToken = kickToken;
	batch->youtubeCategories = youtubeCategories;

	for (const auto &[id, channel] : channels) {
		if (!channel.live || channel.channel.empty()) {
			continue;
		}
		/* YouTube's live video is looked up without credentials, for its
		 * chat dock and dashboard link */
		bool needed = credentials.Has(channel.platform) ||
			      (channel.platform == StreamPlatform::YouTube && channel.videoId.empty());
		if (!needed) {
			continue;
		}

		Job job;
		job.id = id;
		job.platform = channel.platform;
		job.channel = channel.channel;
		job.videoId = channel.videoId;
		batch->jobs.push_back(std::move(job));
	}
	if (batch->jobs.empty()) {
		return;
	}

	busy = true;
	QPointer<StreamInfoFetcher> self(this);
	pool.start([batch, self]() {
		RunBatch(*batch);
		QMetaObject::invokeMethod(
			qApp,
			[batch, self]() {
				if (self) {
					self->ApplyBatch(*batch);
				}
			},
			Qt::QueuedConnection);
	});
}

void StreamInfoFetcher::ApplyBatch(const Batch &batch)
{
	busy = false;

	if (batch.credentialsSerial == credentialsSerial) {
		twitchToken = batch.twitchToken;
		kickToken = batch.kickToken;
	}
	youtubeCategories = batch.youtubeCategories;

	for (const Job &job : batch.jobs) {
		auto it = channels.find(job.id);
		/* Skip results for a channel that changed or went offline since */
		if (it == channels.end() || !it->second.live || it->second.platform != job.platform ||
		    it->second.channel != job.channel) {
			continue;
		}
		Channel &channel = it->second;

		if (!job.error.isEmpty() && job.error != channel.lastError) {
			blog(LOG_WARNING, "[Stream Info] %s channel \"%s\": %s",
			     QT_TO_UTF8(StreamPlatformName(job.platform)), job.channel.c_str(), QT_TO_UTF8(job.error));
		}
		channel.lastError = job.error;

		bool changed = false;
		if (channel.videoId != job.videoId) {
			channel.videoId = job.videoId;
			changed = true;
		}
		/* Keep the last good info when a request fails */
		if (job.error.isEmpty() && !SameInfo(channel.info, job.info)) {
			channel.info = job.info;
			changed = true;
		}
		if (changed) {
			emit InfoChanged(QString::fromStdString(job.id));
		}
	}

	if (pollAgain) {
		pollAgain = false;
		Poll();
	}
}

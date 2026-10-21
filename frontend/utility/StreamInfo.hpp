#pragma once

#include <utility/StreamPlatforms.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <cstdint>
#include <map>
#include <string>

class MultistreamOutput;

/* Developer credentials for reading stream info from the platforms' APIs,
 * saved in the app config (section [PlatformAccounts]) */
struct PlatformCredentials {
	std::string twitchClientId;
	std::string twitchClientSecret;
	std::string youtubeApiKey;
	std::string kickClientId;
	std::string kickClientSecret;

	static PlatformCredentials Load();
	void Save() const;
	bool Has(StreamPlatform platform) const;
};

/* Checks credentials against the platform's API. Blocks; run it off the UI
 * thread. Returns an empty string on success, or the error. */
QString TestPlatformCredentials(StreamPlatform platform, const PlatformCredentials &credentials);

/* What a platform reports about a live channel */
struct StreamInfo {
	bool live = false;
	int64_t viewers = -1; /* -1: unknown */
	QString title;
	QString category;
};

/* Finds the live video of YouTube channels, and polls viewers, title and
 * category of every live channel once a minute */
class StreamInfoFetcher : public QObject {
	Q_OBJECT

public:
	explicit StreamInfoFetcher(MultistreamOutput *multistream, QObject *parent = nullptr);
	~StreamInfoFetcher();

	StreamInfo Info(const std::string &id) const;
	/* The live video of a YouTube channel; empty until it is found */
	std::string YouTubeVideoId(const std::string &id) const;

	/* Call after the credentials were changed */
	void ReloadCredentials();

signals:
	void InfoChanged(const QString &id);

private:
	struct Channel {
		StreamPlatform platform = StreamPlatform::None;
		std::string channel;
		bool live = false;
		std::string videoId;
		StreamInfo info;
		QString lastError; /* logged once, not on every poll */
	};

	struct Job;
	struct Batch;

	QPointer<MultistreamOutput> multistream;
	PlatformCredentials credentials;
	std::map<std::string, Channel> channels;

	/* Only touched on the UI thread; copied into and out of each batch.
	 * The serial changes with the credentials, so a batch that ran with old
	 * credentials doesn't bring back their tokens. */
	std::string twitchToken;
	std::string kickToken;
	std::map<std::string, QString> youtubeCategories;
	uint64_t credentialsSerial = 0;

	QThreadPool pool;
	QTimer pollTimer;
	QTimer soonTimer;
	bool busy = false;
	bool pollAgain = false;

	void UpdateChannels();
	void Poll();
	void ApplyBatch(const Batch &batch);
	static void RunBatch(Batch &batch);
};

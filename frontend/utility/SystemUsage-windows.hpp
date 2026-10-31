#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>

class QTimer;

/* One meter in the System Monitor: how busy one part of the PC is */
struct UsageReading {
	QString key;    /* stays the same for the same device, e.g. "gpu:0x0000d1a5" */
	QString group;  /* "cpu", "memory", "gpu", "disk", "drive" or "network" */
	QString label;  /* "GPU · NVIDIA GeForce RTX 5070" */
	double percent; /* 0 to 100 */
	QString detail; /* "VRAM 7.9 / 12.0 GB" */
};

/* Reads CPU, memory, GPU (load and VRAM), physical disk and network use once a second, the way Task
 * Manager does (PDH, DXGI, GetIfTable2). Lives on its own thread so the counters never hold up the UI;
 * Start and Stop are called through queued connections. */
class SystemUsageSampler : public QObject {
	Q_OBJECT

public:
	SystemUsageSampler();
	~SystemUsageSampler();

public slots:
	void Start();
	void Stop();

signals:
	void Sampled(const QList<UsageReading> &readings);

private:
	struct Adapter {
		QString name;
		uint64_t dedicatedBytes = 0;
	};
	struct NetCount {
		uint64_t in = 0;
		uint64_t out = 0;
	};

	QTimer *timer;
	void *query = nullptr; /* PDH_HQUERY */
	void *gpuEngine = nullptr;
	void *gpuMemory = nullptr;
	void *diskIdle = nullptr;
	void *diskRead = nullptr;
	void *diskWrite = nullptr;
	QHash<uint64_t, Adapter> adapters; /* by LUID */
	uint64_t lastIdle = 0, lastBusy = 0;
	QHash<uint64_t, NetCount> lastNet; /* by interface LUID */
	int64_t lastNetTime = 0;

	void OpenCounters();
	void CloseCounters();
	void FindAdapters();
	void Sample();
	void SampleCpu(QList<UsageReading> &out);
	void SampleMemory(QList<UsageReading> &out);
	void SampleGpus(QList<UsageReading> &out);
	void SampleDisks(QList<UsageReading> &out);
	void SampleNetwork(QList<UsageReading> &out);
};

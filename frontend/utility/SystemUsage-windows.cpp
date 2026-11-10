/* Winsock 2 has to come before anything that pulls in windows.h (and with it the old winsock.h) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "SystemUsage-windows.hpp"

#include <util/windows/ComPtr.hpp>

#include <QMap>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <utility>
#include <vector>

namespace {
using CounterItems = std::vector<std::pair<QString, double>>;

/* Every instance of a wildcard counter ("\GPU Engine(*)\...") with valid data in the last collection */
CounterItems CounterValues(void *counter, DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100)
{
	CounterItems items;
	if (!counter) {
		return items;
	}
	DWORD bytes = 0;
	DWORD count = 0;
	if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), format, &bytes, &count, nullptr) !=
	    PDH_MORE_DATA) {
		return items;
	}
	std::vector<BYTE> buffer(bytes);
	auto *values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
	if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter), format, &bytes, &count, values) !=
	    ERROR_SUCCESS) {
		return items;
	}
	for (DWORD i = 0; i < count; i++) {
		DWORD status = values[i].FmtValue.CStatus;
		if (status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA) {
			items.emplace_back(QString::fromWCharArray(values[i].szName), values[i].FmtValue.doubleValue);
		}
	}
	return items;
}

/* The adapter LUID in a GPU counter's instance name ("pid_42_luid_0x00000000_0x0000D1A5_phys_0_eng_3..."),
 * as DXGI's AdapterLuid packed into one number; 0 if there is none */
uint64_t InstanceLuid(const QString &instance)
{
	static const QRegularExpression re("luid_0x([0-9a-fA-F]+)_0x([0-9a-fA-F]+)");
	QRegularExpressionMatch m = re.match(instance);
	if (!m.hasMatch()) {
		return 0;
	}
	return (m.captured(1).toULongLong(nullptr, 16) << 32) | m.captured(2).toULongLong(nullptr, 16);
}

QString Gigabytes(double bytes)
{
	return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1);
}

QString BitRate(double bitsPerSecond)
{
	if (bitsPerSecond >= 1e9) {
		return QString("%1 Gbps").arg(bitsPerSecond / 1e9, 0, 'f', 2);
	}
	if (bitsPerSecond >= 1e6) {
		return QString("%1 Mbps").arg(bitsPerSecond / 1e6, 0, 'f', 1);
	}
	return QString("%1 Kbps").arg(bitsPerSecond / 1e3, 0, 'f', 0);
}

QString ByteRate(double bytesPerSecond)
{
	if (bytesPerSecond >= 1024.0 * 1024.0) {
		return QString("%1 MB/s").arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 1);
	}
	return QString("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 0);
}

uint64_t FileTimeValue(const FILETIME &ft)
{
	return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
} // namespace

SystemUsageSampler::SystemUsageSampler()
{
	timer = new QTimer(this);
	timer->setInterval(1000);
	connect(timer, &QTimer::timeout, this, &SystemUsageSampler::Sample);
}

SystemUsageSampler::~SystemUsageSampler()
{
	CloseCounters();
}

void SystemUsageSampler::Start()
{
	if (timer->isActive()) {
		return;
	}
	OpenCounters();
	FindAdapters();
	/* CPU and network are measured between two samples, so take the first one now */
	QList<UsageReading> primed;
	SampleCpu(primed);
	SampleNetwork(primed);
	timer->start();
}

void SystemUsageSampler::Stop()
{
	timer->stop();
	CloseCounters();
	lastIdle = lastBusy = 0;
	lastNet.clear();
}

void SystemUsageSampler::OpenCounters()
{
	PDH_HQUERY q = nullptr;
	if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
		return;
	}
	query = q;
	auto add = [q](const wchar_t *path) -> void * {
		PDH_HCOUNTER counter = nullptr;
		return PdhAddEnglishCounterW(q, path, 0, &counter) == ERROR_SUCCESS ? counter : nullptr;
	};
	gpuEngine = add(L"\\GPU Engine(*)\\Utilization Percentage");
	gpuMemory = add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
	diskIdle = add(L"\\PhysicalDisk(*)\\% Idle Time");
	diskRead = add(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
	diskWrite = add(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
	/* Rates need two collections; this is the first */
	PdhCollectQueryData(q);
}

void SystemUsageSampler::CloseCounters()
{
	if (query) {
		PdhCloseQuery(static_cast<PDH_HQUERY>(query));
	}
	query = gpuEngine = gpuMemory = diskIdle = diskRead = diskWrite = nullptr;
}

void SystemUsageSampler::FindAdapters()
{
	adapters.clear();
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return;
	}
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, adapter.Assign()) == S_OK; ++i) {
		DXGI_ADAPTER_DESC1 desc;
		if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			continue;
		}
		uint64_t luid = (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
		adapters[luid] =
			Adapter{QString::fromWCharArray(desc.Description), uint64_t(desc.DedicatedVideoMemory)};
	}
}

void SystemUsageSampler::Sample()
{
	if (query) {
		PdhCollectQueryData(static_cast<PDH_HQUERY>(query));
	}
	QList<UsageReading> readings;
	SampleCpu(readings);
	SampleMemory(readings);
	SampleGpus(readings);
	SampleDisks(readings);
	SampleNetwork(readings);
	emit Sampled(readings);
}

void SystemUsageSampler::SampleCpu(QList<UsageReading> &out)
{
	FILETIME idleTime, kernelTime, userTime;
	if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
		return;
	}
	/* Kernel time includes the idle time */
	uint64_t idle = FileTimeValue(idleTime);
	uint64_t total = FileTimeValue(kernelTime) + FileTimeValue(userTime);
	if (lastBusy && total > lastBusy) {
		double used = 1.0 - double(idle - lastIdle) / double(total - lastBusy);
		out.append({"cpu", "cpu", "CPU", std::clamp(used * 100.0, 0.0, 100.0),
			    QString("%1 logical processors").arg(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))});
	}
	lastIdle = idle;
	lastBusy = total;
}

void SystemUsageSampler::SampleMemory(QList<UsageReading> &out)
{
	MEMORYSTATUSEX mem = {};
	mem.dwLength = sizeof(mem);
	if (!GlobalMemoryStatusEx(&mem) || !mem.ullTotalPhys) {
		return;
	}
	double used = double(mem.ullTotalPhys - mem.ullAvailPhys);
	out.append({"memory", "memory", "Memory", used * 100.0 / double(mem.ullTotalPhys),
		    QString("%1 / %2 GB").arg(Gigabytes(used), Gigabytes(double(mem.ullTotalPhys)))});
}

void SystemUsageSampler::SampleGpus(QList<UsageReading> &out)
{
	/* An engine's use is its processes' use added up; the GPU is as busy as its busiest engine, as in
	 * Task Manager */
	static const QRegularExpression engineRe("_eng_(\\d+)");
	QMap<uint64_t, QHash<int, double>> engines;
	for (const auto &[instance, value] : CounterValues(gpuEngine)) {
		uint64_t luid = InstanceLuid(instance);
		QRegularExpressionMatch m = engineRe.match(instance);
		if (luid && m.hasMatch()) {
			engines[luid][m.captured(1).toInt()] += value;
		}
	}
	QHash<uint64_t, double> vram;
	for (const auto &[instance, value] : CounterValues(gpuMemory)) {
		if (uint64_t luid = InstanceLuid(instance)) {
			vram[luid] += value;
		}
	}

	for (auto it = adapters.cbegin(); it != adapters.cend(); ++it) {
		if (!engines.contains(it.key()) && !vram.contains(it.key())) {
			continue;
		}
		double load = 0.0;
		for (double v : engines.value(it.key())) {
			load = std::max(load, v);
		}
		QString id = QString::number(it.key(), 16);
		out.append({"gpu:" + id, "gpu", "GPU · " + it->name, std::clamp(load, 0.0, 100.0), "Busiest engine"});
		/* Integrated graphics have next to no memory of their own, so a VRAM meter would mislead */
		if (it->dedicatedBytes >= 512ull * 1024 * 1024) {
			double used = vram.value(it.key());
			out.append({"vram:" + id, "gpu", "VRAM · " + it->name,
				    std::clamp(used * 100.0 / double(it->dedicatedBytes), 0.0, 100.0),
				    QString("%1 / %2 GB").arg(Gigabytes(used), Gigabytes(double(it->dedicatedBytes)))});
		}
	}
}

void SystemUsageSampler::SampleDisks(QList<UsageReading> &out)
{
	QHash<QString, double> reads, writes;
	for (const auto &[instance, value] : CounterValues(diskRead)) {
		reads[instance] = value;
	}
	for (const auto &[instance, value] : CounterValues(diskWrite)) {
		writes[instance] = value;
	}
	/* Instances are "<disk number> <drive letters>", e.g. "1 G: H:" */
	CounterItems idle = CounterValues(diskIdle);
	std::sort(idle.begin(), idle.end());
	for (const auto &[instance, value] : idle) {
		if (instance == "_Total") {
			continue;
		}
		QString number = instance.section(' ', 0, 0);
		QString letters = instance.section(' ', 1).trimmed();
		QString label = letters.isEmpty() ? QString("Disk %1").arg(number)
						  : QString("Disk %1 (%2)").arg(number, letters);
		out.append(
			{"disk:" + instance, "disk", label, std::clamp(100.0 - value, 0.0, 100.0),
			 QString("R %1 · W %2").arg(ByteRate(reads.value(instance)), ByteRate(writes.value(instance)))});
	}
}

void SystemUsageSampler::SampleNetwork(QList<UsageReading> &out)
{
	PMIB_IF_TABLE2 table = nullptr;
	if (GetIfTable2(&table) != NO_ERROR) {
		return;
	}
	int64_t now = int64_t(GetTickCount64());
	double seconds = lastNetTime ? (now - lastNetTime) / 1000.0 : 0.0;
	QHash<uint64_t, NetCount> counts;
	for (ULONG i = 0; i < table->NumEntries; i++) {
		const MIB_IF_ROW2 &row = table->Table[i];
		/* Physical adapters that are connected: no loopback, virtual switches or filter layers */
		if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || !row.InterfaceAndOperStatusFlags.HardwareInterface ||
		    row.InterfaceAndOperStatusFlags.FilterInterface || row.OperStatus != IfOperStatusUp ||
		    row.MediaConnectState != MediaConnectStateConnected) {
			continue;
		}
		uint64_t luid = row.InterfaceLuid.Value;
		counts[luid] = NetCount{row.InOctets, row.OutOctets};
		if (seconds <= 0.0 || !lastNet.contains(luid)) {
			continue;
		}
		const NetCount &last = lastNet[luid];
		double down = row.InOctets >= last.in ? (row.InOctets - last.in) * 8.0 / seconds : 0.0;
		double up = row.OutOctets >= last.out ? (row.OutOctets - last.out) * 8.0 / seconds : 0.0;
		double percent = 0.0;
		if (row.ReceiveLinkSpeed) {
			percent = std::max(percent, down * 100.0 / double(row.ReceiveLinkSpeed));
		}
		if (row.TransmitLinkSpeed) {
			percent = std::max(percent, up * 100.0 / double(row.TransmitLinkSpeed));
		}
		QString link = row.ReceiveLinkSpeed ? QString(" · %1 link").arg(BitRate(double(row.ReceiveLinkSpeed)))
						    : QString();
		out.append({"net:" + QString::number(luid, 16), "network",
			    "Network · " + QString::fromWCharArray(row.Alias), std::clamp(percent, 0.0, 100.0),
			    QString("↓ %1  ↑ %2%3").arg(BitRate(down), BitRate(up), link)});
	}
	FreeMibTable(table);
	lastNet = counts;
	lastNetTime = now;
}

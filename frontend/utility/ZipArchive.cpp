#include "ZipArchive.hpp"

#include <util/crc32.h>

#include <algorithm>
#include <limits>

using namespace std;

namespace {
constexpr uint32_t LocalHeaderSignature = 0x04034b50;
constexpr uint32_t CentralHeaderSignature = 0x02014b50;
constexpr uint32_t EndOfCentralDirSignature = 0x06054b50;
constexpr uint16_t VersionNeeded = 20;
constexpr uint16_t VersionMadeBy = 20;
constexpr uint16_t FlagUtf8 = 1 << 11;
constexpr size_t LocalHeaderSize = 30;
constexpr size_t CentralHeaderSize = 46;
constexpr size_t EndOfCentralDirSize = 22;
constexpr size_t MaxCommentSize = 0xFFFF;

void Put16(string &buf, uint16_t value)
{
	buf.push_back(char(value & 0xFF));
	buf.push_back(char((value >> 8) & 0xFF));
}

void Put32(string &buf, uint32_t value)
{
	for (int i = 0; i < 4; i++) {
		buf.push_back(char((value >> (8 * i)) & 0xFF));
	}
}

uint16_t Get16(const string &buf, size_t pos)
{
	return uint16_t(uint8_t(buf[pos]) | (uint8_t(buf[pos + 1]) << 8));
}

uint32_t Get32(const string &buf, size_t pos)
{
	return uint32_t(uint8_t(buf[pos])) | (uint32_t(uint8_t(buf[pos + 1])) << 8) |
	       (uint32_t(uint8_t(buf[pos + 2])) << 16) | (uint32_t(uint8_t(buf[pos + 3])) << 24);
}

bool LocalTime(time_t t, tm &out)
{
#ifdef _WIN32
	return localtime_s(&out, &t) == 0;
#else
	return localtime_r(&t, &out) != nullptr;
#endif
}

void ToDosTime(time_t t, uint16_t &dosTime, uint16_t &dosDate)
{
	tm local{};
	if (!LocalTime(t, local) || local.tm_year < 80) {
		/* ZIP cannot store dates before 1980 */
		dosTime = 0;
		dosDate = (1 << 5) | 1;
		return;
	}

	dosTime = uint16_t((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
	dosDate = uint16_t(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
}

time_t FromDosTime(uint16_t dosTime, uint16_t dosDate)
{
	tm local{};
	local.tm_year = ((dosDate >> 9) & 0x7F) + 80;
	local.tm_mon = ((dosDate >> 5) & 0x0F) - 1;
	local.tm_mday = dosDate & 0x1F;
	local.tm_hour = (dosTime >> 11) & 0x1F;
	local.tm_min = (dosTime >> 5) & 0x3F;
	local.tm_sec = (dosTime & 0x1F) * 2;
	local.tm_isdst = -1;
	return mktime(&local);
}
} // namespace

bool IsSafeZipEntryName(const string &name)
{
	if (name.empty() || name.size() > 0xFFFF) {
		return false;
	}
	if (name.front() == '/' || name.find('\\') != string::npos || name.find(':') != string::npos) {
		return false;
	}
	if (name.find('\0') != string::npos) {
		return false;
	}

	size_t start = 0;
	while (start <= name.size()) {
		size_t end = name.find('/', start);
		if (end == string::npos) {
			end = name.size();
		}

		string_view part(name.data() + start, end - start);
		if (part == "." || part == "..") {
			return false;
		}
		if (part.empty() && end != name.size()) {
			return false;
		}

		start = end + 1;
	}

	return true;
}

/* ------------------------------------------------------------------------- */

ZipWriter::~ZipWriter()
{
	if (out.is_open()) {
		Abort();
	}
}

bool ZipWriter::Open(const filesystem::path &file)
{
	path = file;
	entries.clear();
	offset = 0;
	error.clear();

	out.open(file, ios::binary | ios::trunc);
	if (!out.is_open()) {
		error = "Could not create the file";
		return false;
	}

	return true;
}

bool ZipWriter::Write(const string &data)
{
	out.write(data.data(), streamsize(data.size()));
	if (!out.good()) {
		error = "Could not write to the file (is the disk full?)";
		return false;
	}

	offset += data.size();
	return true;
}

bool ZipWriter::AddFile(const string &name, const string &data, time_t modified)
{
	if (!out.is_open()) {
		return false;
	}
	if (!IsSafeZipEntryName(name)) {
		error = "Invalid entry name: " + name;
		return false;
	}
	if (entries.size() >= 0xFFFF || data.size() > numeric_limits<uint32_t>::max() ||
	    offset + LocalHeaderSize + name.size() + data.size() > numeric_limits<uint32_t>::max()) {
		error = "The backup is too large (over 4 GB or 65535 files)";
		return false;
	}

	ZipEntry entry;
	entry.name = name;
	entry.crc = calc_crc32(0, data.data(), data.size());
	entry.size = uint32_t(data.size());
	entry.compressedSize = entry.size;
	entry.localHeaderOffset = uint32_t(offset);
	entry.modified = modified;

	uint16_t dosTime, dosDate;
	ToDosTime(modified, dosTime, dosDate);

	string header;
	header.reserve(LocalHeaderSize + name.size());
	Put32(header, LocalHeaderSignature);
	Put16(header, VersionNeeded);
	Put16(header, FlagUtf8);
	Put16(header, 0); /* stored */
	Put16(header, dosTime);
	Put16(header, dosDate);
	Put32(header, entry.crc);
	Put32(header, entry.compressedSize);
	Put32(header, entry.size);
	Put16(header, uint16_t(name.size()));
	Put16(header, 0);
	header.append(name);

	if (!Write(header) || !Write(data)) {
		return false;
	}

	entries.push_back(std::move(entry));
	return true;
}

bool ZipWriter::Close()
{
	if (!out.is_open()) {
		return false;
	}

	uint64_t centralStart = offset;
	string central;

	for (const ZipEntry &entry : entries) {
		uint16_t dosTime, dosDate;
		ToDosTime(entry.modified, dosTime, dosDate);

		Put32(central, CentralHeaderSignature);
		Put16(central, VersionMadeBy);
		Put16(central, VersionNeeded);
		Put16(central, FlagUtf8);
		Put16(central, 0);
		Put16(central, dosTime);
		Put16(central, dosDate);
		Put32(central, entry.crc);
		Put32(central, entry.compressedSize);
		Put32(central, entry.size);
		Put16(central, uint16_t(entry.name.size()));
		Put16(central, 0); /* extra */
		Put16(central, 0); /* comment */
		Put16(central, 0); /* disk */
		Put16(central, 0); /* internal attributes */
		Put32(central, 0); /* external attributes */
		Put32(central, entry.localHeaderOffset);
		central.append(entry.name);
	}

	if (centralStart + central.size() > numeric_limits<uint32_t>::max()) {
		error = "The backup is too large (over 4 GB)";
		Abort();
		return false;
	}

	uint32_t centralSize = uint32_t(central.size());

	Put32(central, EndOfCentralDirSignature);
	Put16(central, 0);
	Put16(central, 0);
	Put16(central, uint16_t(entries.size()));
	Put16(central, uint16_t(entries.size()));
	Put32(central, centralSize);
	Put32(central, uint32_t(centralStart));
	Put16(central, 0);

	if (!Write(central)) {
		Abort();
		return false;
	}

	out.close();
	if (out.fail()) {
		error = "Could not finish writing the file";
		error_code ec;
		filesystem::remove(path, ec);
		return false;
	}

	return true;
}

void ZipWriter::Abort()
{
	out.close();
	error_code ec;
	filesystem::remove(path, ec);
}

/* ------------------------------------------------------------------------- */

bool ZipReader::Open(const filesystem::path &file)
{
	entries.clear();
	error.clear();

	in.open(file, ios::binary);
	if (!in.is_open()) {
		error = "Could not open the file";
		return false;
	}

	in.seekg(0, ios::end);
	uint64_t fileSize = uint64_t(in.tellg());
	if (fileSize < EndOfCentralDirSize) {
		error = "This is not a zip file";
		return false;
	}

	/* The end of central directory record sits at the end of the file,
	 * followed only by an optional comment of up to 64 KiB */
	size_t tailSize = size_t(min<uint64_t>(fileSize, EndOfCentralDirSize + MaxCommentSize));
	string tail(tailSize, '\0');
	in.seekg(streamoff(fileSize - tailSize));
	in.read(tail.data(), streamsize(tailSize));
	if (!in.good()) {
		error = "Could not read the file";
		return false;
	}

	size_t eocd = string::npos;
	for (size_t pos = tailSize - EndOfCentralDirSize + 1; pos-- > 0;) {
		if (Get32(tail, pos) == EndOfCentralDirSignature) {
			eocd = pos;
			break;
		}
	}
	if (eocd == string::npos) {
		error = "This is not a zip file";
		return false;
	}

	uint16_t count = Get16(tail, eocd + 10);
	uint32_t centralSize = Get32(tail, eocd + 12);
	uint32_t centralOffset = Get32(tail, eocd + 16);
	if (uint64_t(centralOffset) + centralSize > fileSize) {
		error = "The zip file is damaged";
		return false;
	}

	string central(centralSize, '\0');
	in.seekg(streamoff(centralOffset));
	in.read(central.data(), streamsize(centralSize));
	if (!in.good()) {
		error = "Could not read the file";
		return false;
	}

	size_t pos = 0;
	for (uint16_t i = 0; i < count; i++) {
		if (pos + CentralHeaderSize > central.size() || Get32(central, pos) != CentralHeaderSignature) {
			error = "The zip file is damaged";
			return false;
		}

		ZipEntry entry;
		entry.method = Get16(central, pos + 10);
		entry.modified = FromDosTime(Get16(central, pos + 12), Get16(central, pos + 14));
		entry.crc = Get32(central, pos + 16);
		entry.compressedSize = Get32(central, pos + 20);
		entry.size = Get32(central, pos + 24);
		uint16_t nameLen = Get16(central, pos + 28);
		uint16_t extraLen = Get16(central, pos + 30);
		uint16_t commentLen = Get16(central, pos + 32);
		entry.localHeaderOffset = Get32(central, pos + 42);

		if (pos + CentralHeaderSize + nameLen + extraLen + commentLen > central.size()) {
			error = "The zip file is damaged";
			return false;
		}

		entry.name = central.substr(pos + CentralHeaderSize, nameLen);
		pos += CentralHeaderSize + nameLen + extraLen + commentLen;

		/* Folder entries carry no data */
		if (!entry.name.empty() && entry.name.back() == '/') {
			continue;
		}

		entries.push_back(std::move(entry));
	}

	return true;
}

const ZipEntry *ZipReader::Find(const string &name) const
{
	auto it = find_if(entries.begin(), entries.end(), [&](const ZipEntry &e) { return e.name == name; });
	return it == entries.end() ? nullptr : &*it;
}

bool ZipReader::Read(const ZipEntry &entry, string &data)
{
	error.clear();

	if (!IsSafeZipEntryName(entry.name)) {
		error = "Unsafe file name in backup: " + entry.name;
		return false;
	}
	if (entry.method != 0) {
		error = "\"" + entry.name +
			"\" is compressed. Backups re-zipped with another tool are not supported; "
			"use the original backup file.";
		return false;
	}

	string header(LocalHeaderSize, '\0');
	in.clear();
	in.seekg(streamoff(entry.localHeaderOffset));
	in.read(header.data(), streamsize(LocalHeaderSize));
	if (!in.good() || Get32(header, 0) != LocalHeaderSignature) {
		error = "The zip file is damaged";
		return false;
	}

	uint16_t nameLen = Get16(header, 26);
	uint16_t extraLen = Get16(header, 28);
	in.seekg(streamoff(uint64_t(entry.localHeaderOffset) + LocalHeaderSize + nameLen + extraLen));

	data.assign(entry.compressedSize, '\0');
	in.read(data.data(), streamsize(entry.compressedSize));
	if (!in.good()) {
		error = "The zip file is damaged";
		return false;
	}

	if (calc_crc32(0, data.data(), data.size()) != entry.crc) {
		error = "\"" + entry.name + "\" is damaged (checksum mismatch)";
		return false;
	}

	return true;
}

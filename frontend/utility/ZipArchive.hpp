#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

/* Minimal ZIP archive support for settings backups.
 *
 * Entries are written without compression ("stored"), which keeps the code
 * small and dependency free while producing archives that every zip tool can
 * open. The reader accepts stored entries only. */

struct ZipEntry {
	std::string name;
	uint16_t method = 0;
	uint32_t crc = 0;
	uint32_t compressedSize = 0;
	uint32_t size = 0;
	uint32_t localHeaderOffset = 0;
	time_t modified = 0;
};

class ZipWriter {
public:
	~ZipWriter();

	bool Open(const std::filesystem::path &file);
	bool AddFile(const std::string &name, const std::string &data, time_t modified);
	bool Close();
	void Abort();

	const std::string &Error() const { return error; }

private:
	std::filesystem::path path;
	std::ofstream out;
	std::vector<ZipEntry> entries;
	uint64_t offset = 0;
	std::string error;

	bool Write(const std::string &data);
};

class ZipReader {
public:
	bool Open(const std::filesystem::path &file);

	const std::vector<ZipEntry> &Entries() const { return entries; }
	const ZipEntry *Find(const std::string &name) const;
	bool Read(const ZipEntry &entry, std::string &data);

	const std::string &Error() const { return error; }

private:
	std::ifstream in;
	std::vector<ZipEntry> entries;
	std::string error;
};

/* Rejects absolute paths, drive letters, backslashes and "." or ".."
 * components so an archive can never write outside its target folder. */
bool IsSafeZipEntryName(const std::string &name);

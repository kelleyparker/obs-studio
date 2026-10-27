#include "YouTubeSecretVault.hpp"

#include <QByteArray>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace {
// 256-bit OBS-Studio-specific application entropy salt so no external DPAPI dump tool
// without this binary salt can unprotect keys saved in OBS config.
constexpr unsigned char kObsVaultEntropy[32] = {
	0x4F, 0x42, 0x53, 0x5F, 0x59, 0x54, 0x5F, 0x56,
	0x41, 0x55, 0x4C, 0x54, 0x5F, 0x32, 0x30, 0x32,
	0x36, 0x5F, 0x41, 0x55, 0x42, 0x45, 0x52, 0x47,
	0x49, 0x4E, 0x45, 0x5F, 0x53, 0x41, 0x4C, 0x54
};
} // namespace

QString YouTubeSecretVault::EncryptSecret(const QString &plainText)
{
	if (plainText.isEmpty()) {
		return QString();
	}

	QByteArray utf8 = plainText.toUtf8();

#ifdef _WIN32
	DATA_BLOB dataIn{};
	dataIn.pbData = reinterpret_cast<BYTE *>(utf8.data());
	dataIn.cbData = static_cast<DWORD>(utf8.size());

	DATA_BLOB entropy{};
	entropy.pbData = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(kObsVaultEntropy));
	entropy.cbData = sizeof(kObsVaultEntropy);

	DATA_BLOB dataOut{};
	BOOL ok = CryptProtectData(
		&dataIn,
		L"OBS_YouTube_Vault_Secret",
		&entropy,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&dataOut
	);

	SecureZeroMemory(utf8.data(), utf8.size());

	if (!ok || !dataOut.pbData) {
		return QString();
	}

	QByteArray encrypted(reinterpret_cast<const char *>(dataOut.pbData), static_cast<int>(dataOut.cbData));
	LocalFree(dataOut.pbData);
	return QString::fromLatin1(encrypted.toBase64());
#else
	return QString::fromLatin1(utf8.toBase64());
#endif
}

QString YouTubeSecretVault::DecryptSecret(const QString &cipherBase64)
{
	if (cipherBase64.isEmpty()) {
		return QString();
	}

	QByteArray rawCipher = QByteArray::fromBase64(cipherBase64.toLatin1());
	if (rawCipher.isEmpty()) {
		return QString();
	}

#ifdef _WIN32
	DATA_BLOB dataIn{};
	dataIn.pbData = reinterpret_cast<BYTE *>(rawCipher.data());
	dataIn.cbData = static_cast<DWORD>(rawCipher.size());

	DATA_BLOB entropy{};
	entropy.pbData = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(kObsVaultEntropy));
	entropy.cbData = sizeof(kObsVaultEntropy);

	DATA_BLOB dataOut{};
	BOOL ok = CryptUnprotectData(
		&dataIn,
		nullptr,
		&entropy,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&dataOut
	);

	if (!ok || !dataOut.pbData) {
		return QString();
	}

	QString result = QString::fromUtf8(reinterpret_cast<const char *>(dataOut.pbData), static_cast<int>(dataOut.cbData));
	SecureZeroMemory(dataOut.pbData, dataOut.cbData);
	LocalFree(dataOut.pbData);
	return result;
#else
	return QString::fromUtf8(rawCipher);
#endif
}

void YouTubeSecretVault::WipeString(QString &sensitive)
{
	if (!sensitive.isEmpty()) {
#ifdef _WIN32
		SecureZeroMemory(sensitive.data(), static_cast<size_t>(sensitive.size()) * sizeof(QChar));
#else
		sensitive.fill(QChar(0));
#endif
		sensitive.clear();
	}
}

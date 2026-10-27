#pragma once

#include <QString>

class YouTubeSecretVault {
public:
	// Encrypts plaintext using Windows DPAPI (CryptProtectData) bound to the current
	// Windows user + an OBS-exclusive 256-bit binary entropy salt. Returns Base64.
	static QString EncryptSecret(const QString &plainText);

	// Decrypts a Base64 DPAPI blob produced by EncryptSecret.
	static QString DecryptSecret(const QString &cipherBase64);

	// Overwrites the underlying UTF-16 buffer of a QString in memory before clearing it.
	static void WipeString(QString &sensitive);
};

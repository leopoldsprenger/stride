#pragma once
// AES-256-GCM for the git mirror: everything that reaches the remote
// (GitHub or otherwise) is ciphertext, encrypted with a key that only ever
// lives in local config -- never committed, never pushed. GCM gives
// authenticity too: a wrong key or tampered blob fails loudly (throws)
// rather than silently decrypting to garbage.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using Key = std::vector<uint8_t>;

// 32 cryptographically random bytes (AES-256 key size), from OpenSSL's RNG.
Key generateKey();

std::string keyToHex(const Key& key);
Key keyFromHex(const std::string& hex);  // throws if not exactly 64 hex chars

// Plain SHA-256 (hex), unrelated to the key -- used only to detect "did the
// plaintext actually change" before re-encrypting, since AES-GCM's nonce is
// randomized per call and so its ciphertext differs even for identical
// input. Comparing ciphertext bytes across syncs would therefore always
// look different and defeat "only commit when something changed."
std::string sha256Hex(const std::string& data);

// Layout: [4-byte magic "SKE1"][12-byte nonce][ciphertext][16-byte GCM tag].
// A fresh random nonce is generated on every call -- never reuse a nonce
// under the same key, so never try to make this deterministic.
std::string encryptBlob(const Key& key, const std::string& plaintext);

// Throws std::runtime_error on a bad magic/length (not our format), and on
// GCM tag verification failure (wrong key, or the blob was tampered with) --
// callers must not treat either as "empty content", or a corrupted or
// maliciously-replaced remote file would silently wipe local data.
std::string decryptBlob(const Key& key, const std::string& blob);

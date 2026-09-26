#include "crypto.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>

namespace {
constexpr int kKeyLen = 32;    // AES-256
constexpr int kNonceLen = 12;  // GCM standard nonce size
constexpr int kTagLen = 16;    // GCM standard tag size
const char kMagic[4] = {'S', 'K', 'E', '1'};

[[noreturn]] void throwOpenSSLError(const std::string& what) {
  unsigned long code = ERR_get_error();
  char buf[256];
  ERR_error_string_n(code, buf, sizeof(buf));
  throw std::runtime_error(what + ": " + buf);
}
}  // namespace

Key generateKey() {
  Key key(kKeyLen);
  if (RAND_bytes(key.data(), kKeyLen) != 1) throwOpenSSLError("failed to generate encryption key");
  return key;
}

std::string keyToHex(const Key& key) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(key.size() * 2);
  for (uint8_t b : key) {
    out += hex[b >> 4];
    out += hex[b & 0xf];
  }
  return out;
}

std::string sha256Hex(const std::string& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char*)data.data(), data.size(), digest);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char b : digest) {
    out += hex[b >> 4];
    out += hex[b & 0xf];
  }
  return out;
}

Key keyFromHex(const std::string& hex) {
  if (hex.size() != (size_t)kKeyLen * 2) throw std::runtime_error("encryption key must be exactly 64 hex characters");
  auto nibble = [&](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("encryption key must be hex-encoded");
  };
  Key key(kKeyLen);
  for (int i = 0; i < kKeyLen; ++i) key[i] = (uint8_t)((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
  return key;
}

std::string encryptBlob(const Key& key, const std::string& plaintext) {
  if (key.size() != (size_t)kKeyLen) throw std::runtime_error("encryption key must be 32 bytes");
  uint8_t nonce[kNonceLen];
  if (RAND_bytes(nonce, kNonceLen) != 1) throwOpenSSLError("failed to generate nonce");

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throwOpenSSLError("EVP_CIPHER_CTX_new failed");
  struct CtxGuard {
    EVP_CIPHER_CTX* c;
    ~CtxGuard() { EVP_CIPHER_CTX_free(c); }
  } guard{ctx};

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) throwOpenSSLError("EVP_EncryptInit_ex (cipher) failed");
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) != 1) throwOpenSSLError("failed to set GCM nonce length");
  if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) throwOpenSSLError("EVP_EncryptInit_ex (key/nonce) failed");

  std::string ciphertext(plaintext.size() + EVP_CIPHER_block_size(EVP_aes_256_gcm()), '\0');
  int outLen = 0, totalLen = 0;
  if (EVP_EncryptUpdate(ctx, (uint8_t*)ciphertext.data(), &outLen, (const uint8_t*)plaintext.data(), (int)plaintext.size()) != 1)
    throwOpenSSLError("EVP_EncryptUpdate failed");
  totalLen = outLen;
  if (EVP_EncryptFinal_ex(ctx, (uint8_t*)ciphertext.data() + totalLen, &outLen) != 1) throwOpenSSLError("EVP_EncryptFinal_ex failed");
  totalLen += outLen;
  ciphertext.resize(totalLen);

  uint8_t tag[kTagLen];
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag) != 1) throwOpenSSLError("failed to get GCM tag");

  std::string out;
  out.reserve(4 + kNonceLen + ciphertext.size() + kTagLen);
  out.append(kMagic, 4);
  out.append((const char*)nonce, kNonceLen);
  out.append(ciphertext);
  out.append((const char*)tag, kTagLen);
  return out;
}

std::string decryptBlob(const Key& key, const std::string& blob) {
  if (key.size() != (size_t)kKeyLen) throw std::runtime_error("encryption key must be 32 bytes");
  if (blob.size() < 4 + kNonceLen + kTagLen || memcmp(blob.data(), kMagic, 4) != 0)
    throw std::runtime_error("not a recognized encrypted mirror blob (wrong format, or file is corrupted)");

  const uint8_t* nonce = (const uint8_t*)blob.data() + 4;
  const uint8_t* ciphertext = (const uint8_t*)blob.data() + 4 + kNonceLen;
  size_t ciphertextLen = blob.size() - 4 - kNonceLen - kTagLen;
  const uint8_t* tag = (const uint8_t*)blob.data() + blob.size() - kTagLen;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throwOpenSSLError("EVP_CIPHER_CTX_new failed");
  struct CtxGuard {
    EVP_CIPHER_CTX* c;
    ~CtxGuard() { EVP_CIPHER_CTX_free(c); }
  } guard{ctx};

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) throwOpenSSLError("EVP_DecryptInit_ex (cipher) failed");
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) != 1) throwOpenSSLError("failed to set GCM nonce length");
  if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) throwOpenSSLError("EVP_DecryptInit_ex (key/nonce) failed");

  std::string plaintext(ciphertextLen, '\0');
  int outLen = 0, totalLen = 0;
  if (EVP_DecryptUpdate(ctx, (uint8_t*)plaintext.data(), &outLen, ciphertext, (int)ciphertextLen) != 1)
    throwOpenSSLError("EVP_DecryptUpdate failed");
  totalLen = outLen;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, (void*)tag) != 1) throwOpenSSLError("failed to set GCM tag");
  // A non-1 return here specifically means authentication failed -- wrong
  // key or the ciphertext was altered. This must surface as a hard error:
  // silently returning whatever partial plaintext EVP produced would let a
  // corrupted or tampered remote blob masquerade as "no data".
  int ok = EVP_DecryptFinal_ex(ctx, (uint8_t*)plaintext.data() + totalLen, &outLen);
  if (ok != 1) throw std::runtime_error("decryption failed authentication -- wrong mirror_key, or the remote content was corrupted/tampered with");
  totalLen += outLen;
  plaintext.resize(totalLen);
  return plaintext;
}

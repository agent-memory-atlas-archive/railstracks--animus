#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

namespace animus::kernel::crypto {

// Compute HMAC-SHA256 and return as hex string.
inline std::string HmacSha256Hex(const std::string& key, const std::string& message) {
    unsigned char digest[32];
    unsigned int digestLen = 0;
    HMAC(EVP_sha256(),
         reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         digest, &digestLen);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digestLen; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return ss.str();
}

// Constant-time hex string comparison.
inline bool ConstantTimeCompare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return result == 0;
}

// Generate cryptographically secure random hex string.
inline std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    RAND_bytes(buf.data(), static_cast<int>(bytes));
    std::ostringstream ss;
    for (size_t i = 0; i < bytes; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buf[i]);
    }
    return ss.str();
}

// SHA-256 hash, returned as hex string.
inline std::string Sha256Hex(const std::string& data) {
    unsigned char digest[32];
    unsigned int digestLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digestLen; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return ss.str();
}

// ---------------------------------------------------------------------------
// AES-256-GCM seal/open (#23 secrets vault). Envelope layout (hex-encoded):
//   [0]      version byte (0x01)
//   [1..12]  12-byte random nonce
//   [13..]   ciphertext || 16-byte GCM tag
// AAD is empty; the vault binds entries to packages by row, not by ciphertext.
// ---------------------------------------------------------------------------
inline std::string Aes256GcmSeal(const std::vector<unsigned char>& key32,
                                 const std::string& plaintext,
                                 std::string& error) {
    error.clear();
    if (key32.size() != 32) { error = "seal: key must be 32 bytes"; return {}; }
    unsigned char nonce[12];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) { error = "seal: RAND_bytes failed"; return {}; }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { error = "seal: ctx alloc failed"; return {}; }
    std::string envelope;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), nullptr) != 1 ||
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key32.data(), nonce) != 1) {
            error = "seal: encrypt init failed"; break;
        }
        std::vector<unsigned char> ct(plaintext.size() + 16);
        int len = 0, total = 0;
        if (!plaintext.empty() &&
            EVP_EncryptUpdate(ctx, ct.data(), &len,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) != 1) {
            error = "seal: encrypt update failed"; break;
        }
        total = len;
        if (EVP_EncryptFinal_ex(ctx, ct.data() + total, &len) != 1) {
            error = "seal: encrypt final failed"; break;
        }
        total += len;
        unsigned char tag[16];
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1) {
            error = "seal: get tag failed"; break;
        }
        static const char* hex = "0123456789abcdef";
        envelope.reserve(2 + 2 * (sizeof(nonce) + total + sizeof(tag)));
        envelope += "01";
        for (unsigned char b : nonce) { envelope += hex[b >> 4]; envelope += hex[b & 0xF]; }
        for (int i = 0; i < total; ++i) { envelope += hex[ct[i] >> 4]; envelope += hex[ct[i] & 0xF]; }
        for (unsigned char b : tag) { envelope += hex[b >> 4]; envelope += hex[b & 0xF]; }
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return envelope;
}

inline bool Aes256GcmOpen(const std::vector<unsigned char>& key32,
                          const std::string& envelopeHex,
                          std::string& plaintext,
                          std::string& error) {
    error.clear();
    plaintext.clear();
    if (key32.size() != 32) { error = "open: key must be 32 bytes"; return false; }
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (envelopeHex.size() < 2 || envelopeHex.size() % 2 != 0) {
        error = "open: malformed envelope"; return false;
    }
    // Size discipline: envelopes come from the vault DB; a hostile/garbage
    // row must not drive huge allocations. 2 MB hex = 1 MB plaintext ceiling —
    // credentials are bytes-to-KB, never more.
    if (envelopeHex.size() > 2 * 1024 * 1024) {
        error = "open: envelope exceeds size limit"; return false;
    }
    std::vector<unsigned char> raw;
    raw.reserve(envelopeHex.size() / 2);
    for (size_t i = 0; i < envelopeHex.size(); i += 2) {
        int hi = hexVal(envelopeHex[i]), lo = hexVal(envelopeHex[i + 1]);
        if (hi < 0 || lo < 0) { error = "open: malformed envelope"; return false; }
        raw.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    if (raw[0] != 0x01 || raw.size() < 1 + 12 + 16) {
        error = "open: unsupported envelope version or truncated"; return false;
    }
    const unsigned char* nonce = raw.data() + 1;
    const unsigned char* ct = raw.data() + 13;
    const size_t ctLen = raw.size() - 13 - 16;
    if (ctLen > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "open: envelope exceeds size limit"; return false;
    }
    const unsigned char* tag = ct + ctLen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { error = "open: ctx alloc failed"; return false; }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key32.data(), nonce) != 1) {
            error = "open: decrypt init failed"; break;
        }
        int len = 0, total = 0;
        std::vector<unsigned char> pt(ctLen + 16);  // decrypt target; Final may add a partial block
        if (ctLen > 0 &&
            EVP_DecryptUpdate(ctx, pt.data(), &len, ct,
                              static_cast<int>(ctLen)) != 1) {
            error = "open: decrypt update failed"; break;
        }
        total = len;
        // Supply the expected tag, then Final authenticates against it.
        // (Decrypt uses SET_TAG — GET_TAG is the encrypt-side operation.)
        unsigned char expectedTag[16];
        std::memcpy(expectedTag, tag, sizeof(expectedTag));
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, sizeof(expectedTag),
                                expectedTag) != 1) {
            error = "open: set tag failed"; break;
        }
        if (EVP_DecryptFinal_ex(ctx, pt.data() + total, &len) != 1) {
            // Auth failure — wrong key or tampered ciphertext.
            error = "open: authentication failed (wrong key or tampered ciphertext)";
            break;
        }
        total += len;
        plaintext.assign(reinterpret_cast<const char*>(pt.data()), total);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace animus::kernel::crypto

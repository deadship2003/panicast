// Y01: dependency-free crypto primitives for OAuth token at-rest encryption.
//   Why hand-rolled: the project does not link OpenSSL/libsodium, and pulling one in just for
//   token storage is heavy. These are well-known, public-domain algorithms implemented compactly.
//   - SHA-256 + HMAC-SHA256 + PBKDF2-HMAC-SHA256: key derivation from a machine-bound secret.
//   - ChaCha20 (RFC 8439): stream cipher for the tokens themselves.
//   - HMAC-SHA256 tag: integrity / tamper detection over the ciphertext.
//   The machine secret is derived from /etc/machine-id (fallback: hostname + username), so a stolen
//   db file is not directly usable on another machine. This is at-rest hardening, not a substitute
//   for OS file permissions (the db is still 0600).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace panicast
{

using bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;

// SHA-256
bytes sha256(const uint8_t *data, size_t len);
inline bytes sha256(const std::string &s) {
    return sha256(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

// HMAC-SHA256(key, msg)
bytes hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len);
bytes hmac_sha256(const bytes &key, const bytes &msg);

// PBKDF2-HMAC-SHA256: derive `out_len` bytes from password+salt.
bytes pbkdf2_hmac_sha256(const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                         uint32_t iterations, size_t out_len);

// ChaCha20 (RFC 8439) encryption/decryption — same routine for both (XOR keystream).
//   key: 32 bytes; nonce: 12 bytes; counter starts at 0.
bytes chacha20(const Key32 &key, const std::array<uint8_t, 12> &nonce, const uint8_t *in,
               size_t len);

// ── Token seal/open (high level) ────────────────────────────────────────────
//   seal:   ciphertext = ChaCha20(K, N, plaintext);  tag = HMAC(K, N || ciphertext)
//   output: base64( N || tag || ciphertext )
//   K is the machine key (AccountsManager caches it). Returns base64 text for db storage.
std::string token_seal(const Key32 &key, const std::string &plaintext);
//   open: inverse; returns false if tag mismatch (tamper / wrong machine).
bool token_open(const Key32 &key, const std::string &b64, std::string &out_plaintext);

// base64 encode/decode
std::string base64_encode(const uint8_t *data, size_t len);
bool base64_decode(const std::string &s, bytes &out);

// Derive the machine key (cached internally after first call).
Key32 machine_key();

} // namespace panicast

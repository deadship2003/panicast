// Y01: dependency-free crypto primitives (SHA-256, HMAC, PBKDF2, ChaCha20) + token seal/open.
// See header for rationale. Implementations follow the public specs (NIST FIPS 180-4, RFC 8439).
#include "panicast/core/crypto.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h> // chmod (P2-S5 per-install key file)
#include <unistd.h>

#include <fmt/format.h>

#include "panicast/core/logger.h"
#include "panicast/core/paths.h"

namespace panicast
{

// ───────────────────────── SHA-256 (FIPS 180-4) ─────────────────────────
namespace
{
inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5],
             g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
} // namespace

bytes sha256(const uint8_t *data, size_t len) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t bitlen = uint64_t(len) * 8;
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i)
        sha256_compress(state, data + i * 64);
    uint8_t buf[128];
    size_t rem = len - full * 64;
    memcpy(buf, data + full * 64, rem);
    buf[rem] = 0x80;
    size_t pad_to = (rem < 56) ? 56 : 120;
    for (size_t i = rem + 1; i < pad_to; ++i)
        buf[i] = 0;
    for (int i = 0; i < 8; ++i)
        buf[pad_to + i] = uint8_t((bitlen >> (56 - 8 * i)) & 0xff);
    sha256_compress(state, buf);
    if (pad_to == 120)
        sha256_compress(state, buf + 64);
    bytes out(32);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = uint8_t((state[i] >> 24) & 0xff);
        out[i * 4 + 1] = uint8_t((state[i] >> 16) & 0xff);
        out[i * 4 + 2] = uint8_t((state[i] >> 8) & 0xff);
        out[i * 4 + 3] = uint8_t(state[i] & 0xff);
    }
    return out;
}

// ───────────────────────── HMAC-SHA256 (RFC 2104) ─────────────────────────
bytes hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        bytes h = sha256(key, key_len);
        memcpy(k, h.data(), 32);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    bytes inner;
    inner.insert(inner.end(), ipad, ipad + 64);
    inner.insert(inner.end(), msg, msg + msg_len);
    bytes inner_hash = sha256(inner.data(), inner.size());
    bytes outer;
    outer.insert(outer.end(), opad, opad + 64);
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return sha256(outer.data(), outer.size());
}

bytes hmac_sha256(const bytes &key, const bytes &msg) {
    return hmac_sha256(key.data(), key.size(), msg.data(), msg.size());
}

// ───────────────────────── PBKDF2-HMAC-SHA256 (RFC 2898) ─────────────────────────
bytes pbkdf2_hmac_sha256(const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                         uint32_t iterations, size_t out_len) {
    bytes out;
    uint32_t i = 1;
    while (out.size() < out_len) {
        bytes msg;
        msg.insert(msg.end(), salt, salt + salt_len);
        msg.push_back(uint8_t((i >> 24) & 0xff));
        msg.push_back(uint8_t((i >> 16) & 0xff));
        msg.push_back(uint8_t((i >> 8) & 0xff));
        msg.push_back(uint8_t(i & 0xff));
        bytes u = hmac_sha256(pass, pass_len, msg.data(), msg.size());
        bytes t = u;
        for (uint32_t j = 1; j < iterations; ++j) {
            u = hmac_sha256(pass, pass_len, u.data(), u.size());
            for (size_t k = 0; k < t.size(); ++k)
                t[k] ^= u[k];
        }
        out.insert(out.end(), t.begin(), t.end());
        ++i;
    }
    out.resize(out_len);
    return out;
}

// ───────────────────────── ChaCha20 (RFC 8439) ─────────────────────────
namespace
{
inline uint32_t rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}
inline uint32_t cc_le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int r = 0; r < 10; ++r) {
        // column rounds
        x[0] += x[4];
        x[12] = rotl(x[12] ^ x[0], 16);
        x[8] += x[12];
        x[4] = rotl(x[4] ^ x[8], 12);
        x[0] += x[4];
        x[12] = rotl(x[12] ^ x[0], 8);
        x[8] += x[12];
        x[4] = rotl(x[4] ^ x[8], 7);
        x[1] += x[5];
        x[13] = rotl(x[13] ^ x[1], 16);
        x[9] += x[13];
        x[5] = rotl(x[5] ^ x[9], 12);
        x[1] += x[5];
        x[13] = rotl(x[13] ^ x[1], 8);
        x[9] += x[13];
        x[5] = rotl(x[5] ^ x[9], 7);
        x[2] += x[6];
        x[14] = rotl(x[14] ^ x[2], 16);
        x[10] += x[14];
        x[6] = rotl(x[6] ^ x[10], 12);
        x[2] += x[6];
        x[14] = rotl(x[14] ^ x[2], 8);
        x[10] += x[14];
        x[6] = rotl(x[6] ^ x[10], 7);
        x[3] += x[7];
        x[15] = rotl(x[15] ^ x[3], 16);
        x[11] += x[15];
        x[7] = rotl(x[7] ^ x[11], 12);
        x[3] += x[7];
        x[15] = rotl(x[15] ^ x[3], 8);
        x[11] += x[15];
        x[7] = rotl(x[7] ^ x[11], 7);
        // diagonal rounds
        x[0] += x[5];
        x[15] = rotl(x[15] ^ x[0], 16);
        x[10] += x[15];
        x[5] = rotl(x[5] ^ x[10], 12);
        x[0] += x[5];
        x[15] = rotl(x[15] ^ x[0], 8);
        x[10] += x[15];
        x[5] = rotl(x[5] ^ x[10], 7);
        x[1] += x[6];
        x[12] = rotl(x[12] ^ x[1], 16);
        x[11] += x[12];
        x[6] = rotl(x[6] ^ x[11], 12);
        x[1] += x[6];
        x[12] = rotl(x[12] ^ x[1], 8);
        x[11] += x[12];
        x[6] = rotl(x[6] ^ x[11], 7);
        x[2] += x[7];
        x[13] = rotl(x[13] ^ x[2], 16);
        x[8] += x[13];
        x[7] = rotl(x[7] ^ x[8], 12);
        x[2] += x[7];
        x[13] = rotl(x[13] ^ x[2], 8);
        x[8] += x[13];
        x[7] = rotl(x[7] ^ x[8], 7);
        x[3] += x[4];
        x[14] = rotl(x[14] ^ x[3], 16);
        x[9] += x[14];
        x[4] = rotl(x[4] ^ x[9], 12);
        x[3] += x[4];
        x[14] = rotl(x[14] ^ x[3], 8);
        x[9] += x[14];
        x[4] = rotl(x[4] ^ x[9], 7);
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t v = x[i] + in[i];
        out[i * 4] = uint8_t(v & 0xff);
        out[i * 4 + 1] = uint8_t((v >> 8) & 0xff);
        out[i * 4 + 2] = uint8_t((v >> 16) & 0xff);
        out[i * 4 + 3] = uint8_t((v >> 24) & 0xff);
    }
}
} // namespace

bytes chacha20(const Key32 &key, const std::array<uint8_t, 12> &nonce, const uint8_t *in,
               size_t len) {
    uint32_t state[16];
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i)
        state[4 + i] = cc_le32(key.data() + i * 4);
    state[12] = 0; // counter starts at 0
    for (int i = 0; i < 3; ++i)
        state[13 + i] = cc_le32(nonce.data() + i * 4);
    bytes out(len);
    uint8_t block[64];
    size_t off = 0;
    uint32_t counter = 0;
    while (off < len) {
        state[12] = counter++;
        chacha20_block(state, block);
        size_t n = std::min<size_t>(64, len - off);
        for (size_t i = 0; i < n; ++i)
            out[off + i] = in[off + i] ^ block[i];
        off += n;
    }
    return out;
}

// ───────────────────────── base64 ─────────────────────────
namespace
{
const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
std::string base64_encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len)
            v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len)
            v |= uint32_t(data[i + 2]);
        out.push_back(b64tab[(v >> 18) & 0x3f]);
        out.push_back(b64tab[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? b64tab[(v >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? b64tab[v & 0x3f] : '=');
    }
    return out;
}

bool base64_decode(const std::string &s, bytes &out) {
    // P2-C14: validate charset + padding and return false on malformed input (the old code silently
    //   skipped invalid chars and always returned true, making token_open's guard dead code — only
    //   the later HMAC check caught tampering). Only token_open calls this, on standard-base64
    //   sealed tokens, so strict validation is safe.
    static int8_t tbl[256];
    static std::once_flag init_flag; // P3 (Y23.7): call_once (was unsynchronized bool init)
    std::call_once(init_flag, []() {
        for (int i = 0; i < 256; ++i)
            tbl[i] = -1;
        for (int i = 0; i < 64; ++i)
            tbl[(uint8_t)b64tab[i]] = int8_t(i);
    });
    out.clear();
    if (s.empty())
        return false;
    // Reject whitespace and check padding placement ( '=' only at the end, ≤2 ).
    int val = 0, valb = -8, pad = 0;
    bool padding_started = false;
    for (char c : s) {
        if (c == '=') {
            if (padding_started) {
                if (++pad > 2)
                    return false;
            } else {
                padding_started = true;
                pad = 1;
                if (pad > 2)
                    return false;
            }
            continue;
        }
        if (padding_started)
            return false; // non-'=' char after padding
        int8_t d = tbl[(uint8_t)c];
        if (d < 0)
            return false; // invalid char
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(uint8_t((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return true;
}

// ───────────────────────── token seal/open ─────────────────────────
std::string token_seal(const Key32 &key, const std::string &plaintext) {
    // P3 (Y23.7): random nonce from /dev/urandom (was deterministic SHA-256(plaintext||counter)
    //   → same plaintext+restart = same nonce+ciphertext → leaked token equality).
    std::array<uint8_t, 12> nonce;
    {
        std::ifstream urand("/dev/urandom", std::ios::binary);
        if (urand && urand.read(reinterpret_cast<char *>(nonce.data()), 12)) {
            // got random nonce
        } else {
            // fallback: counter+time (degraded but functional)
            static std::atomic<uint64_t> counter{0};
            uint64_t c = counter.fetch_add(1) + 1;
            bytes seed =
                sha256(reinterpret_cast<const uint8_t *>(plaintext.data()), plaintext.size());
            bytes nonce_in;
            nonce_in.insert(nonce_in.end(), seed.begin(), seed.end());
            nonce_in.insert(nonce_in.end(), reinterpret_cast<uint8_t *>(&c),
                            reinterpret_cast<uint8_t *>(&c) + 8);
            bytes nh = sha256(nonce_in.data(), nonce_in.size());
            memcpy(nonce.data(), nh.data(), 12);
        }
    }

    bytes ct =
        chacha20(key, nonce, reinterpret_cast<const uint8_t *>(plaintext.data()), plaintext.size());

    bytes mac_input;
    mac_input.insert(mac_input.end(), nonce.begin(), nonce.end());
    mac_input.insert(mac_input.end(), ct.begin(), ct.end());
    bytes tag = hmac_sha256(key.data(), key.size(), mac_input.data(), mac_input.size());

    bytes bundle;
    bundle.insert(bundle.end(), nonce.begin(), nonce.end());
    bundle.insert(bundle.end(), tag.begin(), tag.end()); // 32 bytes
    bundle.insert(bundle.end(), ct.begin(), ct.end());
    return base64_encode(bundle.data(), bundle.size());
}

bool token_open(const Key32 &key, const std::string &b64, std::string &out_plaintext) {
    bytes bundle;
    if (!base64_decode(b64, bundle))
        return false;
    if (bundle.size() < 12 + 32)
        return false;
    std::array<uint8_t, 12> nonce;
    memcpy(nonce.data(), bundle.data(), 12);
    bytes tag(bundle.begin() + 12, bundle.begin() + 12 + 32);
    bytes ct(bundle.begin() + 12 + 32, bundle.end());

    bytes mac_input;
    mac_input.insert(mac_input.end(), nonce.begin(), nonce.end());
    mac_input.insert(mac_input.end(), ct.begin(), ct.end());
    bytes expect = hmac_sha256(key.data(), key.size(), mac_input.data(), mac_input.size());
    if (tag.size() != expect.size())
        return false;
    int diff = 0;
    for (size_t i = 0; i < tag.size(); ++i)
        diff |= tag[i] ^ expect[i];
    if (diff != 0)
        return false; // tamper / wrong machine

    bytes pt = chacha20(key, nonce, ct.data(), ct.size());
    out_plaintext.assign(reinterpret_cast<const char *>(pt.data()), pt.size());
    return true;
}

// ───────────────────────── machine key ─────────────────────────
// P2-S5: no hardcoded fallback secret. The key is derived from /etc/machine-id (+hostname+user).
//   On minimal/container hosts where those are all unavailable, a per-install random key is
//   generated once and stored at <data_dir>/panicast_machine_key (0600) — never a shared constant.
Key32 machine_key() {
    static Key32 k = ([]() -> Key32 {
        std::string secret;
        {
            std::ifstream f("/etc/machine-id");
            std::string s;
            if (f >> s)
                secret += s;
        }
        {
            std::ifstream f("/var/lib/dbus/machine-id");
            std::string s;
            if (f >> s)
                secret += s;
        }
        char host[256] = {0};
        gethostname(host, sizeof(host) - 1);
        secret += host;
        const char *user = getenv("USER");
        if (!user)
            user = getenv("LOGNAME");
        if (user)
            secret += user;

        if (!secret.empty()) {
            const char salt[] = "panicast-y01-token-v1";
            bytes dk = pbkdf2_hmac_sha256(reinterpret_cast<const uint8_t *>(secret.data()),
                                          secret.size(), reinterpret_cast<const uint8_t *>(salt),
                                          sizeof(salt) - 1, 100000, 32);
            Key32 out;
            memcpy(out.data(), dk.data(), 32);
            return out;
        }

        // Minimal environment: use/generate a per-install random key file (0600).
        std::string keypath = Paths::get_data_dir() + "/panicast_machine_key";
        Key32 out;
        std::ifstream kf(keypath, std::ios::binary);
        if (kf && kf.read(reinterpret_cast<char *>(out.data()), 32) && kf.gcount() == 32) {
            return out; // reuse existing per-install key
        }
        // Generate a fresh random key from /dev/urandom (fallback: libc random is NOT used).
        std::ifstream urand("/dev/urandom", std::ios::binary);
        if (!urand || !urand.read(reinterpret_cast<char *>(out.data()), 32)) {
            LOG("[crypto] CRITICAL: no machine-id AND /dev/urandom unreadable; tokens cannot be "
                "safely sealed");
            memset(out.data(), 0, 32);
            return out; // degraded; seal/open still round-trip but offer no real protection
        }
        std::ofstream of(keypath, std::ios::binary | std::ios::trunc);
        if (of) {
            of.write(reinterpret_cast<const char *>(out.data()), 32);
            of.close();
            ::chmod(keypath.c_str(), 0600);
        } else {
            LOG(fmt::format(
                "[crypto] could not persist per-install key to {} (tokens won't survive restart)",
                keypath));
        }
        return out;
    })();
    return k;
}

} // namespace panicast

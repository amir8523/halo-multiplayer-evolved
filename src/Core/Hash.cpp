// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Hash.cpp
#include "Core/Hash.h"

#include <bit>
#include <cstring>

namespace mpe::hash {
namespace {

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 256> BuildCrcTable() noexcept {
    constexpr std::uint32_t kPolynomial = 0xEDB88320u;
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) ? ((value >> 1) ^ kPolynomial) : (value >> 1);
        }
        table[i] = value;
    }
    return table;
}

constexpr auto kCrcTable = BuildCrcTable();

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

[[nodiscard]] constexpr std::uint32_t Ror(std::uint32_t value, int bits) noexcept {
    return std::rotr(value, bits);
}

[[nodiscard]] std::uint32_t LoadBigEndian32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           (static_cast<std::uint32_t>(p[3]));
}

void StoreBigEndian32(std::uint8_t* p, std::uint32_t value) noexcept {
    p[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    p[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

[[nodiscard]] bool HexNibble(char c, std::uint8_t& out) noexcept {
    if (c >= '0' && c <= '9') { out = static_cast<std::uint8_t>(c - '0');        return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<std::uint8_t>(c - 'a' + 10);   return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<std::uint8_t>(c - 'A' + 10);   return true; }
    return false;
}

} // namespace

std::uint32_t Crc32(std::span<const std::byte> data, std::uint32_t seed) noexcept {
    std::uint32_t crc = ~seed;
    for (const std::byte b : data) {
        const std::uint8_t index =
            static_cast<std::uint8_t>((crc ^ static_cast<std::uint8_t>(b)) & 0xFFu);
        crc = (crc >> 8) ^ kCrcTable[index];
    }
    return ~crc;
}

std::string ToHex(const Digest256& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2]     = kDigits[(digest[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = kDigits[digest[i] & 0x0Fu];
    }
    return out;
}

bool FromHex(std::string_view hex, Digest256& out_digest) noexcept {
    if (hex.size() != out_digest.size() * 2) {
        return false;
    }
    Digest256 parsed{};
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        std::uint8_t high = 0;
        std::uint8_t low  = 0;
        if (!HexNibble(hex[i * 2], high) || !HexNibble(hex[i * 2 + 1], low)) {
            return false;
        }
        parsed[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    out_digest = parsed;
    return true;
}

void Sha256::Reset() noexcept {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    buffer_.fill(0);
    buffered_   = 0;
    total_bits_ = 0;
    finalized_  = false;
}

void Sha256::Compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = LoadBigEndian32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1    = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        const std::uint32_t ch    = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0    = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        const std::uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::Update(std::span<const std::byte> data) noexcept {
    if (finalized_) {
        return;
    }
    total_bits_ += static_cast<std::uint64_t>(data.size()) * 8u;

    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t space = buffer_.size() - buffered_;
        const std::size_t take  = (data.size() - offset < space) ? (data.size() - offset)
                                                                 : space;
        std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
        buffered_ += take;
        offset    += take;

        if (buffered_ == buffer_.size()) {
            Compress(buffer_.data());
            buffered_ = 0;
        }
    }
}

void Sha256::Update(std::string_view text) noexcept {
    Update(std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Digest256 Sha256::Finalize() noexcept {
    Digest256 digest{};
    if (finalized_) {
        // Defensive: a second Finalize returns the zero digest rather than
        // silently producing a value derived from corrupt state.
        return digest;
    }
    finalized_ = true;

    const std::uint64_t bit_count = total_bits_;

    // Padding: 0x80, zeros, then the 64 bit big endian length.
    buffer_[buffered_++] = 0x80u;
    if (buffered_ > 56) {
        while (buffered_ < buffer_.size()) {
            buffer_[buffered_++] = 0;
        }
        Compress(buffer_.data());
        buffered_ = 0;
    }
    while (buffered_ < 56) {
        buffer_[buffered_++] = 0;
    }
    StoreBigEndian32(buffer_.data() + 56, static_cast<std::uint32_t>(bit_count >> 32));
    StoreBigEndian32(buffer_.data() + 60, static_cast<std::uint32_t>(bit_count & 0xFFFFFFFFu));
    Compress(buffer_.data());

    for (std::size_t i = 0; i < state_.size(); ++i) {
        StoreBigEndian32(digest.data() + i * 4, state_[i]);
    }
    return digest;
}

Digest256 Sha256::Compute(std::span<const std::byte> data) noexcept {
    Sha256 sha;
    sha.Update(data);
    return sha.Finalize();
}

Digest256 Sha256::Compute(std::string_view text) noexcept {
    Sha256 sha;
    sha.Update(text);
    return sha.Finalize();
}

} // namespace mpe::hash

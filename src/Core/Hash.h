// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Hash.h
//
// Deterministic hashing primitives.
//
// Two distinct jobs, two distinct algorithms:
//
//   Fnv1a64  Compile time string keys (palette lookups, message tags). Cheap,
//            not collision resistant, never used for integrity.
//   Sha256   Map variant content identity. Host and every client must agree
//            bit for bit on whether they hold the same map, and a client must
//            not be able to pass off a modified map as the host's. FNV and CRC
//            are trivially forgeable, so integrity uses SHA-256.
//   Crc32    Per chunk validation during map transfer, where the SHA-256 of
//            the whole payload is verified separately at the end. Cheap early
//            rejection of corrupt frames.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mpe::hash {

/// FNV-1a, 64 bit. constexpr so switch tables over string keys compile to
/// integer comparisons.
[[nodiscard]] constexpr std::uint64_t Fnv1a64(std::string_view text) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime       = 1099511628211ULL;

    std::uint64_t value = kOffsetBasis;
    for (const char c : text) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        value *= kPrime;
    }
    return value;
}

/// User defined literal for readable constant keys: "weapon.rocket"_fnv
[[nodiscard]] constexpr std::uint64_t operator""_fnv(const char* text,
                                                     std::size_t length) noexcept {
    return Fnv1a64(std::string_view(text, length));
}

/// CRC-32 (IEEE 802.3 polynomial, reflected).
[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> data,
                                  std::uint32_t seed = 0) noexcept;

/// SHA-256 digest, 32 bytes.
using Digest256 = std::array<std::uint8_t, 32>;

/// Lowercase hex, 64 characters. Used in the map manifest and log output.
[[nodiscard]] std::string ToHex(const Digest256& digest);

/// Parses 64 hex characters. Returns false on any non hex input or wrong
/// length, leaving out_digest untouched.
[[nodiscard]] bool FromHex(std::string_view hex, Digest256& out_digest) noexcept;

/// Incremental SHA-256. Streaming so a multi megabyte map payload never needs
/// to be contiguous in memory.
class Sha256 {
public:
    Sha256() noexcept { Reset(); }

    void Reset() noexcept;
    void Update(std::span<const std::byte> data) noexcept;
    void Update(std::string_view text) noexcept;

    /// Finalizes and returns the digest. The object must be Reset before reuse.
    [[nodiscard]] Digest256 Finalize() noexcept;

    /// One shot convenience.
    [[nodiscard]] static Digest256 Compute(std::span<const std::byte> data) noexcept;
    [[nodiscard]] static Digest256 Compute(std::string_view text) noexcept;

private:
    void Compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t                  buffered_{0};
    std::uint64_t                total_bits_{0};
    bool                         finalized_{false};
};

} // namespace mpe::hash

// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/ByteStream.h
//
// Bounds checked little endian binary serialization.
//
// Every byte that arrives from a remote peer passes through ByteReader. The
// reader can never over-read: a failed read sets a sticky failure flag and
// leaves the output parameter untouched, so a caller may issue a whole sequence
// of reads and check failed() once at the end without risking a buffer overrun
// in between. This is the single most important invariant in the networking
// code, because a malformed packet from a hostile peer must degrade to a
// disconnect and never to memory corruption.
//
// Endianness is fixed to little endian on the wire regardless of host, so a
// future console or ARM port stays compatible.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fe {

/// Upper bound for any length prefixed string on the wire. Anything longer is
/// a protocol violation, not a large name.
inline constexpr std::size_t kMaxWireStringLength = 4096;

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& buffer) noexcept : buffer_(buffer) {}

    /// Reserves capacity for an expected payload size. Purely an allocation hint.
    void Reserve(std::size_t additional) {
        buffer_.reserve(buffer_.size() + additional);
    }

    template <typename T>
    void WriteScalar(T value) {
        static_assert(std::is_trivially_copyable_v<T>, "scalar must be trivially copyable");
        static_assert(!std::is_pointer_v<T>, "pointers are never serialized");

        if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
            value = ByteSwap(value);
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        buffer_.insert(buffer_.end(), bytes, bytes + sizeof(T));
    }

    void WriteU8(std::uint8_t v)   { WriteScalar(v); }
    void WriteU16(std::uint16_t v) { WriteScalar(v); }
    void WriteU32(std::uint32_t v) { WriteScalar(v); }
    void WriteU64(std::uint64_t v) { WriteScalar(v); }
    void WriteI8(std::int8_t v)    { WriteScalar(v); }
    void WriteI16(std::int16_t v)  { WriteScalar(v); }
    void WriteI32(std::int32_t v)  { WriteScalar(v); }
    void WriteI64(std::int64_t v)  { WriteScalar(v); }
    void WriteBool(bool v)         { WriteU8(v ? 1u : 0u); }

    /// Floats are transferred as their IEEE-754 bit pattern so that no
    /// intermediate decimal conversion can perturb a coordinate.
    void WriteFloat(float v) {
        WriteU32(std::bit_cast<std::uint32_t>(v));
    }
    void WriteDouble(double v) {
        WriteU64(std::bit_cast<std::uint64_t>(v));
    }

    void WriteBytes(std::span<const std::byte> data) {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    }

    /// u16 length prefix followed by raw UTF-8. Strings longer than
    /// kMaxWireStringLength are truncated rather than rejected: the writer is
    /// only ever fed locally produced, already validated data, and silently
    /// capping is preferable to aborting a send.
    void WriteString(std::string_view text) {
        const std::size_t length =
            text.size() > kMaxWireStringLength ? kMaxWireStringLength : text.size();
        WriteU16(static_cast<std::uint16_t>(length));
        WriteBytes(std::span(reinterpret_cast<const std::byte*>(text.data()), length));
    }

    [[nodiscard]] std::size_t Size() const noexcept { return buffer_.size(); }

private:
    template <typename T>
    [[nodiscard]] static T ByteSwap(T value) noexcept {
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i) {
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
        }
        return std::bit_cast<T>(bytes);
    }

    std::vector<std::byte>& buffer_;
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    /// True once any read has failed. Sticky: never cleared.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t Remaining() const noexcept {
        return failed_ ? 0 : data_.size() - cursor_;
    }
    [[nodiscard]] std::size_t Cursor() const noexcept { return cursor_; }

    /// True when every byte was consumed and no read failed. Message handlers
    /// assert this to reject packets carrying unexpected trailing data.
    [[nodiscard]] bool AtEnd() const noexcept { return !failed_ && cursor_ == data_.size(); }

    template <typename T>
    [[nodiscard]] bool ReadScalar(T& out) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "scalar must be trivially copyable");
        if (failed_ || Remaining() < sizeof(T)) {
            failed_ = true;
            return false;
        }
        T value{};
        std::memcpy(&value, data_.data() + cursor_, sizeof(T));
        cursor_ += sizeof(T);

        if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
            value = ByteSwap(value);
        }
        out = value;
        return true;
    }

    [[nodiscard]] bool ReadU8(std::uint8_t& v)   noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadU16(std::uint16_t& v) noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadU32(std::uint32_t& v) noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadU64(std::uint64_t& v) noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadI8(std::int8_t& v)    noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadI16(std::int16_t& v)  noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadI32(std::int32_t& v)  noexcept { return ReadScalar(v); }
    [[nodiscard]] bool ReadI64(std::int64_t& v)  noexcept { return ReadScalar(v); }

    [[nodiscard]] bool ReadBool(bool& v) noexcept {
        std::uint8_t raw = 0;
        if (!ReadU8(raw)) {
            return false;
        }
        // Any value other than 0 or 1 is a protocol violation rather than truthy.
        if (raw > 1u) {
            failed_ = true;
            return false;
        }
        v = (raw == 1u);
        return true;
    }

    [[nodiscard]] bool ReadFloat(float& v) noexcept {
        std::uint32_t bits = 0;
        if (!ReadU32(bits)) {
            return false;
        }
        v = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] bool ReadDouble(double& v) noexcept {
        std::uint64_t bits = 0;
        if (!ReadU64(bits)) {
            return false;
        }
        v = std::bit_cast<double>(bits);
        return true;
    }

    /// Returns a view into the source buffer. Valid only while that buffer
    /// lives, which for received packets is the duration of the handler call.
    [[nodiscard]] bool ReadBytes(std::size_t count, std::span<const std::byte>& out) noexcept {
        if (failed_ || Remaining() < count) {
            failed_ = true;
            return false;
        }
        out = data_.subspan(cursor_, count);
        cursor_ += count;
        return true;
    }

    [[nodiscard]] bool ReadString(std::string& out, std::size_t max_length = kMaxWireStringLength) noexcept {
        std::uint16_t length = 0;
        if (!ReadU16(length)) {
            return false;
        }
        if (length > max_length) {
            failed_ = true;
            return false;
        }
        std::span<const std::byte> bytes;
        if (!ReadBytes(length, bytes)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

private:
    template <typename T>
    [[nodiscard]] static T ByteSwap(T value) noexcept {
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i) {
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
        }
        return std::bit_cast<T>(bytes);
    }

    std::span<const std::byte> data_;
    std::size_t                cursor_{0};
    bool                       failed_{false};
};

} // namespace fe

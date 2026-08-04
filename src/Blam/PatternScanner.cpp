// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/PatternScanner.cpp
#define FE_LOG_CATEGORY "Blam.Scanner"

#include "Blam/PatternScanner.h"

#include "Core/Log.h"

#include <cctype>
#include <cstring>

namespace fe::blam {
namespace {

[[nodiscard]] bool ParseHexDigit(char c, std::uint8_t& out) noexcept {
    if (c >= '0' && c <= '9') { out = static_cast<std::uint8_t>(c - '0');      return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<std::uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<std::uint8_t>(c - 'A' + 10); return true; }
    return false;
}

[[nodiscard]] bool IsIdentifierByte(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
           (uc >= '0' && uc <= '9') || c == '_' || c == '.';
}

} // namespace

// ---------------------------------------------------------------------------
// BytePattern
// ---------------------------------------------------------------------------

Expected<BytePattern> BytePattern::Parse(std::string_view text) {
    BytePattern pattern;

    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }

        if (text[i] == '?') {
            // Accept both "?" and "??".
            ++i;
            if (i < text.size() && text[i] == '?') {
                ++i;
            }
            pattern.bytes_.push_back(0);
            pattern.wildcard_.push_back(true);
            continue;
        }

        std::uint8_t high = 0;
        std::uint8_t low  = 0;
        if (i + 1 >= text.size() || !ParseHexDigit(text[i], high) ||
            !ParseHexDigit(text[i + 1], low)) {
            return Error{ErrorCode::InvalidArgument,
                         "pattern must be space separated hex pairs or wildcards"};
        }
        pattern.bytes_.push_back(static_cast<std::uint8_t>((high << 4) | low));
        pattern.wildcard_.push_back(false);
        i += 2;
    }

    if (pattern.bytes_.empty()) {
        return Error{ErrorCode::InvalidArgument, "pattern is empty"};
    }
    // A pattern that is entirely wildcards matches everywhere and is always a
    // authoring mistake.
    bool any_fixed = false;
    for (const bool wild : pattern.wildcard_) {
        any_fixed = any_fixed || !wild;
    }
    if (!any_fixed) {
        return Error{ErrorCode::InvalidArgument, "pattern contains only wildcards"};
    }

    return pattern;
}

bool BytePattern::MatchesAt(const std::byte* candidate) const noexcept {
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (wildcard_[i]) {
            continue;
        }
        if (static_cast<std::uint8_t>(candidate[i]) != bytes_[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// PatternScanner
// ---------------------------------------------------------------------------

std::vector<std::uintptr_t> PatternScanner::FindPattern(const Section& section,
                                                        const BytePattern& pattern,
                                                        std::size_t max_results) const {
    std::vector<std::uintptr_t> results;
    if (pattern.Size() == 0 || section.bytes.size() < pattern.Size()) {
        return results;
    }

    const std::byte* const data = section.bytes.data();
    const std::size_t last = section.bytes.size() - pattern.Size();
    for (std::size_t offset = 0; offset <= last; ++offset) {
        if (pattern.MatchesAt(data + offset)) {
            results.push_back(section.begin + offset);
            if (results.size() >= max_results) {
                break;
            }
        }
    }
    return results;
}

std::vector<std::uintptr_t> PatternScanner::FindStringLiteral(std::string_view text,
                                                              std::size_t max_results) const {
    std::vector<std::uintptr_t> results;
    if (text.empty()) {
        return results;
    }

    for (const Section& section : image_.Sections()) {
        // Only sections that can hold literals; skip code and relocation data.
        if (section.name != ".rdata" && section.name != ".data" && section.name != "_RDATA") {
            continue;
        }
        if (section.bytes.size() <= text.size()) {
            continue;
        }

        const char* const data = reinterpret_cast<const char*>(section.bytes.data());
        const std::size_t size = section.bytes.size();
        const std::size_t last = size - text.size() - 1; // room for trailing NUL

        for (std::size_t offset = 0; offset <= last; ++offset) {
            if (data[offset] != text[0]) {
                continue;
            }
            if (std::memcmp(data + offset, text.data(), text.size()) != 0) {
                continue;
            }
            // Require exact NUL termination.
            if (data[offset + text.size()] != '\0') {
                continue;
            }
            // Require a NUL before, so we do not match the tail of a longer
            // string. Offset zero is accepted since there is nothing before it.
            if (offset > 0 && data[offset - 1] != '\0') {
                continue;
            }

            results.push_back(section.begin + offset);
            if (results.size() >= max_results) {
                return results;
            }
        }
    }
    return results;
}

std::vector<std::uintptr_t> PatternScanner::FindPointersTo(const Section& section,
                                                            std::uintptr_t target,
                                                            std::size_t max_results) const {
    std::vector<std::uintptr_t> results;
    if (section.bytes.size() < sizeof(std::uintptr_t)) {
        return results;
    }

    // Pointers in a table are pointer aligned in every build produced by MSVC,
    // so stepping by 8 is both correct and eight times faster than by 1.
    const std::uintptr_t first_aligned =
        (section.begin + (sizeof(std::uintptr_t) - 1)) & ~static_cast<std::uintptr_t>(sizeof(std::uintptr_t) - 1);

    for (std::uintptr_t address = first_aligned;
         address + sizeof(std::uintptr_t) <= section.end;
         address += sizeof(std::uintptr_t)) {
        if (*reinterpret_cast<const std::uintptr_t*>(address) == target) {
            results.push_back(address);
            if (results.size() >= max_results) {
                break;
            }
        }
    }
    return results;
}

std::vector<std::uintptr_t> PatternScanner::FindRipRelativeReferences(
    const Section& code, std::uintptr_t target, std::size_t max_results) const {
    std::vector<std::uintptr_t> results;
    // Shortest encoding we consider is 7 bytes: REX + opcode + modrm + disp32.
    constexpr std::size_t kMinInstruction = 7;
    if (code.bytes.size() < kMinInstruction) {
        return results;
    }

    const std::byte* const data = code.bytes.data();
    const std::size_t size = code.bytes.size();

    for (std::size_t offset = 0; offset + kMinInstruction <= size; ++offset) {
        const auto b0 = static_cast<std::uint8_t>(data[offset]);
        const auto b1 = static_cast<std::uint8_t>(data[offset + 1]);
        const auto b2 = static_cast<std::uint8_t>(data[offset + 2]);

        // REX.W prefix, then lea (0x8D) or mov r64, m64 (0x8B).
        const bool rex_w  = (b0 & 0xF8u) == 0x48u;
        const bool is_op  = (b1 == 0x8Du || b1 == 0x8Bu);
        // ModRM with mod=00 and rm=101 selects RIP relative addressing.
        const bool rip_rm = (b2 & 0xC7u) == 0x05u;
        if (!rex_w || !is_op || !rip_rm) {
            continue;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, data + offset + 3, sizeof(displacement));

        // The displacement is relative to the end of the instruction.
        const std::uintptr_t instruction = code.begin + offset;
        const std::uintptr_t resolved =
            instruction + kMinInstruction + static_cast<std::intptr_t>(displacement);

        if (resolved == target) {
            results.push_back(instruction);
            if (results.size() >= max_results) {
                break;
            }
        }
    }
    return results;
}

bool PatternScanner::IsPlausibleIdentifier(std::uintptr_t address, std::size_t min_length,
                                            std::size_t max_length) const noexcept {
    if (!image_.ContainsStringAddress(address)) {
        return false;
    }
    const std::string_view text = image_.ReadCString(address, max_length + 1);
    if (text.size() < min_length || text.size() > max_length) {
        return false;
    }
    for (const char c : text) {
        if (!IsIdentifierByte(c)) {
            return false;
        }
    }
    // Engine identifiers never begin with a digit or a dot.
    const char first = text.front();
    return !(first >= '0' && first <= '9') && first != '.';
}

} // namespace fe::blam

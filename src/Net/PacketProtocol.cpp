// SPDX-License-Identifier: MIT
// ForgeEvolved: Net/PacketProtocol.cpp
#define FE_LOG_CATEGORY "Net.Protocol"

#include "Net/PacketProtocol.h"

#include "Core/Hash.h"
#include "Core/Log.h"

#include <algorithm>
#include <format>

namespace fe::net {
namespace {

/// Recognized message types. DecodePacket rejects anything absent from this set
/// before the body is touched.
[[nodiscard]] bool IsKnownType(std::uint16_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::HandshakeRequest:
        case MessageType::HandshakeAccept:
        case MessageType::HandshakeReject:
        case MessageType::Keepalive:
        case MessageType::Goodbye:
        case MessageType::RosterUpdate:
        case MessageType::MatchSettingsSync:
        case MessageType::ReadyStateChange:
        case MessageType::LaunchCountdown:
        case MessageType::LaunchNow:
        case MessageType::LoadProgress:
        case MessageType::AllPeersLoaded:
        case MessageType::MatchEnded:
        case MessageType::ChatMessage:
        case MessageType::MapManifest:
        case MessageType::MapChunkRequest:
        case MessageType::MapChunk:
        case MessageType::MapTransferDone:
        case MessageType::SimulationDatagram:
            return true;
    }
    return false;
}

/// Messages a host legitimately receives. Everything else arriving at a host is
/// either a client impersonating authority or a bug.
[[nodiscard]] bool HostAccepts(MessageType type, ProtocolPhase phase) noexcept {
    switch (type) {
        case MessageType::HandshakeRequest:
            return phase == ProtocolPhase::Handshaking || phase == ProtocolPhase::InLobby;

        case MessageType::Keepalive:
        case MessageType::Goodbye:
        case MessageType::ChatMessage:
            return true;

        case MessageType::ReadyStateChange:
            return phase == ProtocolPhase::InLobby;

        case MessageType::MapChunkRequest:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::MapTransferDone:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::LoadProgress:
            return phase == ProtocolPhase::Loading;

        case MessageType::SimulationDatagram:
            // Accepted during Loading as well: the engine begins exchanging
            // session state before the match is visibly running.
            return phase == ProtocolPhase::Loading || phase == ProtocolPhase::InMatch;

        // Host authored messages are never inbound at a host.
        case MessageType::HandshakeAccept:
        case MessageType::HandshakeReject:
        case MessageType::RosterUpdate:
        case MessageType::MatchSettingsSync:
        case MessageType::LaunchCountdown:
        case MessageType::LaunchNow:
        case MessageType::AllPeersLoaded:
        case MessageType::MatchEnded:
            return false;
    }
    return false;
}

/// Messages a client legitimately receives.
[[nodiscard]] bool ClientAccepts(MessageType type, ProtocolPhase phase) noexcept {
    switch (type) {
        case MessageType::HandshakeAccept:
        case MessageType::HandshakeReject:
            return phase == ProtocolPhase::Handshaking;

        case MessageType::Keepalive:
        case MessageType::Goodbye:
        case MessageType::ChatMessage:
            return true;

        case MessageType::RosterUpdate:
        case MessageType::MatchSettingsSync:
            // Both are legal in every post handshake phase: the roster keeps
            // updating during a match so the scoreboard stays live.
            return phase != ProtocolPhase::Handshaking;

        case MessageType::LaunchCountdown:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::LaunchNow:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::MapManifest:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::MapChunk:
            return phase == ProtocolPhase::InLobby || phase == ProtocolPhase::DistributingMap;

        case MessageType::AllPeersLoaded:
            return phase == ProtocolPhase::Loading;

        case MessageType::MatchEnded:
            return phase == ProtocolPhase::InMatch || phase == ProtocolPhase::Loading;

        case MessageType::SimulationDatagram:
            return phase == ProtocolPhase::Loading || phase == ProtocolPhase::InMatch;

        // Client authored messages are never inbound at a client.
        case MessageType::HandshakeRequest:
        case MessageType::ReadyStateChange:
        case MessageType::LoadProgress:
        case MessageType::MapChunkRequest:
        case MessageType::MapTransferDone:
            return false;
    }
    return false;
}

} // namespace

std::string_view ToString(MessageType type) noexcept {
    switch (type) {
        case MessageType::HandshakeRequest:   return "HandshakeRequest";
        case MessageType::HandshakeAccept:    return "HandshakeAccept";
        case MessageType::HandshakeReject:    return "HandshakeReject";
        case MessageType::Keepalive:          return "Keepalive";
        case MessageType::Goodbye:            return "Goodbye";
        case MessageType::RosterUpdate:       return "RosterUpdate";
        case MessageType::MatchSettingsSync:  return "MatchSettingsSync";
        case MessageType::ReadyStateChange:   return "ReadyStateChange";
        case MessageType::LaunchCountdown:    return "LaunchCountdown";
        case MessageType::LaunchNow:          return "LaunchNow";
        case MessageType::LoadProgress:       return "LoadProgress";
        case MessageType::AllPeersLoaded:     return "AllPeersLoaded";
        case MessageType::MatchEnded:         return "MatchEnded";
        case MessageType::ChatMessage:        return "ChatMessage";
        case MessageType::MapManifest:        return "MapManifest";
        case MessageType::MapChunkRequest:    return "MapChunkRequest";
        case MessageType::MapChunk:           return "MapChunk";
        case MessageType::MapTransferDone:    return "MapTransferDone";
        case MessageType::SimulationDatagram: return "SimulationDatagram";
    }
    return "Unknown";
}

bool IsMessageAcceptable(MessageType type, PeerRole local_role, ProtocolPhase phase) noexcept {
    return local_role == PeerRole::Host ? HostAccepts(type, phase) : ClientAccepts(type, phase);
}

Channel ExpectedChannel(MessageType type) noexcept {
    switch (type) {
        case MessageType::HandshakeRequest:
        case MessageType::HandshakeAccept:
        case MessageType::HandshakeReject:
        case MessageType::Keepalive:
        case MessageType::Goodbye:
            return Channel::Control;

        case MessageType::RosterUpdate:
        case MessageType::MatchSettingsSync:
        case MessageType::ReadyStateChange:
        case MessageType::LaunchCountdown:
        case MessageType::LaunchNow:
        case MessageType::LoadProgress:
        case MessageType::AllPeersLoaded:
        case MessageType::MatchEnded:
        case MessageType::ChatMessage:
            return Channel::Lobby;

        case MessageType::MapManifest:
        case MessageType::MapChunkRequest:
        case MessageType::MapChunk:
        case MessageType::MapTransferDone:
            return Channel::MapTransfer;

        case MessageType::SimulationDatagram:
            return Channel::Simulation;
    }
    return Channel::Control;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

PacketBuilder::PacketBuilder(std::vector<std::byte>& buffer, MessageType type, Channel channel)
    : writer_(buffer) {
    writer_.WriteU16(kPacketMagic);
    writer_.WriteU16(kProtocolVersion);
    writer_.WriteU16(static_cast<std::uint16_t>(type));
    writer_.WriteU8(static_cast<std::uint8_t>(channel));
    writer_.WriteU8(0); // flags, reserved
}

Expected<DecodedPacket> DecodePacket(std::span<const std::byte> datagram, Channel arrived_on) {
    if (datagram.size() < PacketHeader::kSize) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("datagram of {} bytes is shorter than the {} byte header",
                                 datagram.size(), PacketHeader::kSize)};
    }
    if (datagram.size() > kMaxPayloadBytes) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("datagram of {} bytes exceeds the {} byte ceiling",
                                 datagram.size(), kMaxPayloadBytes)};
    }

    ByteReader reader(datagram);
    std::uint16_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t raw_type = 0;
    std::uint8_t  raw_channel = 0;
    std::uint8_t  flags = 0;

    if (!reader.ReadU16(magic) || !reader.ReadU16(version) || !reader.ReadU16(raw_type) ||
        !reader.ReadU8(raw_channel) || !reader.ReadU8(flags)) {
        return Error{ErrorCode::ProtocolViolation, "header truncated"};
    }

    if (magic != kPacketMagic) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("bad magic 0x{:04X}, expected 0x{:04X}", magic, kPacketMagic)};
    }
    if (version != kProtocolVersion) {
        return Error{ErrorCode::ProtocolVersionMismatch,
                     std::format("peer speaks protocol {}, this build speaks {}", version,
                                 kProtocolVersion)};
    }
    if (!IsKnownType(raw_type)) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("unknown message type {}", raw_type)};
    }

    const auto type = static_cast<MessageType>(raw_type);

    // The declared channel must match both the arrival channel and the channel
    // the type is defined on. Two independent checks: the first catches a peer
    // lying about the channel, the second catches a peer using the wrong stream.
    const auto declared = static_cast<Channel>(raw_channel);
    if (declared != arrived_on) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("{} declares channel {} but arrived on {}", ToString(type),
                                 ToString(declared), ToString(arrived_on))};
    }
    if (ExpectedChannel(type) != arrived_on) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("{} must travel on {} but arrived on {}", ToString(type),
                                 ToString(ExpectedChannel(type)), ToString(arrived_on))};
    }

    DecodedPacket packet;
    packet.type    = type;
    packet.channel = arrived_on;
    packet.flags   = flags;
    packet.body    = datagram.subspan(PacketHeader::kSize);
    return packet;
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

void HandshakeRequestBody::Write(ByteWriter& writer) const {
    writer.WriteU32(mod_version);
    writer.WriteString(game_build);
    writer.WriteString(display_name);
    writer.WriteU64(platform_id);
}

Expected<HandshakeRequestBody> HandshakeRequestBody::Read(ByteReader& reader) {
    HandshakeRequestBody body;
    if (!reader.ReadU32(body.mod_version) ||
        !reader.ReadString(body.game_build, 128) ||
        !reader.ReadString(body.display_name, 64) ||
        !reader.ReadU64(body.platform_id)) {
        return Error{ErrorCode::ProtocolViolation, "malformed HandshakeRequest"};
    }
    if (body.game_build.empty()) {
        return Error{ErrorCode::ProtocolViolation, "HandshakeRequest has an empty game_build"};
    }
    if (body.platform_id == 0) {
        return Error{ErrorCode::ProtocolViolation, "HandshakeRequest has a zero platform_id"};
    }
    return body;
}

void HandshakeAcceptBody::Write(ByteWriter& writer) const {
    writer.WriteU8(assigned_slot);
    writer.WriteU8(assigned_team);
    writer.WriteU32(host_tick_rate);
}

Expected<HandshakeAcceptBody> HandshakeAcceptBody::Read(ByteReader& reader) {
    HandshakeAcceptBody body;
    if (!reader.ReadU8(body.assigned_slot) || !reader.ReadU8(body.assigned_team) ||
        !reader.ReadU32(body.host_tick_rate)) {
        return Error{ErrorCode::ProtocolViolation, "malformed HandshakeAccept"};
    }
    if (body.host_tick_rate == 0 || body.host_tick_rate > 240) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("implausible host_tick_rate {}", body.host_tick_rate)};
    }
    return body;
}

void HandshakeRejectBody::Write(ByteWriter& writer) const {
    writer.WriteU8(static_cast<std::uint8_t>(reason));
    writer.WriteString(detail);
}

Expected<HandshakeRejectBody> HandshakeRejectBody::Read(ByteReader& reader) {
    HandshakeRejectBody body;
    std::uint8_t raw_reason = 0;
    if (!reader.ReadU8(raw_reason) || !reader.ReadString(body.detail, 256)) {
        return Error{ErrorCode::ProtocolViolation, "malformed HandshakeReject"};
    }
    if (raw_reason > static_cast<std::uint8_t>(DisconnectReason::InternalError)) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("unknown disconnect reason {}", raw_reason)};
    }
    body.reason = static_cast<DisconnectReason>(raw_reason);
    return body;
}

void RosterEntry::Write(ByteWriter& writer) const {
    writer.WriteU64(platform_id);
    writer.WriteString(display_name);
    writer.WriteU8(slot);
    writer.WriteU8(team);
    writer.WriteBool(is_host);
    writer.WriteBool(is_ready);
    writer.WriteBool(has_map);
    writer.WriteU16(ping_milliseconds);
}

Expected<RosterEntry> RosterEntry::Read(ByteReader& reader) {
    RosterEntry entry;
    if (!reader.ReadU64(entry.platform_id) || !reader.ReadString(entry.display_name, 64) ||
        !reader.ReadU8(entry.slot) || !reader.ReadU8(entry.team) ||
        !reader.ReadBool(entry.is_host) || !reader.ReadBool(entry.is_ready) ||
        !reader.ReadBool(entry.has_map) || !reader.ReadU16(entry.ping_milliseconds)) {
        return Error{ErrorCode::ProtocolViolation, "malformed RosterEntry"};
    }
    if (entry.platform_id == 0) {
        return Error{ErrorCode::ProtocolViolation, "RosterEntry has a zero platform_id"};
    }
    return entry;
}

void RosterUpdateBody::Write(ByteWriter& writer) const {
    writer.WriteU32(revision);
    const std::size_t count = std::min(entries.size(), kMaxEntries);
    writer.WriteU8(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        entries[i].Write(writer);
    }
}

Expected<RosterUpdateBody> RosterUpdateBody::Read(ByteReader& reader) {
    RosterUpdateBody body;
    std::uint8_t count = 0;
    if (!reader.ReadU32(body.revision) || !reader.ReadU8(count)) {
        return Error{ErrorCode::ProtocolViolation, "malformed RosterUpdate header"};
    }
    if (count > kMaxEntries) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("RosterUpdate declares {} entries, ceiling is {}", count,
                                 kMaxEntries)};
    }

    body.entries.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        auto entry = RosterEntry::Read(reader);
        if (!entry.ok()) {
            return entry.error();
        }
        body.entries.push_back(std::move(entry).value());
    }
    return body;
}

void MatchSettingsBody::Write(ByteWriter& writer) const {
    writer.WriteU8(mode);
    writer.WriteString(scenario);
    writer.WriteString(variant_name);
    writer.WriteU16(score_to_win);
    writer.WriteU16(time_limit_seconds);
    writer.WriteU8(team_count);
    writer.WriteBool(friendly_fire);
    writer.WriteBool(respawn_enabled);
    writer.WriteU16(respawn_delay_seconds);
    writer.WriteU32(random_seed);
}

Expected<MatchSettingsBody> MatchSettingsBody::Read(ByteReader& reader) {
    MatchSettingsBody body;
    if (!reader.ReadU8(body.mode) || !reader.ReadString(body.scenario, 256) ||
        !reader.ReadString(body.variant_name, 128) || !reader.ReadU16(body.score_to_win) ||
        !reader.ReadU16(body.time_limit_seconds) || !reader.ReadU8(body.team_count) ||
        !reader.ReadBool(body.friendly_fire) || !reader.ReadBool(body.respawn_enabled) ||
        !reader.ReadU16(body.respawn_delay_seconds) || !reader.ReadU32(body.random_seed)) {
        return Error{ErrorCode::ProtocolViolation, "malformed MatchSettingsSync"};
    }
    return body;
}

void MapManifestBody::Write(ByteWriter& writer) const {
    writer.WriteString(map_name);
    writer.WriteString(content_hash_hex);
    writer.WriteU32(total_bytes);
    writer.WriteU32(chunk_count);
    writer.WriteString(base_scenario);
}

Expected<MapManifestBody> MapManifestBody::Read(ByteReader& reader) {
    MapManifestBody body;
    if (!reader.ReadString(body.map_name, 128) ||
        !reader.ReadString(body.content_hash_hex, 64) || !reader.ReadU32(body.total_bytes) ||
        !reader.ReadU32(body.chunk_count) || !reader.ReadString(body.base_scenario, 256)) {
        return Error{ErrorCode::ProtocolViolation, "malformed MapManifest"};
    }
    const Result validation = body.Validate();
    if (!validation.ok()) {
        return Error{validation.error()};
    }
    return body;
}

Result MapManifestBody::Validate() const {
    if (map_name.empty()) {
        return Result::Fail(ErrorCode::ValidationFailed, "manifest has an empty map_name");
    }
    if (total_bytes == 0) {
        return Result::Fail(ErrorCode::ValidationFailed, "manifest declares zero bytes");
    }
    if (total_bytes > kMaxTotalBytes) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("manifest declares {} bytes, ceiling is {}", total_bytes,
                                        kMaxTotalBytes));
    }

    hash::Digest256 parsed{};
    if (!hash::FromHex(content_hash_hex, parsed)) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            "manifest content_hash_hex is not 64 hex characters");
    }

    // chunk_count must be exactly the number of chunks total_bytes implies.
    // Trusting a peer supplied count would let a host drive a receiver's loop
    // past the end of its buffer.
    const std::uint32_t expected =
        static_cast<std::uint32_t>((total_bytes + kMapChunkBytes - 1) / kMapChunkBytes);
    if (chunk_count != expected) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("manifest declares {} chunks but {} bytes implies {}",
                                        chunk_count, total_bytes, expected));
    }
    if (base_scenario.empty()) {
        return Result::Fail(ErrorCode::ValidationFailed, "manifest has an empty base_scenario");
    }
    return Result::Success();
}

void MapChunkBody::Write(ByteWriter& writer) const {
    writer.WriteU32(chunk_index);
    writer.WriteU32(crc32);
    writer.WriteU32(static_cast<std::uint32_t>(data.size()));
    writer.WriteBytes(data);
}

Expected<MapChunkBody> MapChunkBody::Read(ByteReader& reader) {
    MapChunkBody body;
    std::uint32_t length = 0;
    if (!reader.ReadU32(body.chunk_index) || !reader.ReadU32(body.crc32) ||
        !reader.ReadU32(length)) {
        return Error{ErrorCode::ProtocolViolation, "malformed MapChunk header"};
    }
    if (length == 0 || length > kMapChunkBytes) {
        return Error{ErrorCode::ProtocolViolation,
                     std::format("MapChunk declares {} bytes, valid range is 1..{}", length,
                                 kMapChunkBytes)};
    }
    if (!reader.ReadBytes(length, body.data)) {
        return Error{ErrorCode::ProtocolViolation, "MapChunk body is shorter than declared"};
    }
    // Verify the chunk before it is ever copied into the assembly buffer.
    const std::uint32_t actual = hash::Crc32(body.data);
    if (actual != body.crc32) {
        return Error{ErrorCode::IntegrityMismatch,
                     std::format("MapChunk {} CRC mismatch: declared 0x{:08X}, computed 0x{:08X}",
                                 body.chunk_index, body.crc32, actual)};
    }
    return body;
}

void LaunchCountdownBody::Write(ByteWriter& writer) const {
    writer.WriteU8(seconds_remaining);
    writer.WriteBool(cancelled);
    writer.WriteString(cancel_reason);
}

Expected<LaunchCountdownBody> LaunchCountdownBody::Read(ByteReader& reader) {
    LaunchCountdownBody body;
    if (!reader.ReadU8(body.seconds_remaining) || !reader.ReadBool(body.cancelled) ||
        !reader.ReadString(body.cancel_reason, 128)) {
        return Error{ErrorCode::ProtocolViolation, "malformed LaunchCountdown"};
    }
    return body;
}

void LaunchNowBody::Write(ByteWriter& writer) const {
    writer.WriteString(scenario);
    writer.WriteString(map_content_hash_hex);
    writer.WriteU32(random_seed);
    writer.WriteU64(launch_epoch_milliseconds);
}

Expected<LaunchNowBody> LaunchNowBody::Read(ByteReader& reader) {
    LaunchNowBody body;
    if (!reader.ReadString(body.scenario, 256) ||
        !reader.ReadString(body.map_content_hash_hex, 64) || !reader.ReadU32(body.random_seed) ||
        !reader.ReadU64(body.launch_epoch_milliseconds)) {
        return Error{ErrorCode::ProtocolViolation, "malformed LaunchNow"};
    }
    if (body.scenario.empty()) {
        return Error{ErrorCode::ProtocolViolation, "LaunchNow has an empty scenario"};
    }
    // An empty hash means the base scenario with no variant, which is legal.
    if (!body.map_content_hash_hex.empty()) {
        hash::Digest256 parsed{};
        if (!hash::FromHex(body.map_content_hash_hex, parsed)) {
            return Error{ErrorCode::ProtocolViolation,
                         "LaunchNow map_content_hash_hex is not 64 hex characters"};
        }
    }
    return body;
}

void LoadProgressBody::Write(ByteWriter& writer) const {
    writer.WriteFloat(progress);
}

Expected<LoadProgressBody> LoadProgressBody::Read(ByteReader& reader) {
    LoadProgressBody body;
    if (!reader.ReadFloat(body.progress)) {
        return Error{ErrorCode::ProtocolViolation, "malformed LoadProgress"};
    }
    // NaN and out of range values are clamped rather than rejected: a peer with
    // a quirky progress source should not be able to end the match, and the only
    // consumer is a progress bar.
    if (!(body.progress >= 0.0f)) { // false for NaN
        body.progress = 0.0f;
    } else if (body.progress > 1.0f) {
        body.progress = 1.0f;
    }
    return body;
}

void ChatMessageBody::Write(ByteWriter& writer) const {
    writer.WriteU64(author_platform_id);
    writer.WriteString(text);
}

Expected<ChatMessageBody> ChatMessageBody::Read(ByteReader& reader) {
    ChatMessageBody body;
    if (!reader.ReadU64(body.author_platform_id) ||
        !reader.ReadString(body.text, kMaxTextLength)) {
        return Error{ErrorCode::ProtocolViolation, "malformed ChatMessage"};
    }
    // Strip control characters so a peer cannot inject newlines or terminal
    // escapes into the chat overlay or the log.
    std::erase_if(body.text, [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc < 0x20u || uc == 0x7Fu;
    });
    if (body.text.empty()) {
        return Error{ErrorCode::ProtocolViolation, "ChatMessage is empty after sanitization"};
    }
    return body;
}

} // namespace fe::net

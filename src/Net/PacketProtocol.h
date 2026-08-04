// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Net/PacketProtocol.h
//
// Wire protocol for lobby control, map distribution and simulation tunnelling.
//
// THREAT MODEL
//
// Any byte on this wire may have been produced by a hostile peer. Two rules
// follow, and both are enforced structurally rather than by convention:
//
//   1. Every decode goes through ByteReader, which cannot over-read.
//   2. Every message is checked against the receiver's current state and role
//      before its body is even parsed (see IsMessageAcceptable). A client that
//      sends LaunchMatch, or a peer that sends Ready before completing the
//      handshake, is disconnected for ProtocolViolation. Without this check a
//      client could start matches on the host.
//
// VERSIONING
//
// kProtocolVersion is compared during the handshake and a mismatch is a clean,
// explained rejection. Bump it for any change to an existing message layout.
// New message types appended at the end do not require a bump, because an older
// peer rejects an unknown type as a violation only after the handshake has
// already established that both sides run the same version.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/ByteStream.h"
#include "Core/Result.h"
#include "Net/IPeerTransport.h"

namespace mpe::net {

/// Identifies a MultiplayerEvolved packet. Guards against a stray datagram from an
/// unrelated application on the same relay socket being parsed as lobby traffic.
inline constexpr std::uint16_t kPacketMagic = 0xFE07u;

/// Incremented on any incompatible change to an existing message layout.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Largest payload we will accept on any channel. Map transfer is chunked well
/// below this; anything larger is rejected before allocation.
inline constexpr std::size_t kMaxPayloadBytes = 60 * 1024;

/// Payload bytes per map transfer chunk. Chosen to sit under the relay's
/// reliable segment size so a chunk is one round trip rather than a fragmented
/// series, while keeping per chunk overhead negligible.
inline constexpr std::size_t kMapChunkBytes = 16 * 1024;

/// Message identifiers. Explicitly numbered: these values are on the wire and
/// must never be reordered.
enum class MessageType : std::uint16_t {
    // --- Control (Channel::Control) --------------------------------------
    HandshakeRequest  = 1,  ///< Client to host, first packet on a connection.
    HandshakeAccept   = 2,  ///< Host to client, assigns a slot.
    HandshakeReject   = 3,  ///< Host to client, carries a reason and detail.
    Keepalive         = 4,  ///< Either direction.
    Goodbye           = 5,  ///< Clean disconnect notice.

    // --- Lobby (Channel::Lobby) ------------------------------------------
    RosterUpdate      = 20, ///< Host to all, full roster snapshot.
    MatchSettingsSync = 21, ///< Host to all, authoritative settings.
    ReadyStateChange  = 22, ///< Client to host, request; host echoes via roster.
    LaunchCountdown   = 23, ///< Host to all, countdown started or cancelled.
    LaunchNow         = 24, ///< Host to all, begin loading.
    LoadProgress      = 25, ///< Client to host, zero to one.
    AllPeersLoaded    = 26, ///< Host to all, release the simulation.
    MatchEnded        = 27, ///< Host to all, return to lobby.
    ChatMessage       = 28, ///< Either direction, host relays.

    // --- Map transfer (Channel::MapTransfer) ------------------------------
    MapManifest       = 40, ///< Host to all: identity, size, chunk count.
    MapChunkRequest   = 41, ///< Client to host: request a specific chunk.
    MapChunk          = 42, ///< Host to client: one chunk with its CRC.
    MapTransferDone   = 43, ///< Client to host: full payload verified.

    // --- Simulation tunnel (Channel::Simulation) --------------------------
    /// Opaque engine datagram. MultiplayerEvolved does not interpret the body; it
    /// carries the engine's own session and simulation traffic over Steam so the
    /// shipped replication, interpolation and priority systems work unchanged.
    SimulationDatagram = 60,
};

[[nodiscard]] std::string_view ToString(MessageType type) noexcept;

/// Fixed 8 byte header prefixed to every payload.
struct PacketHeader {
    std::uint16_t magic{kPacketMagic};
    std::uint16_t version{kProtocolVersion};
    std::uint16_t type{0};
    std::uint8_t  channel{0};
    std::uint8_t  flags{0};

    static constexpr std::size_t kSize = 8;
};

/// Role of the machine doing the checking. Determines which messages are legal
/// to receive.
enum class PeerRole : std::uint8_t { Host = 0, Client };

/// Receiver side lobby phase, used for the state gate. Mirrors the externally
/// visible phases of LobbyManager without depending on it, so the protocol layer
/// stays free of lobby internals.
enum class ProtocolPhase : std::uint8_t {
    Handshaking = 0,
    InLobby,
    DistributingMap,
    Loading,
    InMatch,
};

/// True when a message of this type is legal to receive given the local role and
/// phase. This is the authorization matrix for the whole protocol.
[[nodiscard]] bool IsMessageAcceptable(MessageType type, PeerRole local_role,
                                       ProtocolPhase phase) noexcept;

/// Channel a message type must arrive on. Receiving a type on the wrong channel
/// is a violation, which stops a bulk map chunk from being injected as control
/// traffic.
[[nodiscard]] Channel ExpectedChannel(MessageType type) noexcept;

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/// Builds a packet: header followed by the body written by the caller.
///
/// Usage:
///   std::vector<std::byte> packet;
///   PacketBuilder builder(packet, MessageType::ReadyStateChange, Channel::Lobby);
///   builder.Body().WriteBool(true);
class PacketBuilder {
public:
    PacketBuilder(std::vector<std::byte>& buffer, MessageType type, Channel channel);

    [[nodiscard]] ByteWriter& Body() noexcept { return writer_; }

private:
    ByteWriter writer_;
};

/// Decoded header plus a view of the remaining body.
struct DecodedPacket {
    MessageType                type{};
    Channel                    channel{};
    std::uint8_t               flags{0};
    std::span<const std::byte> body;
};

/// Validates and splits a received datagram.
///
/// Rejects: short frames, wrong magic, version mismatch, unknown message type,
/// a type arriving on the wrong channel, and oversized payloads. Everything past
/// this function is guaranteed to be a well formed packet of a known type on its
/// correct channel; it is still untrusted data.
[[nodiscard]] Expected<DecodedPacket> DecodePacket(std::span<const std::byte> datagram,
                                                   Channel arrived_on);

// ---------------------------------------------------------------------------
// Message bodies
// ---------------------------------------------------------------------------

/// Client to host, opening a connection.
struct HandshakeRequestBody {
    std::uint32_t mod_version{0};      ///< MultiplayerEvolved build, informational.
    std::string   game_build;          ///< Must match the host exactly.
    std::string   display_name;        ///< Steam persona, trimmed.
    std::uint64_t platform_id{0};      ///< Claimed identity, cross checked by
                                       ///< the host against the transport's
                                       ///< authenticated identity.

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<HandshakeRequestBody> Read(ByteReader& reader);
};

struct HandshakeAcceptBody {
    std::uint8_t  assigned_slot{0};
    std::uint8_t  assigned_team{0};
    std::uint32_t host_tick_rate{0};

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<HandshakeAcceptBody> Read(ByteReader& reader);
};

struct HandshakeRejectBody {
    DisconnectReason reason{DisconnectReason::InternalError};
    std::string      detail;

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<HandshakeRejectBody> Read(ByteReader& reader);
};

/// One entry in a roster snapshot.
struct RosterEntry {
    std::uint64_t platform_id{0};
    std::string   display_name;
    std::uint8_t  slot{0};
    std::uint8_t  team{0};
    bool          is_host{false};
    bool          is_ready{false};
    bool          has_map{false};
    std::uint16_t ping_milliseconds{0};

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<RosterEntry> Read(ByteReader& reader);
};

/// Host to all. A full snapshot rather than a delta: the roster is small and
/// bounded, and a snapshot cannot desynchronize the way a missed delta can.
struct RosterUpdateBody {
    std::uint32_t            revision{0}; ///< Monotonic; clients ignore stale ones.
    std::vector<RosterEntry> entries;

    /// Hard cap so a hostile host cannot force a client to allocate without
    /// bound. Well above any real player count.
    static constexpr std::size_t kMaxEntries = 32;

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<RosterUpdateBody> Read(ByteReader& reader);
};

/// Host to all. Mirrors engine::MatchSettings as plain scalars so the protocol
/// layer stays independent of the engine abstraction; LobbyManager converts.
struct MatchSettingsBody {
    std::uint8_t  mode{0};
    std::string   scenario;
    std::string   variant_name;
    std::uint16_t score_to_win{0};
    std::uint16_t time_limit_seconds{0};
    std::uint8_t  team_count{0};
    bool          friendly_fire{false};
    bool          respawn_enabled{true};
    std::uint16_t respawn_delay_seconds{0};
    std::uint32_t random_seed{0};

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<MatchSettingsBody> Read(ByteReader& reader);
};

struct MapManifestBody {
    std::string   map_name;
    std::string   content_hash_hex; ///< SHA-256 of the canonical payload.
    std::uint32_t total_bytes{0};
    std::uint32_t chunk_count{0};
    std::string   base_scenario;

    /// Ceiling on a transferable map. A legitimate variant is far smaller; this
    /// stops a malicious host from announcing a gigabyte transfer.
    static constexpr std::uint32_t kMaxTotalBytes = 8u * 1024u * 1024u;

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<MapManifestBody> Read(ByteReader& reader);

    /// Checks internal consistency: chunk_count must match total_bytes, the hash
    /// must be well formed, and the size must be within bounds.
    [[nodiscard]] Result Validate() const;
};

struct MapChunkBody {
    std::uint32_t              chunk_index{0};
    std::uint32_t              crc32{0};
    std::span<const std::byte> data; ///< View into the received datagram.

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<MapChunkBody> Read(ByteReader& reader);
};

struct LaunchCountdownBody {
    std::uint8_t seconds_remaining{0};
    bool         cancelled{false};
    std::string  cancel_reason;

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<LaunchCountdownBody> Read(ByteReader& reader);
};

struct LaunchNowBody {
    std::string   scenario;
    std::string   map_content_hash_hex;
    std::uint32_t random_seed{0};
    std::uint64_t launch_epoch_milliseconds{0}; ///< Host clock at launch.

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<LaunchNowBody> Read(ByteReader& reader);
};

struct LoadProgressBody {
    float progress{0.0f}; ///< Clamped to zero..one on read.

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<LoadProgressBody> Read(ByteReader& reader);
};

struct ChatMessageBody {
    std::uint64_t author_platform_id{0};
    std::string   text;

    static constexpr std::size_t kMaxTextLength = 256;

    void Write(ByteWriter& writer) const;
    [[nodiscard]] static Expected<ChatMessageBody> Read(ByteReader& reader);
};

} // namespace mpe::net

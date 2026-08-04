// SPDX-License-Identifier: MIT
// ForgeEvolved: Net/IPeerTransport.h
//
// Transport abstraction. This is the interface that makes the dedicated server
// migration a configuration change rather than a rewrite.
//
// TOPOLOGY
//
// Listen server. One peer is the host and owns the authoritative simulation;
// every other peer connects to it and nowhere else. There is no peer to peer
// mesh, so a client only ever has one connection and the host's outbound
// bandwidth is the constraint the lobby manages.
//
// Concrete implementations:
//
//   SteamSocketsTransport   ISteamNetworkingSockets over the Steam Datagram
//                           Relay. Players connect by Steam identity, so no
//                           port forwarding, no public IP exposure, and Valve's
//                           relay network absorbs the NAT problem that the
//                           engine's own strings show it otherwise hits
//                           (join_failed_unable_to_connect_party_strict_nat).
//
//   Future SteamIPTransport Same interface bound to a listen address, for a
//                           headless dedicated server. Nothing above this
//                           header changes.
//
//   LoopbackTransport       In process, for tests. Runs the entire lobby state
//                           machine with no Steam client and no game.
//
// THREADING
//
// Implementations are not thread safe. Every call, including Poll, happens on
// the single thread that owns the mod tick. Steam callbacks are drained inside
// Poll so observer notifications arrive on that same thread, which is what lets
// LobbyManager be written without a single lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

namespace fe::net {

/// Local identifier for a connected peer. Stable for the lifetime of the
/// connection and never reused within a session. Zero is never valid.
enum class PeerHandle : std::uint32_t { Invalid = 0 };

/// Platform identity of a peer. On Steam this wraps a CSteamID; the raw 64 bit
/// value is kept opaque here so no Steam header leaks above the transport.
struct PeerIdentity {
    std::uint64_t platform_id{0};

    [[nodiscard]] bool IsValid() const noexcept { return platform_id != 0; }
    [[nodiscard]] bool operator==(const PeerIdentity& other) const noexcept {
        return platform_id == other.platform_id;
    }
};

/// Delivery guarantee for one send.
enum class SendMode : std::uint8_t {
    /// Dropped freely, may arrive out of order. The engine's own simulation
    /// stream is already built for loss, so tunnelled simulation traffic uses
    /// this and never pays for retransmission of state that is already stale.
    Unreliable = 0,

    /// Dropped freely, but a datagram older than one already delivered is
    /// discarded rather than delivered late.
    UnreliableSequenced,

    /// Retransmitted and delivered in order. Lobby control traffic and map
    /// transfer only.
    Reliable,
};

/// Logical stream. Separating control from bulk transfer means a multi megabyte
/// map download cannot delay a ready-up or a launch message.
enum class Channel : std::uint8_t {
    Control     = 0, ///< Handshake, keepalive, disconnect reason.
    Lobby       = 1, ///< Roster, settings, ready state, launch.
    MapTransfer = 2, ///< Chunked map variant payloads.
    Simulation  = 3, ///< Opaque engine datagrams, tunnelled verbatim.
};

[[nodiscard]] std::string_view ToString(Channel channel) noexcept;

/// Why a connection ended. Surfaced to the user, so each value maps to a
/// distinct, actionable message rather than a generic failure.
enum class DisconnectReason : std::uint8_t {
    LocalRequest = 0,   ///< This machine closed the connection.
    RemoteRequest,      ///< The other end closed it cleanly.
    Timeout,            ///< No traffic within the keepalive window.
    ProtocolViolation,  ///< Malformed or out of state traffic.
    VersionMismatch,    ///< Incompatible mod or game build.
    Kicked,             ///< Host removed this peer.
    HostShutdown,       ///< Host left, ending the session.
    RelayFailure,       ///< Steam relay could not sustain a route.
    InternalError,
};

[[nodiscard]] std::string_view ToString(DisconnectReason reason) noexcept;

/// Live quality figures for one connection. The lobby uses these to warn before
/// a match starts rather than after it degrades.
struct PeerStats {
    std::uint32_t ping_milliseconds{0};
    float         packet_loss_out{0.0f}; ///< Zero to one.
    float         packet_loss_in{0.0f};
    std::uint32_t pending_reliable_bytes{0};
    std::uint32_t send_rate_bytes_per_second{0};
    bool          is_relayed{false};     ///< True when traffic goes via SDR.
};

/// Configuration for opening a host endpoint.
struct ListenConfig {
    /// Maximum simultaneous clients, excluding the host. Enforced by the
    /// transport so a rejected connection never reaches lobby logic.
    std::uint32_t max_clients{15};

    /// Milliseconds without traffic before a peer is dropped.
    std::uint32_t timeout_milliseconds{15000};

    /// When true the transport advertises a relay backed listen socket, which is
    /// what allows connections without port forwarding. Only turn this off for
    /// a dedicated server with a routable address.
    bool use_relay{true};
};

/// Notifications raised during Poll. Implemented by SessionHost and
/// SessionClient; LobbyManager receives them through those.
class ITransportObserver {
public:
    virtual ~ITransportObserver() = default;

    /// A new peer finished connecting. On the host this is an inbound client; on
    /// a client this is the host.
    virtual void OnPeerConnected(PeerHandle peer, const PeerIdentity& identity) = 0;

    /// The peer is gone. No further sends to this handle will succeed.
    virtual void OnPeerDisconnected(PeerHandle peer, DisconnectReason reason,
                                    std::string_view detail) = 0;

    /// One complete message. The payload view is valid only for the duration of
    /// this call; handlers that need to retain it must copy.
    virtual void OnPacketReceived(PeerHandle peer, Channel channel,
                                  std::span<const std::byte> payload) = 0;

    /// A connection attempt this machine initiated failed before completing.
    virtual void OnConnectFailed(DisconnectReason reason, std::string_view detail) = 0;
};

/// What a given implementation is.
enum class TransportKind : std::uint8_t {
    SteamRelay = 0,
    DirectAddress,
    Loopback,
};

class IPeerTransport {
public:
    virtual ~IPeerTransport() = default;

    [[nodiscard]] virtual TransportKind Kind() const noexcept = 0;

    /// The identity this machine presents to peers. Required before hosting so
    /// the lobby can publish it for clients to connect to.
    [[nodiscard]] virtual Expected<PeerIdentity> LocalIdentity() const = 0;

    // --- Endpoint lifecycle ----------------------------------------------
    /// Opens a host endpoint. Fails if already listening or connecting.
    [[nodiscard]] virtual Result Listen(const ListenConfig& config) = 0;

    /// Connects to a host. Completion is asynchronous: success is reported
    /// through OnPeerConnected and failure through OnConnectFailed.
    [[nodiscard]] virtual Result Connect(const PeerIdentity& host,
                                        std::uint32_t timeout_milliseconds) = 0;

    /// Closes every connection and releases the endpoint. Safe to call in any
    /// state, including from an error path, and idempotent.
    virtual void Shutdown() = 0;

    [[nodiscard]] virtual bool IsListening() const noexcept = 0;
    [[nodiscard]] virtual std::size_t PeerCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<PeerHandle> Peers() const = 0;
    [[nodiscard]] virtual Expected<PeerIdentity> IdentityOf(PeerHandle peer) const = 0;

    // --- Data ------------------------------------------------------------
    [[nodiscard]] virtual Result Send(PeerHandle peer, Channel channel,
                                     std::span<const std::byte> payload, SendMode mode) = 0;

    /// Sends to every connected peer. A failure to reach one peer does not stop
    /// the rest; the first error encountered is returned after all attempts.
    [[nodiscard]] virtual Result Broadcast(Channel channel, std::span<const std::byte> payload,
                                          SendMode mode) = 0;

    /// Same as Broadcast but skips one peer, for relaying a client's message to
    /// everyone except its author.
    [[nodiscard]] virtual Result BroadcastExcept(PeerHandle exclude, Channel channel,
                                                 std::span<const std::byte> payload,
                                                 SendMode mode) = 0;

    /// Flushes any coalesced sends immediately. Called before a launch message
    /// so the transition is not delayed by Nagle style batching.
    virtual void Flush() = 0;

    // --- Pump ------------------------------------------------------------
    /// Drains platform callbacks and delivers received packets to the observer.
    /// Must be called every tick. All observer callbacks happen inside this call.
    virtual void Poll(ITransportObserver& observer) = 0;

    /// Closes one connection with a reason the remote end will see.
    virtual void Disconnect(PeerHandle peer, DisconnectReason reason,
                            std::string_view detail) = 0;

    [[nodiscard]] virtual Expected<PeerStats> QueryStats(PeerHandle peer) const = 0;
};

} // namespace fe::net

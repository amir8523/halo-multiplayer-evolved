// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Net/ISimulationSink.h
//
// Delivery point for tunnelled engine datagrams.
//
// The engine already owns a complete replication layer: object interpolation,
// per object simulation priority, per peer stream bandwidth, and its own session
// state machine. Reimplementing any of that would be strictly worse than what
// ships in the binary. So MultiplayerEvolved carries the engine's datagrams instead of
// replacing them, and this interface is where a received datagram crosses back
// into the engine.
//
// Separating this from LobbyManager is deliberate. The lobby owns policy: who is
// allowed in, what the rules are, when the match starts. The simulation tunnel is
// pure conveyance and must stay on the fastest possible path with no lobby logic
// in the way. Keeping the two apart also means the tunnel can be absent (during
// lobby, or before the engine bridge has resolved) without the lobby caring.
#pragma once

#include <span>

#include "Core/Result.h"
#include "Net/IPeerTransport.h"

namespace mpe::net {

class ISimulationSink {
public:
    virtual ~ISimulationSink() = default;

    /// True once the engine side is able to accept datagrams. False during the
    /// lobby and while the engine is between scenarios, in which case the lobby
    /// drops simulation traffic rather than buffering it: the engine's own
    /// recovery is built for loss, and a buffer would deliver stale state.
    [[nodiscard]] virtual bool IsReady() const noexcept = 0;

    /// Hands one datagram to the engine's session layer, attributed to the peer
    /// it authenticably came from.
    ///
    /// The span is valid only for the duration of the call.
    [[nodiscard]] virtual Result DeliverToEngine(PeerHandle from,
                                                std::span<const std::byte> datagram) = 0;
};

} // namespace mpe::net

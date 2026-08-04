// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Net/IPeerTransport.cpp
//
// Enum names for the transport vocabulary. Kept in its own translation unit so
// the interface header stays free of definitions.
#include "Net/IPeerTransport.h"

namespace mpe::net {

std::string_view ToString(Channel channel) noexcept {
    switch (channel) {
        case Channel::Control:     return "control";
        case Channel::Lobby:       return "lobby";
        case Channel::MapTransfer: return "map_transfer";
        case Channel::Simulation:  return "simulation";
    }
    return "unknown";
}

std::string_view ToString(DisconnectReason reason) noexcept {
    switch (reason) {
        case DisconnectReason::LocalRequest:      return "local_request";
        case DisconnectReason::RemoteRequest:     return "remote_request";
        case DisconnectReason::Timeout:           return "timeout";
        case DisconnectReason::ProtocolViolation: return "protocol_violation";
        case DisconnectReason::VersionMismatch:   return "version_mismatch";
        case DisconnectReason::Kicked:            return "kicked";
        case DisconnectReason::HostShutdown:      return "host_shutdown";
        case DisconnectReason::RelayFailure:      return "relay_failure";
        case DisconnectReason::InternalError:     return "internal_error";
    }
    return "unknown";
}

} // namespace mpe::net

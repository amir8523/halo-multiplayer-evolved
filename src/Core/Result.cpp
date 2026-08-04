// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/Result.cpp
#include "Core/Result.h"

namespace fe {

std::string_view ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:                    return "None";
        case ErrorCode::InvalidArgument:         return "InvalidArgument";
        case ErrorCode::InvalidState:            return "InvalidState";
        case ErrorCode::NotFound:                return "NotFound";
        case ErrorCode::AlreadyExists:           return "AlreadyExists";
        case ErrorCode::Timeout:                 return "Timeout";
        case ErrorCode::Cancelled:               return "Cancelled";
        case ErrorCode::OutOfMemory:             return "OutOfMemory";
        case ErrorCode::NotImplemented:          return "NotImplemented";
        case ErrorCode::ModuleNotLoaded:         return "ModuleNotLoaded";
        case ErrorCode::SectionNotFound:         return "SectionNotFound";
        case ErrorCode::SymbolNotResolved:       return "SymbolNotResolved";
        case ErrorCode::SymbolValidationFailed:  return "SymbolValidationFailed";
        case ErrorCode::IncompatibleGameBuild:   return "IncompatibleGameBuild";
        case ErrorCode::ParseError:              return "ParseError";
        case ErrorCode::SchemaMismatch:          return "SchemaMismatch";
        case ErrorCode::ValidationFailed:        return "ValidationFailed";
        case ErrorCode::IntegrityMismatch:       return "IntegrityMismatch";
        case ErrorCode::FileNotFound:            return "FileNotFound";
        case ErrorCode::FileReadError:           return "FileReadError";
        case ErrorCode::FileWriteError:          return "FileWriteError";
        case ErrorCode::TransportUnavailable:    return "TransportUnavailable";
        case ErrorCode::TransportSendFailed:     return "TransportSendFailed";
        case ErrorCode::PeerNotFound:            return "PeerNotFound";
        case ErrorCode::PeerRejected:            return "PeerRejected";
        case ErrorCode::ProtocolViolation:       return "ProtocolViolation";
        case ErrorCode::ProtocolVersionMismatch: return "ProtocolVersionMismatch";
        case ErrorCode::SteamUnavailable:        return "SteamUnavailable";
        case ErrorCode::SteamCallFailed:         return "SteamCallFailed";
        case ErrorCode::LobbyUnavailable:        return "LobbyUnavailable";
    }
    return "Unknown";
}

} // namespace fe

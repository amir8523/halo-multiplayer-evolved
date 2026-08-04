// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Result.h
//
// Exception-free error propagation.
//
// The Blam simulation DLL is compiled without exception support across its ABI
// boundary, and unwinding through engine frames corrupts its internal state.
// Every fallible operation in this project therefore returns Result or
// Expected<T> and never throws. Any third party call that may throw is wrapped
// at the call site (see MapVariantParser.cpp for the canonical pattern).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mpe {

/// Coarse failure classification. Callers branch on this; the message carries
/// the human readable detail. Kept flat and stable so it can be logged as an
/// integer and mapped in crash reports.
enum class ErrorCode : std::uint16_t {
    None = 0,

    // Generic
    InvalidArgument,
    InvalidState,
    NotFound,
    AlreadyExists,
    Timeout,
    Cancelled,
    OutOfMemory,
    NotImplemented,

    // Host process / binary introspection
    ModuleNotLoaded,
    SectionNotFound,
    SymbolNotResolved,
    SymbolValidationFailed,
    IncompatibleGameBuild,

    // Serialization
    ParseError,
    SchemaMismatch,
    ValidationFailed,
    IntegrityMismatch,

    // Filesystem
    FileNotFound,
    FileReadError,
    FileWriteError,

    // Networking
    TransportUnavailable,
    TransportSendFailed,
    PeerNotFound,
    PeerRejected,
    ProtocolViolation,
    ProtocolVersionMismatch,

    // Platform (Steam)
    SteamUnavailable,
    SteamCallFailed,
    LobbyUnavailable,
};

[[nodiscard]] std::string_view ToString(ErrorCode code) noexcept;

/// A failure value: code plus a caller supplied description.
struct Error {
    ErrorCode   code{ErrorCode::None};
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) noexcept : code(c), message(std::move(msg)) {}
    explicit Error(ErrorCode c) : code(c), message(std::string(ToString(c))) {}
};

/// Result of an operation that yields no value.
///
/// Deliberately implicitly convertible to bool so call sites read naturally,
/// and marked nodiscard so a dropped failure is a compile warning.
class [[nodiscard]] Result {
public:
    Result() = default;                                   ///< Success.
    Result(Error err) : error_(std::move(err)) {}         // NOLINT: implicit by design
    Result(ErrorCode code, std::string msg) : error_(Error{code, std::move(msg)}) {}

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    /// Precondition: !ok(). Guarded by IsOk checks at every call site.
    [[nodiscard]] const Error& error() const noexcept { return *error_; }
    [[nodiscard]] ErrorCode code() const noexcept {
        return error_ ? error_->code : ErrorCode::None;
    }
    [[nodiscard]] std::string_view message() const noexcept {
        return error_ ? std::string_view(error_->message) : std::string_view{};
    }

    static Result Success() noexcept { return Result{}; }
    static Result Fail(ErrorCode code, std::string msg) {
        return Result{code, std::move(msg)};
    }

private:
    std::optional<Error> error_;
};

/// Result of an operation that yields a value on success.
template <typename T>
class [[nodiscard]] Expected {
public:
    Expected(T value) : value_(std::move(value)) {}       // NOLINT: implicit by design
    Expected(Error err) : error_(std::move(err)) {}       // NOLINT: implicit by design
    Expected(ErrorCode code, std::string msg) : error_(Error{code, std::move(msg)}) {}

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    /// Precondition: ok().
    [[nodiscard]] T&       value() &       noexcept { return *value_; }
    [[nodiscard]] const T& value() const & noexcept { return *value_; }
    [[nodiscard]] T&&      value() &&      noexcept { return std::move(*value_); }

    /// Precondition: !ok().
    [[nodiscard]] const Error& error() const noexcept { return *error_; }
    [[nodiscard]] ErrorCode code() const noexcept {
        return error_ ? error_->code : ErrorCode::None;
    }
    [[nodiscard]] std::string_view message() const noexcept {
        return error_ ? std::string_view(error_->message) : std::string_view{};
    }

    /// Discards the value, keeping only success or failure.
    [[nodiscard]] Result AsResult() const {
        return ok() ? Result::Success() : Result{*error_};
    }

private:
    std::optional<T>     value_;
    std::optional<Error> error_;
};

/// Propagate a failure out of the current function.
///
/// Usage:
///   MPE_TRY(registry.Resolve());
#define MPE_TRY(expr)                                                           \
    do {                                                                       \
        ::mpe::Result fe_try_result_ = (expr);                                   \
        if (!fe_try_result_.ok()) {                                             \
            return ::mpe::Result{fe_try_result_.error()};                        \
        }                                                                       \
    } while (false)

/// Propagate a failure out of a function returning Expected<U>.
#define MPE_TRY_EXPECTED(expr)                                                  \
    do {                                                                       \
        ::mpe::Result fe_try_result_ = (expr);                                   \
        if (!fe_try_result_.ok()) {                                             \
            return fe_try_result_.error();                                       \
        }                                                                       \
    } while (false)

// Two level indirection so __LINE__ expands before pasting.
#define MPE_CONCAT_INNER(a, b) a##b
#define MPE_CONCAT(a, b) MPE_CONCAT_INNER(a, b)

/// Bind the value of an Expected<T> or propagate its failure.
///
/// Usage:
///   MPE_ASSIGN_OR_RETURN(auto bytes, ReadFile(path));
#define MPE_ASSIGN_OR_RETURN(decl, expr)                                        \
    auto MPE_CONCAT(fe_expected_, __LINE__) = (expr);                            \
    if (!MPE_CONCAT(fe_expected_, __LINE__).ok()) {                              \
        return MPE_CONCAT(fe_expected_, __LINE__).error();                       \
    }                                                                           \
    decl = std::move(MPE_CONCAT(fe_expected_, __LINE__)).value()

} // namespace mpe

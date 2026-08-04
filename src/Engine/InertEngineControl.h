// SPDX-License-Identifier: MIT
// ForgeEvolved: Engine/InertEngineControl.h
//
// Fail closed IEngineControl for a game build whose engine binding has not been
// resolved.
//
// THIS IS A DELIBERATE IMPLEMENTATION, NOT A STUB
//
// Symbol discovery either succeeds and validates against the running binary, or
// it does not. When it does not, the correct behaviour is to keep the mod loaded
// and completely inert: the lobby UI can open, the log explains exactly which
// symbols failed, and every attempt to host or join is refused with that reason.
//
// The alternative, guessing at an unvalidated address, is how a mod corrupts a
// save or crashes on patch day. Reporting zero capabilities means HostSession and
// JoinSession refuse immediately, before a lobby is created or another player is
// involved.
//
// It is also the implementation the unit tests run against: the whole lobby state
// machine can be exercised with an engine that answers every call predictably.
#pragma once

#include <string>
#include <utility>

#include "Engine/IEngineControl.h"

namespace fe::engine {

class InertEngineControl final : public IEngineControl {
public:
    /// The reason capabilities are empty, surfaced to the player verbatim.
    explicit InertEngineControl(std::string reason) : reason_(std::move(reason)) {}

    [[nodiscard]] EngineCapabilities Capabilities() const override {
        return EngineCapabilities{}; // Everything false.
    }
    [[nodiscard]] EngineLifecycle Lifecycle() const override {
        return EngineLifecycle::Uninitialized;
    }

    [[nodiscard]] Result SetSessionClass(SessionClass) override { return Refuse(); }
    [[nodiscard]] Result SetSessionPrivacy(SessionPrivacy) override { return Refuse(); }
    [[nodiscard]] Result SetSimulationBandwidth(std::uint32_t) override { return Refuse(); }
    [[nodiscard]] Result SetHostMigrationEnabled(bool) override { return Refuse(); }

    [[nodiscard]] Result ApplyMatchSettings(const MatchSettings&) override { return Refuse(); }
    [[nodiscard]] Result BeginLoadScenario(std::string_view, std::uint32_t) override {
        return Refuse();
    }
    [[nodiscard]] Expected<float> QueryLoadProgress() const override { return RefuseValue<float>(); }
    [[nodiscard]] Result LaunchMatch() override { return Refuse(); }
    [[nodiscard]] Result EndMatch() override { return Refuse(); }
    [[nodiscard]] Result ReturnToFrontEnd() override { return Refuse(); }

    [[nodiscard]] Result LoadMapVariant(std::string_view) override { return Refuse(); }
    [[nodiscard]] Result ClearSandbox() override { return Refuse(); }
    [[nodiscard]] Expected<SandboxObjectHandle> SpawnSandboxObject(
        const SandboxPlacement&) override {
        return RefuseValue<SandboxObjectHandle>();
    }
    [[nodiscard]] Result DespawnSandboxObject(SandboxObjectHandle) override { return Refuse(); }
    [[nodiscard]] Expected<std::int32_t> ResolvePaletteIndex(std::string_view) const override {
        return RefuseValue<std::int32_t>();
    }
    [[nodiscard]] Result ExecuteConsoleCommand(std::string_view) override { return Refuse(); }

    [[nodiscard]] const std::string& Reason() const noexcept { return reason_; }

private:
    [[nodiscard]] Result Refuse() const {
        return Result::Fail(ErrorCode::IncompatibleGameBuild, reason_);
    }
    template <typename T>
    [[nodiscard]] Expected<T> RefuseValue() const {
        return Error{ErrorCode::IncompatibleGameBuild, reason_};
    }

    std::string reason_;
};

} // namespace fe::engine

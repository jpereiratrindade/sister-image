// SPDX-License-Identifier: MIT
// OBCE - Online Binary Classification Engine
//
// RegimeStateMachine.hpp - Temporal persistence for adaptive regime detection.

#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace obce {

enum class RegimeState {
    kCalibrating,
    kNormal,
    kSuspect,
    kPerturbation,
    kRecovering,
};

[[nodiscard]] std::string toString(RegimeState state);
[[nodiscard]] RegimeState regimeStateFromString(const std::string& value);

class RegimeStateMachine {
public:
    struct Config {
        std::size_t suspicion_persistence{3};
        std::size_t perturbation_persistence{5};
        std::size_t recovery_persistence{10};
    };

    RegimeStateMachine();
    explicit RegimeStateMachine(Config cfg);

    [[nodiscard]] RegimeState update(bool warmed_up, bool anomalous);

    [[nodiscard]] RegimeState state() const noexcept { return state_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] std::size_t highAnomalyStreak() const noexcept { return high_anomaly_streak_; }
    [[nodiscard]] std::size_t stableStreak() const noexcept { return stable_streak_; }

    void reset();

    [[nodiscard]] nlohmann::json toJson() const;
    static RegimeStateMachine fromJson(const nlohmann::json& j);

private:
    Config      cfg_;
    RegimeState state_{RegimeState::kCalibrating};
    std::size_t high_anomaly_streak_{0};
    std::size_t stable_streak_{0};

    void validateConfig() const;
};

} // namespace obce

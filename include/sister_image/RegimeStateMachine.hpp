// SPDX-License-Identifier: MIT
// SisTer Image — RegimeStateMachine.hpp

#pragma once

#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>

namespace sister_image {

enum class RegimeState {
    kCalibrating,
    kNormal,
    kSuspect,
    kPerturbation,
    kRecovering,
};

[[nodiscard]] std::string toString(RegimeState state);

class RegimeStateMachine {
public:
    struct Config {
        std::size_t suspect_patience{3};
        std::size_t recovery_patience{5};
    };

    RegimeStateMachine() = default;
    explicit RegimeStateMachine(Config cfg);

    RegimeState update(bool warmed_up, bool anomalous);

    [[nodiscard]] RegimeState state() const noexcept { return state_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    void reset() noexcept;

    [[nodiscard]] nlohmann::json toJson() const;
    static RegimeStateMachine fromJson(const nlohmann::json& j);

private:
    Config      cfg_;
    RegimeState state_{RegimeState::kCalibrating};
    std::size_t streak_{0};
};

} // namespace sister_image

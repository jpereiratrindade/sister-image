// SPDX-License-Identifier: MIT
// SisTer Image — RegimeStateMachine.cpp

#include "sister_image/RegimeStateMachine.hpp"
#include <stdexcept>

namespace sister_image {

std::string toString(RegimeState state) {
    switch (state) {
        case RegimeState::kCalibrating:  return "calibrating";
        case RegimeState::kNormal:       return "normal";
        case RegimeState::kSuspect:      return "suspect";
        case RegimeState::kPerturbation: return "perturbation";
        case RegimeState::kRecovering:   return "recovering";
    }
    return "unknown";
}

RegimeStateMachine::RegimeStateMachine(Config cfg) : cfg_(cfg) {}

RegimeState RegimeStateMachine::update(bool warmed_up, bool anomalous) {
    if (!warmed_up) {
        state_ = RegimeState::kCalibrating;
        streak_ = 0;
        return state_;
    }

    if (state_ == RegimeState::kCalibrating) {
        state_ = RegimeState::kNormal;
    }

    if (anomalous) {
        streak_++;
        if (state_ == RegimeState::kNormal && streak_ >= cfg_.suspect_patience) {
            state_ = RegimeState::kSuspect;
        } else if (state_ == RegimeState::kSuspect && streak_ >= cfg_.suspect_patience + 2) {
            state_ = RegimeState::kPerturbation;
        }
    } else {
        if (state_ != RegimeState::kNormal) {
            if (streak_ > 0) streak_--;
            else state_ = RegimeState::kNormal;
        }
    }
    return state_;
}

void RegimeStateMachine::reset() noexcept {
    state_ = RegimeState::kCalibrating;
    streak_ = 0;
}

nlohmann::json RegimeStateMachine::toJson() const {
    return {
        {"suspect_patience", cfg_.suspect_patience},
        {"recovery_patience", cfg_.recovery_patience},
        {"state", toString(state_)},
        {"streak", streak_}
    };
}

RegimeStateMachine RegimeStateMachine::fromJson(const nlohmann::json& j) {
    Config cfg;
    cfg.suspect_patience = j.value("suspect_patience", std::size_t{3});
    cfg.recovery_patience = j.value("recovery_patience", std::size_t{5});

    RegimeStateMachine machine(cfg);
    std::string s = j.value("state", "calibrating");
    if (s == "normal") machine.state_ = RegimeState::kNormal;
    else if (s == "suspect") machine.state_ = RegimeState::kSuspect;
    else if (s == "perturbation") machine.state_ = RegimeState::kPerturbation;
    else if (s == "recovering") machine.state_ = RegimeState::kRecovering;
    else machine.state_ = RegimeState::kCalibrating;

    machine.streak_ = j.value("streak", std::size_t{0});
    return machine;
}

} // namespace sister_image

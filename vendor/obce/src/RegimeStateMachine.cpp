// SPDX-License-Identifier: MIT
// OBCE - RegimeStateMachine.cpp

#include "obce/RegimeStateMachine.hpp"

#include <stdexcept>

namespace obce {

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

RegimeState regimeStateFromString(const std::string& value) {
    if (value == "calibrating")  return RegimeState::kCalibrating;
    if (value == "normal")       return RegimeState::kNormal;
    if (value == "suspect")      return RegimeState::kSuspect;
    if (value == "perturbation") return RegimeState::kPerturbation;
    if (value == "recovering")   return RegimeState::kRecovering;
    throw std::invalid_argument("RegimeState invalido: " + value);
}

RegimeStateMachine::RegimeStateMachine() : RegimeStateMachine(Config{}) {}

RegimeStateMachine::RegimeStateMachine(Config cfg) : cfg_(cfg) {
    validateConfig();
}

RegimeState RegimeStateMachine::update(bool warmed_up, bool anomalous) {
    if (!warmed_up) {
        state_ = RegimeState::kCalibrating;
        high_anomaly_streak_ = 0;
        stable_streak_ = 0;
        return state_;
    }

    if (state_ == RegimeState::kCalibrating) {
        state_ = RegimeState::kNormal;
    }

    switch (state_) {
        case RegimeState::kNormal:
            if (anomalous) {
                ++high_anomaly_streak_;
                stable_streak_ = 0;
                if (high_anomaly_streak_ >= cfg_.suspicion_persistence) {
                    state_ = RegimeState::kSuspect;
                }
            } else {
                high_anomaly_streak_ = 0;
                ++stable_streak_;
            }
            break;

        case RegimeState::kSuspect:
            if (anomalous) {
                ++high_anomaly_streak_;
                stable_streak_ = 0;
                if (high_anomaly_streak_ >= cfg_.perturbation_persistence) {
                    state_ = RegimeState::kPerturbation;
                }
            } else {
                ++stable_streak_;
                if (stable_streak_ >= cfg_.recovery_persistence) {
                    state_ = RegimeState::kNormal;
                    high_anomaly_streak_ = 0;
                    stable_streak_ = 0;
                }
            }
            break;

        case RegimeState::kPerturbation:
            if (anomalous) {
                stable_streak_ = 0;
            } else {
                ++stable_streak_;
                if (stable_streak_ >= cfg_.recovery_persistence) {
                    state_ = RegimeState::kRecovering;
                    high_anomaly_streak_ = 0;
                    stable_streak_ = 0;
                }
            }
            break;

        case RegimeState::kRecovering:
            if (anomalous) {
                state_ = RegimeState::kSuspect;
                high_anomaly_streak_ = 1;
                stable_streak_ = 0;
            } else {
                ++stable_streak_;
                if (stable_streak_ >= cfg_.recovery_persistence) {
                    state_ = RegimeState::kNormal;
                    high_anomaly_streak_ = 0;
                    stable_streak_ = 0;
                }
            }
            break;

        case RegimeState::kCalibrating:
            break;
    }

    return state_;
}

void RegimeStateMachine::reset() {
    state_ = RegimeState::kCalibrating;
    high_anomaly_streak_ = 0;
    stable_streak_ = 0;
}

nlohmann::json RegimeStateMachine::toJson() const {
    return {
        {"suspicion_persistence",    cfg_.suspicion_persistence},
        {"perturbation_persistence", cfg_.perturbation_persistence},
        {"recovery_persistence",     cfg_.recovery_persistence},
        {"state",                    toString(state_)},
        {"high_anomaly_streak",      high_anomaly_streak_},
        {"stable_streak",            stable_streak_},
    };
}

RegimeStateMachine RegimeStateMachine::fromJson(const nlohmann::json& j) {
    Config cfg;
    cfg.suspicion_persistence = j.value("suspicion_persistence", std::size_t{3});
    cfg.perturbation_persistence = j.value("perturbation_persistence", std::size_t{5});
    cfg.recovery_persistence = j.value("recovery_persistence", std::size_t{10});

    RegimeStateMachine machine(cfg);
    machine.state_ = regimeStateFromString(j.value("state", "calibrating"));
    machine.high_anomaly_streak_ = j.value("high_anomaly_streak", std::size_t{0});
    machine.stable_streak_ = j.value("stable_streak", std::size_t{0});
    return machine;
}

void RegimeStateMachine::validateConfig() const {
    if (cfg_.suspicion_persistence == 0 ||
        cfg_.perturbation_persistence == 0 ||
        cfg_.recovery_persistence == 0) {
        throw std::invalid_argument("RegimeStateMachine: persistencias devem ser > 0");
    }
    if (cfg_.perturbation_persistence < cfg_.suspicion_persistence) {
        throw std::invalid_argument(
            "RegimeStateMachine: perturbation_persistence deve ser >= suspicion_persistence");
    }
}

} // namespace obce

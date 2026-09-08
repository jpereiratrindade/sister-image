// SPDX-License-Identifier: MIT
// OBCE - Online Binary Classification Engine
//
// NormalityModel.hpp - Online baseline and anomaly scoring.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Label.hpp"
#include "RegimeStateMachine.hpp"

namespace obce {

struct NormalityReport {
    RegimeState               state{RegimeState::kCalibrating};
    bool                      warmed_up{false};
    std::size_t               n_seen{0};
    double                    anomaly_score{0.0};
    std::vector<double>       z_scores;
    std::vector<double>       feature_scores;
    std::vector<double>       baseline_mean;
    std::vector<double>       baseline_stddev;
    std::optional<int>        weak_label;

    [[nodiscard]] nlohmann::json toJson() const;
};

class NormalityModel {
public:
    struct Config {
        std::size_t              n_features{1};
        std::size_t              warmup_samples{100};
        double                   min_std{1e-6};
        double                   z_threshold{3.0};
        double                   anomaly_score_threshold{0.6};
        double                   max_z_score{10.0};
        double                   normal_learning_rate{0.01};
        double                   suspect_learning_rate{0.0};
        double                   perturbation_learning_rate{0.0};
        double                   recovery_learning_rate{0.002};
        std::vector<double>      feature_weights;
        RegimeStateMachine::Config state_machine;
    };

    explicit NormalityModel(Config cfg);

    [[nodiscard]] NormalityReport observe(std::span<const double> x);
    [[nodiscard]] NormalityReport report(std::span<const double> x) const;

    [[nodiscard]] bool warmedUp() const noexcept { return n_seen_ >= cfg_.warmup_samples; }
    [[nodiscard]] std::size_t nSeen() const noexcept { return n_seen_; }
    [[nodiscard]] RegimeState state() const noexcept { return state_machine_.state(); }
    [[nodiscard]] const std::vector<double>& mean() const noexcept { return mean_; }
    [[nodiscard]] std::vector<double> stdDev() const;

    void reset();

    [[nodiscard]] nlohmann::json toJson() const;
    static NormalityModel fromJson(const nlohmann::json& j);

private:
    Config             cfg_;
    RegimeStateMachine state_machine_;
    std::size_t        n_seen_{0};
    std::vector<double> n_per_feature_;
    std::vector<double> mean_;
    std::vector<double> m2_;
    std::vector<double> variance_;
    std::vector<double> weights_;

    void validateConfig() const;
    void validateShape(std::span<const double> x) const;
    void updateCalibration(std::span<const double> x);
    void updateEwma(std::span<const double> x, double lr);
    void initializeVarianceFromWelford();

    [[nodiscard]] std::vector<double> computeZScores(std::span<const double> x) const;
    [[nodiscard]] std::vector<double> computeFeatureScores(const std::vector<double>& z_scores) const;
    [[nodiscard]] double aggregateScore(const std::vector<double>& feature_scores) const;
    [[nodiscard]] double learningRateFor(RegimeState state) const noexcept;
    [[nodiscard]] std::optional<int> weakLabelFor(RegimeState state) const noexcept;
};

} // namespace obce

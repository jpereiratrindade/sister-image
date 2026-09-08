// SPDX-License-Identifier: MIT
// OBCE - NormalityModel.cpp

#include "obce/NormalityModel.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace obce {

nlohmann::json NormalityReport::toJson() const {
    nlohmann::json weak = nullptr;
    if (weak_label.has_value()) weak = weak_label.value();
    return {
        {"state",           toString(state)},
        {"warmed_up",       warmed_up},
        {"n_seen",          n_seen},
        {"anomaly_score",   anomaly_score},
        {"z_scores",        z_scores},
        {"feature_scores",  feature_scores},
        {"baseline_mean",   baseline_mean},
        {"baseline_stddev", baseline_stddev},
        {"weak_label",      weak},
    };
}

NormalityModel::NormalityModel(Config cfg)
    : cfg_(cfg),
      state_machine_(cfg.state_machine),
      n_per_feature_(cfg.n_features, 0.0),
      mean_(cfg.n_features, 0.0),
      m2_(cfg.n_features, 0.0),
      variance_(cfg.n_features, cfg.min_std * cfg.min_std),
      weights_(cfg.n_features, 1.0 / static_cast<double>(cfg.n_features)) {
    validateConfig();

    if (!cfg_.feature_weights.empty()) {
        weights_ = cfg_.feature_weights;
        double total = std::accumulate(weights_.begin(), weights_.end(), 0.0);
        if (total <= 0.0) {
            throw std::invalid_argument("NormalityModel: feature_weights deve somar > 0");
        }
        for (auto& weight : weights_) weight /= total;
    }
}

NormalityReport NormalityModel::observe(std::span<const double> x) {
    validateShape(x);
    ++n_seen_;

    if (!warmedUp()) {
        updateCalibration(x);
        (void)state_machine_.update(false, false);
        return report(x);
    }

    if (n_seen_ == cfg_.warmup_samples) {
        updateCalibration(x);
        initializeVarianceFromWelford();
        (void)state_machine_.update(true, false);
        return report(x);
    }

    auto z_scores = computeZScores(x);
    auto feature_scores = computeFeatureScores(z_scores);
    bool anomalous = aggregateScore(feature_scores) >= cfg_.anomaly_score_threshold;
    RegimeState new_state = state_machine_.update(true, anomalous);

    double lr = learningRateFor(new_state);
    if (!anomalous || new_state == RegimeState::kRecovering) {
        updateEwma(x, lr);
    }

    return report(x);
}

NormalityReport NormalityModel::report(std::span<const double> x) const {
    validateShape(x);
    auto z_scores = computeZScores(x);
    auto feature_scores = computeFeatureScores(z_scores);

    NormalityReport r;
    r.state = state_machine_.state();
    r.warmed_up = warmedUp();
    r.n_seen = n_seen_;
    r.anomaly_score = aggregateScore(feature_scores);
    r.z_scores = std::move(z_scores);
    r.feature_scores = std::move(feature_scores);
    r.baseline_mean = mean_;
    r.baseline_stddev = stdDev();
    r.weak_label = weakLabelFor(r.state);
    return r;
}

std::vector<double> NormalityModel::stdDev() const {
    std::vector<double> sd(cfg_.n_features, cfg_.min_std);
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        double var = variance_[i];
        if (n_per_feature_[i] >= 2.0 && n_seen_ <= cfg_.warmup_samples) {
            var = m2_[i] / (n_per_feature_[i] - 1.0);
        }
        sd[i] = std::sqrt(std::max(cfg_.min_std * cfg_.min_std, var));
    }
    return sd;
}

void NormalityModel::reset() {
    n_seen_ = 0;
    state_machine_.reset();
    std::fill(n_per_feature_.begin(), n_per_feature_.end(), 0.0);
    std::fill(mean_.begin(), mean_.end(), 0.0);
    std::fill(m2_.begin(), m2_.end(), 0.0);
    std::fill(variance_.begin(), variance_.end(), cfg_.min_std * cfg_.min_std);
}

nlohmann::json NormalityModel::toJson() const {
    return {
        {"n_features",                  cfg_.n_features},
        {"warmup_samples",              cfg_.warmup_samples},
        {"min_std",                     cfg_.min_std},
        {"z_threshold",                 cfg_.z_threshold},
        {"anomaly_score_threshold",     cfg_.anomaly_score_threshold},
        {"max_z_score",                 cfg_.max_z_score},
        {"normal_learning_rate",        cfg_.normal_learning_rate},
        {"suspect_learning_rate",       cfg_.suspect_learning_rate},
        {"perturbation_learning_rate",  cfg_.perturbation_learning_rate},
        {"recovery_learning_rate",      cfg_.recovery_learning_rate},
        {"feature_weights",             weights_},
        {"state_machine",               state_machine_.toJson()},
        {"n_seen",                      n_seen_},
        {"n_per_feature",               n_per_feature_},
        {"mean",                        mean_},
        {"m2",                          m2_},
        {"variance",                    variance_},
    };
}

NormalityModel NormalityModel::fromJson(const nlohmann::json& j) {
    Config cfg;
    cfg.n_features = j.at("n_features").get<std::size_t>();
    cfg.warmup_samples = j.value("warmup_samples", std::size_t{100});
    cfg.min_std = j.value("min_std", 1e-6);
    cfg.z_threshold = j.value("z_threshold", 3.0);
    cfg.anomaly_score_threshold = j.value("anomaly_score_threshold", 0.6);
    cfg.max_z_score = j.value("max_z_score", 10.0);
    cfg.normal_learning_rate = j.value("normal_learning_rate", 0.01);
    cfg.suspect_learning_rate = j.value("suspect_learning_rate", 0.0);
    cfg.perturbation_learning_rate = j.value("perturbation_learning_rate", 0.0);
    cfg.recovery_learning_rate = j.value("recovery_learning_rate", 0.002);
    cfg.feature_weights = j.value("feature_weights", std::vector<double>{});

    if (j.contains("state_machine")) {
        auto machine = RegimeStateMachine::fromJson(j.at("state_machine"));
        cfg.state_machine = machine.config();
    }

    NormalityModel model(cfg);
    if (j.contains("state_machine")) {
        model.state_machine_ = RegimeStateMachine::fromJson(j.at("state_machine"));
    }
    model.n_seen_ = j.at("n_seen").get<std::size_t>();
    model.n_per_feature_ = j.at("n_per_feature").get<std::vector<double>>();
    model.mean_ = j.at("mean").get<std::vector<double>>();
    model.m2_ = j.at("m2").get<std::vector<double>>();
    model.variance_ = j.at("variance").get<std::vector<double>>();
    return model;
}

void NormalityModel::validateConfig() const {
    if (cfg_.n_features == 0) {
        throw std::invalid_argument("NormalityModel: n_features deve ser > 0");
    }
    if (cfg_.warmup_samples == 0) {
        throw std::invalid_argument("NormalityModel: warmup_samples deve ser > 0");
    }
    if (cfg_.min_std <= 0.0 || cfg_.z_threshold <= 0.0 ||
        cfg_.anomaly_score_threshold <= 0.0 || cfg_.anomaly_score_threshold > 1.0 ||
        cfg_.max_z_score <= 0.0) {
        throw std::invalid_argument("NormalityModel: thresholds devem ser positivos");
    }
    if (cfg_.feature_weights.size() != 0 && cfg_.feature_weights.size() != cfg_.n_features) {
        throw std::invalid_argument("NormalityModel: feature_weights tem tamanho invalido");
    }
}

void NormalityModel::validateShape(std::span<const double> x) const {
    if (x.size() != cfg_.n_features) {
        throw std::invalid_argument(
            "NormalityModel: esperava " + std::to_string(cfg_.n_features) +
            " features, recebeu " + std::to_string(x.size()));
    }
}

void NormalityModel::updateCalibration(std::span<const double> x) {
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        double v = x[i];
        if (!std::isfinite(v)) continue;

        n_per_feature_[i] += 1.0;
        double delta = v - mean_[i];
        mean_[i] += delta / n_per_feature_[i];
        double delta2 = v - mean_[i];
        m2_[i] += delta * delta2;
    }
}

void NormalityModel::updateEwma(std::span<const double> x, double lr) {
    if (lr <= 0.0) return;

    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        double v = x[i];
        if (!std::isfinite(v)) continue;

        double delta = v - mean_[i];
        mean_[i] += lr * delta;
        variance_[i] = (1.0 - lr) * (variance_[i] + lr * delta * delta);
        variance_[i] = std::max(variance_[i], cfg_.min_std * cfg_.min_std);
        n_per_feature_[i] += 1.0;
    }
}

void NormalityModel::initializeVarianceFromWelford() {
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        if (n_per_feature_[i] >= 2.0) {
            variance_[i] = std::max(cfg_.min_std * cfg_.min_std,
                                    m2_[i] / (n_per_feature_[i] - 1.0));
        }
    }
}

std::vector<double> NormalityModel::computeZScores(std::span<const double> x) const {
    std::vector<double> z(cfg_.n_features, 0.0);
    auto sd = stdDev();
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        double v = x[i];
        if (!std::isfinite(v)) continue;
        z[i] = std::abs(v - mean_[i]) / std::max(sd[i], cfg_.min_std);
        z[i] = std::min(z[i], cfg_.max_z_score);
    }
    return z;
}

std::vector<double>
NormalityModel::computeFeatureScores(const std::vector<double>& z_scores) const {
    std::vector<double> scores(cfg_.n_features, 0.0);
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        scores[i] = std::clamp(z_scores[i] / cfg_.z_threshold, 0.0, 1.0);
    }
    return scores;
}

double NormalityModel::aggregateScore(const std::vector<double>& feature_scores) const {
    double score = 0.0;
    for (std::size_t i = 0; i < cfg_.n_features; ++i) {
        score += weights_[i] * feature_scores[i];
    }
    return std::clamp(score, 0.0, 1.0);
}

double NormalityModel::learningRateFor(RegimeState state) const noexcept {
    switch (state) {
        case RegimeState::kNormal:       return cfg_.normal_learning_rate;
        case RegimeState::kSuspect:      return cfg_.suspect_learning_rate;
        case RegimeState::kPerturbation: return cfg_.perturbation_learning_rate;
        case RegimeState::kRecovering:   return cfg_.recovery_learning_rate;
        case RegimeState::kCalibrating:  return 0.0;
    }
    return 0.0;
}

std::optional<int> NormalityModel::weakLabelFor(RegimeState state) const noexcept {
    if (state == RegimeState::kNormal) return kLabelNegative;
    if (state == RegimeState::kPerturbation) return kLabelPositive;
    return std::nullopt;
}

} // namespace obce

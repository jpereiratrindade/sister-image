// SPDX-License-Identifier: MIT
// OBCE — Online Binary Classification Engine
//
// Label.hpp — Constantes genéricas de rótulo binário.
//
// O motor opera em espaço {kNegative, kPositive}.
// Domínios concretos (ex: CSI) mapeiam seus rótulos via adaptador.
//
// Referência: Rosenblatt (1958), Novikoff (1962).

#pragma once

namespace obce {

/// Rótulo da classe positiva (perturbação, interrupção, anomalia).
constexpr int kLabelPositive = +1;

/// Rótulo da classe negativa (estado estável, basal, natural).
constexpr int kLabelNegative = -1;

/// Verifica se um rótulo inteiro é válido para o motor.
[[nodiscard]] constexpr bool isValidLabel(int label) noexcept {
    return label == kLabelPositive || label == kLabelNegative;
}

} // namespace obce

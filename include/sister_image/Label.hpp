// SPDX-License-Identifier: MIT
// SisTer Image — Label.hpp

#pragma once

namespace sister_image {

constexpr int kLabelPositive = +1;
constexpr int kLabelNegative = -1;

[[nodiscard]] constexpr bool isValidLabel(int label) noexcept {
    return label == kLabelPositive || label == kLabelNegative;
}

} // namespace sister_image

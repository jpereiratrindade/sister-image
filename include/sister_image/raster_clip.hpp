// SPDX-License-Identifier: MIT
// SisTer Image — raster_clip.hpp

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "sister_image/vector_shape.hpp"
#include <nlohmann/json.hpp>

namespace sister_image {

struct ClipConfig {
    std::filesystem::path input_tiff;
    std::filesystem::path output_tiff;
    VectorShape shape;
    bool mask_outside{true};
    uint8_t nodata_val{0};
    int zone{22};
    bool southern{true};
};

struct ClipResult {
    std::size_t input_width{0};
    std::size_t input_height{0};
    std::size_t output_width{0};
    std::size_t output_height{0};
    std::size_t crop_col_min{0};
    std::size_t crop_row_min{0};
    std::size_t crop_col_max{0};
    std::size_t crop_row_max{0};
    double tie_x{0.0};
    double tie_y{0.0};
    double scale_x{1.0};
    double scale_y{1.0};
    double elapsed_seconds{0.0};

    [[nodiscard]] nlohmann::json to_json() const;
};

ClipResult clip_raster(const ClipConfig& config);

} // namespace sister_image

// SPDX-License-Identifier: MIT
// SisTer Image — RasterClip.cpp

#include "sister_image/raster_clip.hpp"
#include "tiff_resource.hpp"
#include <tiffio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace sister_image {

namespace {
constexpr std::uint32_t kModelPixelScaleTag = 33550;
constexpr std::uint32_t kModelTiepointTag = 33922;
constexpr std::uint32_t kGeoKeyDirectoryTag = 34735;
}

nlohmann::json ClipResult::to_json() const {
    return {
        {"input_width", input_width},
        {"input_height", input_height},
        {"output_width", output_width},
        {"output_height", output_height},
        {"crop_col_min", crop_col_min},
        {"crop_row_min", crop_row_min},
        {"crop_col_max", crop_col_max},
        {"crop_row_max", crop_row_max},
        {"tie_x", tie_x},
        {"tie_y", tie_y},
        {"scale_x", scale_x},
        {"scale_y", scale_y},
        {"elapsed_seconds", elapsed_seconds}
    };
}

ClipResult clip_raster(const ClipConfig& config) {
    const auto t_start = std::chrono::steady_clock::now();

    std::unique_ptr<TIFF, decltype(&TIFFClose)> in_tif(
        sister_image::open_tiff(config.input_tiff.c_str(), "r"), TIFFClose);
    if (!in_tif) throw std::runtime_error("Nao foi possivel abrir imagem TIFF para recorte");

    std::uint32_t width{}, height{};
    std::uint16_t spp{}, bps{}, photo{}, planar{};
    TIFFGetField(in_tif.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(in_tif.get(), TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(in_tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(in_tif.get(), TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(in_tif.get(), TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetField(in_tif.get(), TIFFTAG_PLANARCONFIG, &planar);

    if (bps != 8 || planar != PLANARCONFIG_CONTIG) {
        throw std::invalid_argument("Apenas TIFF uint8 contiguo e suportado para recorte");
    }

    double scale[3] = {1.0, 1.0, 0.0};
    double tie[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    uint32_t scale_count = 0, tie_count = 0;
    double *scale_ptr = nullptr, *tie_ptr = nullptr;

    bool has_scale = (TIFFGetField(in_tif.get(), kModelPixelScaleTag, &scale_count, &scale_ptr) == 1 && scale_count >= 2);
    bool has_tie = (TIFFGetField(in_tif.get(), kModelTiepointTag, &tie_count, &tie_ptr) == 1 && tie_count >= 6);

    if (has_scale) { scale[0] = scale_ptr[0]; scale[1] = scale_ptr[1]; }
    if (has_tie) {
        tie[0] = tie_ptr[0]; tie[1] = tie_ptr[1]; tie[2] = tie_ptr[2];
        tie[3] = tie_ptr[3]; tie[4] = tie_ptr[4]; tie[5] = tie_ptr[5];
    }

    uint16_t key_count = 0;
    uint16_t* key_ptr = nullptr;
    bool has_keys = (TIFFGetField(in_tif.get(), kGeoKeyDirectoryTag, &key_count, &key_ptr) == 1);

    // Convert shape coordinates to pixel grid coordinates
    VectorShape px_shape = config.shape;
    if (has_scale && has_tie && (px_shape.min_x < 0 || px_shape.max_x > width || px_shape.min_y < 0 || px_shape.max_y > height)) {
        for (auto& poly : px_shape.polygons) {
            for (auto& pt : poly.outer_ring) {
                pt.x = (pt.x - tie[3]) / scale[0];
                pt.y = (tie[4] - pt.y) / scale[1];
            }
            for (auto& hole : poly.inner_rings) {
                for (auto& pt : hole) {
                    pt.x = (pt.x - tie[3]) / scale[0];
                    pt.y = (tie[4] - pt.y) / scale[1];
                }
            }
            poly.compute_bounds();
        }
        px_shape.compute_bounds();
    }

    std::size_t col_min = 0, row_min = 0, col_max = width - 1, row_max = height - 1;
    if (!px_shape.polygons.empty()) {
        col_min = std::clamp<std::size_t>(std::floor(px_shape.min_x), 0, width - 1);
        row_min = std::clamp<std::size_t>(std::floor(px_shape.min_y), 0, height - 1);
        col_max = std::clamp<std::size_t>(std::ceil(px_shape.max_x), 0, width - 1);
        row_max = std::clamp<std::size_t>(std::ceil(px_shape.max_y), 0, height - 1);
    }

    if (col_min > col_max || row_min > row_max) {
        throw std::invalid_argument("Poligono de recorte esta fora dos limites do raster");
    }

    std::size_t out_w = col_max - col_min + 1;
    std::size_t out_h = row_max - row_min + 1;

    std::unique_ptr<TIFF, decltype(&TIFFClose)> out_tif(
        sister_image::open_tiff(config.output_tiff.c_str(), "w8"), TIFFClose);
    if (!out_tif) throw std::runtime_error("Falha ao criar GeoTIFF de recorte");

    TIFFSetField(out_tif.get(), TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(out_w));
    TIFFSetField(out_tif.get(), TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(out_h));
    TIFFSetField(out_tif.get(), TIFFTAG_SAMPLESPERPIXEL, spp);
    TIFFSetField(out_tif.get(), TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(out_tif.get(), TIFFTAG_PHOTOMETRIC, photo);
    TIFFSetField(out_tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(out_tif.get(), TIFFTAG_ROWSPERSTRIP, std::min<uint32_t>(16, out_h));

    if (has_scale) TIFFSetField(out_tif.get(), kModelPixelScaleTag, 3, scale);
    if (has_tie) {
        double new_tie[6] = {
            tie[0], tie[1], tie[2],
            tie[3] + static_cast<double>(col_min) * scale[0],
            tie[4] - static_cast<double>(row_min) * scale[1],
            tie[5]
        };
        TIFFSetField(out_tif.get(), kModelTiepointTag, 6, new_tie);
    }
    if (has_keys && key_ptr) {
        TIFFSetField(out_tif.get(), kGeoKeyDirectoryTag, key_count, key_ptr);
    }

    std::vector<uint8_t> in_row(width * spp);
    std::vector<uint8_t> out_row(out_w * spp);

    for (std::size_t y = row_min; y <= row_max; ++y) {
        if (TIFFReadScanline(in_tif.get(), in_row.data(), static_cast<uint32_t>(y)) < 0) {
            throw std::runtime_error("Falha lendo scanline da imagem de entrada");
        }

        std::size_t out_y = y - row_min;
        for (std::size_t x = col_min; x <= col_max; ++x) {
            std::size_t out_x = x - col_min;
            bool keep = true;
            if (config.mask_outside && !px_shape.polygons.empty()) {
                keep = px_shape.contains(static_cast<double>(x), static_cast<double>(y));
            }
            for (std::size_t c = 0; c < spp; ++c) {
                if (keep) {
                    out_row[out_x * spp + c] = in_row[x * spp + c];
                } else {
                    out_row[out_x * spp + c] = config.nodata_val;
                }
            }
        }

        if (TIFFWriteScanline(out_tif.get(), out_row.data(), static_cast<uint32_t>(out_y)) < 0) {
            throw std::runtime_error("Falha gravando scanline recortada");
        }
    }

    const auto t_end = std::chrono::steady_clock::now();
    ClipResult res;
    res.input_width = width;
    res.input_height = height;
    res.output_width = out_w;
    res.output_height = out_h;
    res.crop_col_min = col_min;
    res.crop_row_min = row_min;
    res.crop_col_max = col_max;
    res.crop_row_max = row_max;
    res.tie_x = tie[3] + static_cast<double>(col_min) * scale[0];
    res.tie_y = tie[4] - static_cast<double>(row_min) * scale[1];
    res.scale_x = scale[0];
    res.scale_y = scale[1];
    res.elapsed_seconds = std::chrono::duration<double>(t_end - t_start).count();
    return res;
}

} // namespace sister_image

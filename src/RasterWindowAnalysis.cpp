#include "sister_image/RasterWindowAnalysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <tiffio.h>
#include "tiff_resource.hpp"

#include "sister_image/NormalityModel.hpp"

namespace sister_image {
namespace {

constexpr std::uint32_t kModelPixelScaleTag = 33550;
constexpr std::uint32_t kModelTiepointTag = 33922;
constexpr std::uint32_t kModelTransformationTag = 34264;
constexpr std::uint32_t kGeoKeyDirectoryTag = 34735;
constexpr std::uint32_t kGeoDoubleParamsTag = 34736;
constexpr std::uint32_t kGeoAsciiParamsTag = 34737;

struct WindowAccumulator {
    std::size_t total{0};
    std::size_t ignored{0};
    std::size_t count{0};
    double sum{0.0};
    double sum_sq{0.0};
    double dx_sum{0.0};
    std::size_t dx_count{0};

    void add(double value) {
        ++total;
        ++count;
        sum += value;
        sum_sq += value * value;
    }

    void ignore() {
        ++total;
        ++ignored;
    }

    void addDx(double value) {
        ++dx_count;
        dx_sum += value;
    }
};

void registerGeoTiffTags(TIFF* tif) {
    TIFFFieldInfo geo_fields[] = {
        {kModelPixelScaleTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelPixelScaleTag")},
        {kModelTiepointTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelTiepointTag")},
        {kModelTransformationTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelTransformationTag")},
        {kGeoKeyDirectoryTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoKeyDirectoryTag")},
        {kGeoDoubleParamsTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoDoubleParamsTag")},
        {kGeoAsciiParamsTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoAsciiParamsTag")},
    };
    TIFFMergeFieldInfo(tif, geo_fields,
                       static_cast<std::uint32_t>(sizeof(geo_fields) / sizeof(geo_fields[0])));
}

double grayFromPixel(const unsigned char* pixel, std::uint16_t samples_per_pixel) {
    if (samples_per_pixel >= 3) {
        return 0.299 * static_cast<double>(pixel[0]) +
               0.587 * static_cast<double>(pixel[1]) +
               0.114 * static_cast<double>(pixel[2]);
    }
    return static_cast<double>(pixel[0]);
}

bool isZeroPixel(const unsigned char* pixel, std::uint16_t samples_per_pixel) {
    std::uint16_t color_samples = samples_per_pixel <= 2 ? 1 : 3;
    for (std::uint16_t i = 0; i < color_samples; ++i) {
        if (pixel[i] != 0) return false;
    }
    return true;
}

bool isNoDataGray(double gray, const RasterWindowAnalysisConfig& config) {
    for (double value : config.nodata_values) {
        if (std::abs(gray - value) <= config.nodata_epsilon) return true;
    }
    return false;
}

bool shouldIgnorePixel(const unsigned char* pixel,
                       std::uint16_t samples_per_pixel,
                       double gray,
                       const RasterWindowAnalysisConfig& config) {
    if (config.ignore_transparent_pixels && ((samples_per_pixel == 4 && pixel[3] == 0) || (samples_per_pixel == 2 && pixel[1] == 0))) {
        return true;
    }
    if (config.ignore_zero_pixels && isZeroPixel(pixel, samples_per_pixel)) {
        return true;
    }
    return isNoDataGray(gray, config);
}

double windowStddev(const WindowAccumulator& window, double mean) {
    if (window.count < 2) return 0.0;
    double variance = (window.sum_sq / static_cast<double>(window.count)) - (mean * mean);
    return std::sqrt(std::max(0.0, variance));
}

void ignorePixelInWindow(std::vector<WindowAccumulator>& accum,
                         std::size_t windows_x,
                         std::size_t window_size,
                         std::size_t x,
                         std::size_t y) {
    std::size_t wx = x / window_size;
    std::size_t wy = y / window_size;
    accum[(wy * windows_x) + wx].ignore();
}

void addPixelToWindow(std::vector<WindowAccumulator>& accum,
                      std::size_t windows_x,
                      std::size_t window_size,
                      std::size_t x,
                      std::size_t y,
                      double gray,
                      double previous_gray,
                      std::size_t previous_wx,
                      bool has_previous) {
    std::size_t wx = x / window_size;
    std::size_t wy = y / window_size;
    auto& window = accum[(wy * windows_x) + wx];
    window.add(gray);
    if (has_previous && previous_wx == wx) window.addDx(std::abs(gray - previous_gray));
}

double validFraction(const WindowAccumulator& acc) {
    return acc.total > 0 ? static_cast<double>(acc.count) / static_cast<double>(acc.total) : 0.0;
}

std::vector<std::size_t> buildWindowScales(std::size_t min_window_size, std::size_t max_window_size) {
    std::vector<std::size_t> scales;
    std::size_t current = std::max<std::size_t>(1, min_window_size);
    while (current < max_window_size) {
        scales.push_back(current);
        if (current > (max_window_size / 2)) break;
        current *= 2;
    }
    if (scales.empty() || scales.back() != max_window_size) {
        scales.push_back(max_window_size);
    }
    return scales;
}

RasterRegionResult makeRegion(const std::vector<RasterWindowResult>& windows,
                              const std::vector<std::size_t>& component) {
    RasterRegionResult region;
    region.x_min = windows[component.front()].x;
    region.y_min = windows[component.front()].y;

    for (std::size_t window_index : component) {
        const auto& window = windows[window_index];
        region.window_size_px = window.window_size_px;
        ++region.window_count;
        region.x_min = std::min(region.x_min, window.x);
        region.y_min = std::min(region.y_min, window.y);
        region.x_max = std::max(region.x_max, window.x + window.width);
        region.y_max = std::max(region.y_max, window.y + window.height);
        region.mean_anomaly_score += window.anomaly_score;
        region.max_anomaly_score = std::max(region.max_anomaly_score, window.anomaly_score);
        region.mean_intensity += window.mean_intensity;
        region.stddev_intensity += window.stddev_intensity;
        region.mean_abs_dx += window.mean_abs_dx;
        region.mean_valid_fraction += window.valid_fraction;
    }

    double count = static_cast<double>(region.window_count);
    region.mean_anomaly_score /= count;
    region.mean_intensity /= count;
    region.stddev_intensity /= count;
    region.mean_abs_dx /= count;
    region.mean_valid_fraction /= count;
    return region;
}

std::vector<RasterRegionResult> buildRasterRegions(const RasterWindowAnalysisConfig& config,
                                                   const std::vector<RasterWindowResult>& windows) {
    std::map<std::size_t, std::map<std::pair<std::size_t, std::size_t>, std::size_t>> grids;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        if (window.anomaly_score < config.anomaly_threshold || window.window_size_px == 0) continue;
        std::size_t gx = window.x / window.window_size_px;
        std::size_t gy = window.y / window.window_size_px;
        grids[window.window_size_px][{gx, gy}] = i;
    }

    std::vector<RasterRegionResult> regions;
    for (const auto& [window_size, grid] : grids) {
        (void)window_size;
        std::map<std::pair<std::size_t, std::size_t>, bool> visited;
        for (const auto& entry : grid) {
            const auto& coord = entry.first;
            if (visited[coord]) continue;

            std::vector<std::size_t> component;
            std::queue<std::pair<std::size_t, std::size_t>> pending;
            pending.push(coord);
            visited[coord] = true;

            while (!pending.empty()) {
                auto current = pending.front();
                pending.pop();
                component.push_back(grid.at(current));

                std::vector<std::pair<std::size_t, std::size_t>> neighbors;
                neighbors.push_back({current.first + 1, current.second});
                neighbors.push_back({current.first, current.second + 1});
                if (current.first > 0) neighbors.push_back({current.first - 1, current.second});
                if (current.second > 0) neighbors.push_back({current.first, current.second - 1});
                for (const auto& neighbor : neighbors) {
                    if (grid.find(neighbor) == grid.end() || visited[neighbor]) continue;
                    visited[neighbor] = true;
                    pending.push(neighbor);
                }
            }

            if (component.size() >= config.min_region_windows) {
                regions.push_back(makeRegion(windows, component));
            }
        }
    }

    std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
        if (a.max_anomaly_score != b.max_anomaly_score) {
            return a.max_anomaly_score > b.max_anomaly_score;
        }
        return a.window_count > b.window_count;
    });
    return regions;
}

unsigned char scoreToGray(double score, bool binary_map, double threshold) {
    if (binary_map) return score >= threshold ? 255 : 0;
    double clamped = std::clamp(score, 0.0, 1.0);
    return static_cast<unsigned char>(std::lround(clamped * 255.0));
}

double squaredDistance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    double total = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        double diff = a[i] - b[i];
        total += diff * diff;
    }
    return total;
}

std::size_t chooseMapWindowSize(const RasterWindowAnalysisResult& result,
                                const RasterClassificationMapConfig& config) {
    if (config.window_size_px > 0) return config.window_size_px;
    if (result.window_size_px > 0) return result.window_size_px;
    std::size_t chosen = 0;
    for (const auto& window : result.windows) {
        chosen = std::max(chosen, window.window_size_px);
    }
    return chosen;
}

std::vector<const RasterWindowResult*> selectMapWindows(const RasterWindowAnalysisResult& result,
                                                       std::size_t window_size_px) {
    std::vector<const RasterWindowResult*> selected;
    for (const auto& window : result.windows) {
        if (window.window_size_px == window_size_px) selected.push_back(&window);
    }
    if (!selected.empty()) return selected;

    std::size_t nearest_size = 0;
    std::size_t nearest_distance = std::numeric_limits<std::size_t>::max();
    for (const auto& window : result.windows) {
        std::size_t distance = window.window_size_px > window_size_px
            ? window.window_size_px - window_size_px
            : window_size_px - window.window_size_px;
        if (distance < nearest_distance ||
            (distance == nearest_distance && window.window_size_px > nearest_size)) {
            nearest_size = window.window_size_px;
            nearest_distance = distance;
        }
    }

    for (const auto& window : result.windows) {
        if (window.window_size_px == nearest_size) selected.push_back(&window);
    }
    return selected;
}

std::vector<std::array<double, 3>> normalizedWindowFeatures(
        const std::vector<const RasterWindowResult*>& windows) {
    if (windows.empty()) return {};
    std::array<double, 3> mean{0.0, 0.0, 0.0};
    for (const auto* window : windows) {
        mean[0] += window->mean_intensity;
        mean[1] += window->stddev_intensity;
        mean[2] += window->mean_abs_dx;
    }
    for (double& value : mean) value /= static_cast<double>(windows.size());

    std::array<double, 3> stddev{0.0, 0.0, 0.0};
    for (const auto* window : windows) {
        stddev[0] += std::pow(window->mean_intensity - mean[0], 2.0);
        stddev[1] += std::pow(window->stddev_intensity - mean[1], 2.0);
        stddev[2] += std::pow(window->mean_abs_dx - mean[2], 2.0);
    }
    for (double& value : stddev) {
        value = std::sqrt(value / static_cast<double>(windows.size()));
        if (value <= 1e-9) value = 1.0;
    }

    std::vector<std::array<double, 3>> features;
    features.reserve(windows.size());
    for (const auto* window : windows) {
        features.push_back({
            (window->mean_intensity - mean[0]) / stddev[0],
            (window->stddev_intensity - mean[1]) / stddev[1],
            (window->mean_abs_dx - mean[2]) / stddev[2],
        });
    }
    return features;
}

std::vector<unsigned char> classifyWindowsByFeatures(
        const std::vector<const RasterWindowResult*>& windows,
        std::size_t requested_classes) {
    if (windows.empty()) return {};
    std::size_t class_count = std::clamp<std::size_t>(requested_classes, 2, 9);
    class_count = std::min(class_count, windows.size());

    auto features = normalizedWindowFeatures(windows);

    std::vector<std::size_t> order(windows.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return windows[a]->mean_intensity != windows[b]->mean_intensity ? windows[a]->mean_intensity < windows[b]->mean_intensity : a < b;
    });

    std::vector<std::array<double, 3>> centers;
    centers.reserve(class_count);
    for (std::size_t i = 0; i < class_count; ++i) {
        std::size_t quantile = (i * (order.size() - 1)) / std::max<std::size_t>(1, class_count - 1);
        centers.push_back(features[order[quantile]]);
    }

    std::vector<std::size_t> labels(windows.size(), 0);
    for (std::size_t iteration = 0; iteration < 24; ++iteration) {
        bool changed = false;
        for (std::size_t i = 0; i < features.size(); ++i) {
            std::size_t best_label = 0;
            double best_distance = squaredDistance(features[i], centers[0]);
            for (std::size_t label = 1; label < centers.size(); ++label) {
                double distance = squaredDistance(features[i], centers[label]);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_label = label;
                }
            }
            if (labels[i] != best_label) {
                labels[i] = best_label;
                changed = true;
            }
        }

        std::vector<std::array<double, 3>> next_centers(class_count, {0.0, 0.0, 0.0});
        std::vector<std::size_t> counts(class_count, 0);
        for (std::size_t i = 0; i < features.size(); ++i) {
            ++counts[labels[i]];
            for (std::size_t feature = 0; feature < 3; ++feature) {
                next_centers[labels[i]][feature] += features[i][feature];
            }
        }
        for (std::size_t label = 0; label < class_count; ++label) {
            if (counts[label] == 0) continue;
            for (double& value : next_centers[label]) {
                value /= static_cast<double>(counts[label]);
            }
            centers[label] = next_centers[label];
        }
        if (!changed) break;
    }

    std::vector<double> raw_mean_by_class(class_count, 0.0);
    std::vector<std::size_t> counts(class_count, 0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        raw_mean_by_class[labels[i]] += windows[i]->mean_intensity;
        ++counts[labels[i]];
    }
    for (std::size_t label = 0; label < class_count; ++label) {
        if (counts[label] > 0) raw_mean_by_class[label] /= static_cast<double>(counts[label]);
    }

    std::vector<std::size_t> class_order(class_count);
    for (std::size_t i = 0; i < class_order.size(); ++i) class_order[i] = i;
    std::sort(class_order.begin(), class_order.end(), [&](std::size_t a, std::size_t b) {
        return raw_mean_by_class[a] != raw_mean_by_class[b] ? raw_mean_by_class[a] < raw_mean_by_class[b] : a < b;
    });
    std::vector<unsigned char> class_gray(class_count, 0);
    for (std::size_t rank = 0; rank < class_order.size(); ++rank) {
        double fraction = class_order.size() > 1
            ? static_cast<double>(rank) / static_cast<double>(class_order.size() - 1)
            : 1.0;
        class_gray[class_order[rank]] =
            static_cast<unsigned char>(std::lround(32.0 + (fraction * 223.0)));
    }

    std::vector<unsigned char> grays(windows.size(), 0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        grays[i] = class_gray[labels[i]];
    }
    return grays;
}

std::vector<unsigned char> classifyWindowsByHomogeneousPatches(
        const std::vector<const RasterWindowResult*>& windows,
        double homogeneity_threshold) {
    if (windows.empty()) return {};
    auto features = normalizedWindowFeatures(windows);
    double threshold = homogeneity_threshold > 0.0 ? homogeneity_threshold : 1.25;
    double threshold_sq = threshold * threshold;

    // Dense row-major lookup and reusable BFS queue: no tree nodes or per-cell allocations.
    std::size_t nx = 0, ny = 0;
    for (const auto* w : windows) {
        nx = std::max(nx, w->x / w->window_size_px + 1);
        ny = std::max(ny, w->y / w->window_size_px + 1);
    }
    const auto absent = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> grid(nx * ny, absent);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto* w = windows[i];
        grid[(w->y / w->window_size_px) * nx + w->x / w->window_size_px] = i;
    }
    std::vector<unsigned char> visited(windows.size(), 0);
    std::vector<std::size_t> patch_label(windows.size(), 0), pending;
    pending.reserve(windows.size());
    std::vector<double> patch_mean_intensity;
    for (std::size_t start = 0; start < windows.size(); ++start) {
        if (visited[start]) continue;
        const auto patch_id = patch_mean_intensity.size();
        pending.clear(); pending.push_back(start); visited[start] = 1;
        double mean_intensity = 0.0;
        for (std::size_t head = 0; head < pending.size(); ++head) {
            const auto i = pending[head];
            const auto* w = windows[i];
            patch_label[i] = patch_id;
            mean_intensity += w->mean_intensity;
            const auto x = w->x / w->window_size_px, y = w->y / w->window_size_px;
            const std::array<std::size_t, 4> neighbors = {
                x + 1 < nx ? y * nx + x + 1 : absent,
                y + 1 < ny ? (y + 1) * nx + x : absent,
                x > 0 ? y * nx + x - 1 : absent,
                y > 0 ? (y - 1) * nx + x : absent};
            for (const auto cell : neighbors) {
                if (cell == absent || grid[cell] == absent) continue;
                const auto neighbor = grid[cell];
                if (visited[neighbor] || squaredDistance(features[i], features[neighbor]) > threshold_sq) continue;
                visited[neighbor] = 1;
                pending.push_back(neighbor);
            }
        }
        patch_mean_intensity.push_back(mean_intensity / static_cast<double>(pending.size()));
    }

    auto [min_it, max_it] = std::minmax_element(patch_mean_intensity.begin(),
                                                patch_mean_intensity.end());
    double min_mean = min_it != patch_mean_intensity.end() ? *min_it : 0.0;
    double max_mean = max_it != patch_mean_intensity.end() ? *max_it : min_mean;
    double span = max_mean - min_mean;

    std::vector<unsigned char> patch_gray(patch_mean_intensity.size(), 160);
    for (std::size_t patch = 0; patch < patch_mean_intensity.size(); ++patch) {
        double fraction = span > 1e-9 ? (patch_mean_intensity[patch] - min_mean) / span : 0.5;
        patch_gray[patch] =
            static_cast<unsigned char>(std::lround(32.0 + (std::clamp(fraction, 0.0, 1.0) * 223.0)));
    }

    std::vector<unsigned char> grays(windows.size(), 0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        grays[i] = patch_gray[patch_label[i]];
    }
    return grays;
}

std::size_t scaledCoordinate(std::size_t value, double scale, std::size_t limit, bool upper_bound) {
    double scaled = static_cast<double>(value) * scale;
    auto coordinate = upper_bound ? static_cast<std::size_t>(std::ceil(scaled))
                                  : static_cast<std::size_t>(std::floor(scaled));
    return std::min(coordinate, limit);
}

void copyGeoTiffTags(const RasterWindowAnalysisResult& result,
                     TIFF* output,
                     std::size_t output_width,
                     std::size_t output_height) {
    if (result.raster_path.empty()) return;

    TIFF* source = sister_image::open_tiff(result.raster_path.c_str(), "rm");
    if (source == nullptr) return;
    registerGeoTiffTags(source);
    registerGeoTiffTags(output);

    double scale_x = static_cast<double>(result.width_px) / static_cast<double>(output_width);
    double scale_y = static_cast<double>(result.height_px) / static_cast<double>(output_height);
    double inverse_scale_x = static_cast<double>(output_width) / static_cast<double>(result.width_px);
    double inverse_scale_y = static_cast<double>(output_height) / static_cast<double>(result.height_px);

    std::uint32_t count = 0;
    double* doubles = nullptr;
    std::uint16_t* shorts = nullptr;
    char* ascii = nullptr;

    if (TIFFGetField(source, kModelPixelScaleTag, &count, &doubles) == 1 && count > 0) {
        std::vector<double> values(doubles, doubles + count);
        if (values.size() >= 2) {
            values[0] *= scale_x;
            values[1] *= scale_y;
        }
        TIFFSetField(output, kModelPixelScaleTag, count, values.data());
    }

    count = 0;
    doubles = nullptr;
    if (TIFFGetField(source, kModelTiepointTag, &count, &doubles) == 1 && count > 0) {
        std::vector<double> values(doubles, doubles + count);
        for (std::size_t i = 0; i + 5 < values.size(); i += 6) {
            values[i] *= inverse_scale_x;
            values[i + 1] *= inverse_scale_y;
        }
        TIFFSetField(output, kModelTiepointTag, count, values.data());
    }

    count = 0;
    doubles = nullptr;
    if (TIFFGetField(source, kModelTransformationTag, &count, &doubles) == 1 && count > 0) {
        std::vector<double> values(doubles, doubles + count);
        if (values.size() >= 16) {
            values[0] *= scale_x;
            values[4] *= scale_x;
            values[8] *= scale_x;
            values[1] *= scale_y;
            values[5] *= scale_y;
            values[9] *= scale_y;
        }
        TIFFSetField(output, kModelTransformationTag, count, values.data());
    }

    count = 0;
    shorts = nullptr;
    if (TIFFGetField(source, kGeoKeyDirectoryTag, &count, &shorts) == 1 && count > 0) {
        TIFFSetField(output, kGeoKeyDirectoryTag, count, shorts);
    }

    count = 0;
    doubles = nullptr;
    if (TIFFGetField(source, kGeoDoubleParamsTag, &count, &doubles) == 1 && count > 0) {
        TIFFSetField(output, kGeoDoubleParamsTag, count, doubles);
    }

    count = 0;
    ascii = nullptr;
    if (TIFFGetField(source, kGeoAsciiParamsTag, &count, &ascii) == 1 && count > 0) {
        TIFFSetField(output, kGeoAsciiParamsTag, count, ascii);
    }

    TIFFClose(source);
}

} // namespace

RasterWindowAnalysisResult analyzeRasterWindowsSingleScale(const RasterWindowAnalysisConfig& config,
                                                           std::size_t active_window_size) {
    if (config.raster_path.empty()) throw std::invalid_argument("Raster vazio");
    if (active_window_size == 0) throw std::invalid_argument("Janela raster deve ser > 0");
    if (config.warmup_windows == 0) throw std::invalid_argument("Warmup raster deve ser > 0");
    if (config.min_valid_fraction < 0.0 || config.min_valid_fraction > 1.0) {
        throw std::invalid_argument("Fracao valida minima deve estar em [0, 1]");
    }

    std::unique_ptr<TIFF, decltype(&TIFFClose)> owner(sister_image::open_tiff(config.raster_path.c_str(), "rm"), TIFFClose);
    TIFF* tif = owner.get();
    if (tif == nullptr) {
        throw std::runtime_error("Nao foi possivel abrir raster TIFF: " + config.raster_path);
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t samples_per_pixel = 1;
    std::uint16_t bits_per_sample = 8;
    std::uint16_t planar_config = PLANARCONFIG_CONTIG;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar_config);

    if (width == 0 || height == 0) {
        owner.reset();
        throw std::runtime_error("Raster TIFF sem dimensoes validas");
    }
    if (bits_per_sample != 8) {
        owner.reset();
        throw std::runtime_error("Analise raster inicial suporta apenas TIFF 8-bit");
    }
    if (planar_config != PLANARCONFIG_CONTIG) {
        owner.reset();
        throw std::runtime_error("Analise raster inicial suporta apenas TIFF contiguo");
    }

    std::uint16_t photometric = 0, orientation = 0, sample_format = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    if (orientation != ORIENTATION_TOPLEFT || sample_format != SAMPLEFORMAT_UINT ||
        !((photometric == PHOTOMETRIC_RGB && (samples_per_pixel == 3 || samples_per_pixel == 4)) ||
          (photometric == PHOTOMETRIC_MINISBLACK && (samples_per_pixel == 1 || samples_per_pixel == 2)))) {
        throw std::invalid_argument("Use TIFF uint8 RGB/RGBA ou cinza, orientacao top-left");
    }
    if (samples_per_pixel == 2 || samples_per_pixel == 4) {
        std::uint16_t count = 0;
        std::uint16_t* extras = nullptr;
        TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &count, &extras);
        if (count != 1 || extras == nullptr || extras[0] != EXTRASAMPLE_UNASSALPHA) {
            throw std::invalid_argument("TIFF com alpha requer alpha nao associado declarado");
        }
    }
    if (width > 1000000 || height > 1000000 ||
        static_cast<std::uint64_t>(width) * height > 4000000000ULL) {
        throw std::invalid_argument("Imagem excede o limite de 4 bilhoes de pixels ou lado de 1 milhao");
    }
    const auto nx = (static_cast<std::size_t>(width) + active_window_size - 1) / active_window_size;
    const auto ny = (static_cast<std::size_t>(height) + active_window_size - 1) / active_window_size;
    if (nx * ny > 262144) throw std::invalid_argument("Mais de 262144 janelas; aumente o tamanho da janela");

    RasterWindowAnalysisResult result;
    result.raster_path = config.raster_path;
    result.width_px = width;
    result.height_px = height;
    result.channels = samples_per_pixel;
    result.bits_per_sample = bits_per_sample;
    result.window_size_px = active_window_size;
    result.windows_x = (static_cast<std::size_t>(width) + active_window_size - 1) /
                       active_window_size;
    result.windows_y = (static_cast<std::size_t>(height) + active_window_size - 1) /
                       active_window_size;

    std::vector<WindowAccumulator> accum(result.windows_x * result.windows_y);
    if (TIFFIsTiled(tif) != 0) {
        std::uint32_t tile_width = 0;
        std::uint32_t tile_height = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_height);
        tmsize_t tile_size = TIFFTileSize(tif);
        if (tile_width == 0 || tile_height == 0 || tile_size <= 0 || tile_size > 67108864 || static_cast<std::uint64_t>(tile_width) * tile_height * samples_per_pixel != static_cast<std::uint64_t>(tile_size)) {
            owner.reset();
            throw std::runtime_error("Raster TIFF com tile invalido");
        }
        std::vector<unsigned char> tile(static_cast<std::size_t>(tile_size));

        for (std::uint32_t tile_y = 0; tile_y < height; tile_y += tile_height) {
            std::vector<double> previous_gray(tile_height, 0.0);
            std::vector<std::size_t> previous_wx(tile_height, 0);
            std::vector<unsigned char> previous_valid(tile_height, 0);
            for (std::uint32_t tile_x = 0; tile_x < width; tile_x += tile_width) {
                if (TIFFReadTile(tif, tile.data(), tile_x, tile_y, 0, 0) < 0) {
                    owner.reset();
                    throw std::runtime_error("Falha lendo tile TIFF");
                }
                for (std::uint32_t local_y = 0; local_y < tile_height; ++local_y) {
                    std::uint32_t y = tile_y + local_y;
                    if (y >= height) break;
                    double prev_gray = previous_gray[local_y];
                    std::size_t prev_wx = previous_wx[local_y];
                    bool has_prev = previous_valid[local_y] != 0;
                    for (std::uint32_t local_x = 0; local_x < tile_width; ++local_x) {
                        std::uint32_t x = tile_x + local_x;
                        if (x >= width) break;
                        std::size_t pixel_index =
                            ((static_cast<std::size_t>(local_y) * tile_width) + local_x) *
                            samples_per_pixel;
                        const auto* pixel = tile.data() + pixel_index;
                        double gray = grayFromPixel(pixel, samples_per_pixel);
                        std::size_t wx = static_cast<std::size_t>(x) / active_window_size;
                        if (shouldIgnorePixel(pixel, samples_per_pixel, gray, config)) {
                            ignorePixelInWindow(accum, result.windows_x, active_window_size, x, y);
                            has_prev = false;
                            continue;
                        }
                        addPixelToWindow(accum, result.windows_x, active_window_size, x, y,
                                         gray, prev_gray, prev_wx, has_prev);
                        prev_gray = gray;
                        prev_wx = wx;
                        has_prev = true;
                    }
                    previous_gray[local_y] = prev_gray;
                    previous_wx[local_y] = prev_wx;
                    previous_valid[local_y] = has_prev;
                }
            }
        }
    } else {
        tmsize_t scanline_size = TIFFScanlineSize(tif);
        if (scanline_size <= 0 || scanline_size > 67108864 || static_cast<std::uint64_t>(scanline_size) < static_cast<std::uint64_t>(width) * samples_per_pixel) {
            owner.reset();
            throw std::runtime_error("Raster TIFF com scanline invalida");
        }
        std::vector<unsigned char> row(static_cast<std::size_t>(scanline_size));

        for (std::uint32_t y = 0; y < height; ++y) {
            if (TIFFReadScanline(tif, row.data(), y, 0) < 0) {
                owner.reset();
                throw std::runtime_error("Falha lendo scanline TIFF");
            }

            double prev_gray = 0.0;
            std::size_t prev_wx = 0;
            bool has_prev = false;

            for (std::uint32_t x = 0; x < width; ++x) {
                const auto* pixel = row.data() + (static_cast<std::size_t>(x) * samples_per_pixel);
                double gray = grayFromPixel(pixel, samples_per_pixel);
                std::size_t wx = static_cast<std::size_t>(x) / active_window_size;
                if (shouldIgnorePixel(pixel, samples_per_pixel, gray, config)) {
                    ignorePixelInWindow(accum, result.windows_x, active_window_size, x, y);
                    has_prev = false;
                    continue;
                }
                addPixelToWindow(accum, result.windows_x, active_window_size, x, y,
                                 gray, prev_gray, prev_wx, has_prev);
                prev_gray = gray;
                prev_wx = wx;
                has_prev = true;
            }
        }
    }

    owner.reset();

    std::vector<RasterWindowResult> candidate_windows;
    candidate_windows.reserve(accum.size());
    for (std::size_t wy = 0; wy < result.windows_y; ++wy) {
        for (std::size_t wx = 0; wx < result.windows_x; ++wx) {
            const auto& acc = accum[(wy * result.windows_x) + wx];
            if (acc.count == 0 || validFraction(acc) < config.min_valid_fraction) {
                ++result.skipped_windows;
                result.ignored_pixels += acc.ignored;
                result.valid_pixels += acc.count;
                continue;
            }

            double mean = acc.sum / static_cast<double>(acc.count);
            double stddev = windowStddev(acc, mean);
            double mean_abs_dx = acc.dx_count > 0
                ? acc.dx_sum / static_cast<double>(acc.dx_count)
                : 0.0;

            RasterWindowResult window;
            window.x = wx * active_window_size;
            window.y = wy * active_window_size;
            window.window_size_px = active_window_size;
            window.width = std::min(active_window_size,
                                    static_cast<std::size_t>(width) - window.x);
            window.height = std::min(active_window_size,
                                     static_cast<std::size_t>(height) - window.y);
            window.valid_pixels = acc.count;
            window.ignored_pixels = acc.ignored;
            window.valid_fraction = validFraction(acc);
            window.mean_intensity = mean;
            window.stddev_intensity = stddev;
            window.mean_abs_dx = mean_abs_dx;
            result.valid_pixels += acc.count;
            result.ignored_pixels += acc.ignored;
            candidate_windows.push_back(window);
        }
    }

    if (candidate_windows.empty()) return result;
    if (!config.compute_anomalies) {
        result.windows = std::move(candidate_windows);
        return result;
    }

    sister_image::NormalityModel::Config model_cfg;
    model_cfg.n_features = 3;
    model_cfg.warmup_samples = std::min(config.warmup_windows, candidate_windows.size());
    model_cfg.z_threshold = config.z_threshold;
    model_cfg.anomaly_score_threshold = config.anomaly_threshold;
    model_cfg.state_machine.suspect_patience = 1;
    model_cfg.state_machine.recovery_patience = 1;
    sister_image::NormalityModel model(model_cfg);

    result.windows.reserve(candidate_windows.size());
    for (auto window : candidate_windows) {
        std::array<double, 3> features = {
            window.mean_intensity,
            window.stddev_intensity,
            window.mean_abs_dx,
        };
        auto report = model.observe(features);
        window.anomaly_score = report.anomaly_score;
        if (window.anomaly_score >= config.anomaly_threshold &&
            report.n_seen > model_cfg.warmup_samples) {
            ++result.anomalous_windows;
        }
        result.max_anomaly_score = std::max(result.max_anomaly_score, window.anomaly_score);
        result.windows.push_back(window);
    }

    const auto& baseline = model.mean();
    if (baseline.size() == 3) {
        result.baseline_mean_intensity = baseline[0];
        result.baseline_stddev_intensity = baseline[1];
        result.baseline_mean_abs_dx = baseline[2];
    }
    return result;
}

RasterWindowAnalysisResult analyzeRasterWindows(const RasterWindowAnalysisConfig& config) {
    if (config.window_size_px == 0) throw std::invalid_argument("Janela raster deve ser > 0");
    if (config.min_window_size_px == 0) throw std::invalid_argument("Janela minima deve ser > 0");
    if (config.min_window_size_px > config.window_size_px) {
        throw std::invalid_argument("Janela minima nao pode ser maior que a janela maxima");
    }

    if (config.min_window_size_px == config.window_size_px) {
        return analyzeRasterWindowsSingleScale(config, config.window_size_px);
    }
    auto scales = buildWindowScales(config.min_window_size_px, config.window_size_px);
    RasterWindowAnalysisResult combined;
    bool initialized = false;

    for (std::size_t scale : scales) {
        auto result = analyzeRasterWindowsSingleScale(config, scale);
        RasterScaleSummary scale_summary;
        scale_summary.window_size_px = scale;
        scale_summary.windows_x = result.windows_x;
        scale_summary.windows_y = result.windows_y;
        scale_summary.valid_windows = result.windows.size();
        scale_summary.skipped_windows = result.skipped_windows;
        scale_summary.anomalous_windows = result.anomalous_windows;
        scale_summary.baseline_mean_intensity = result.baseline_mean_intensity;
        scale_summary.baseline_stddev_intensity = result.baseline_stddev_intensity;
        scale_summary.baseline_mean_abs_dx = result.baseline_mean_abs_dx;
        scale_summary.max_anomaly_score = result.max_anomaly_score;

        if (!initialized) {
            combined = result;
            combined.scales.clear();
            combined.windows.clear();
            combined.skipped_windows = 0;
            combined.anomalous_windows = 0;
            combined.valid_pixels = 0;
            combined.ignored_pixels = 0;
            combined.max_anomaly_score = 0.0;
            initialized = true;
        }

        combined.window_size_px = config.window_size_px;
        combined.windows_x = result.windows_x;
        combined.windows_y = result.windows_y;
        combined.skipped_windows += result.skipped_windows;
        combined.anomalous_windows += result.anomalous_windows;
        combined.valid_pixels += result.valid_pixels;
        combined.ignored_pixels += result.ignored_pixels;
        combined.max_anomaly_score = std::max(combined.max_anomaly_score, result.max_anomaly_score);
        combined.scales.push_back(scale_summary);
        combined.windows.insert(combined.windows.end(), result.windows.begin(), result.windows.end());
    }

    if (config.compute_anomalies) combined.regions = buildRasterRegions(config, combined.windows);
    return combined;
}

void writeRasterClassificationMap(const RasterWindowAnalysisResult& result,
                                  const RasterClassificationMapConfig& config) {
    if (config.output_path.empty()) throw std::invalid_argument("Caminho de saida vazio");
    if (result.width_px == 0 || result.height_px == 0) {
        throw std::invalid_argument("Resultado raster sem dimensoes");
    }
    if (result.windows.empty()) {
        throw std::invalid_argument("Resultado raster sem janelas classificadas");
    }

    std::size_t max_input_side = std::max(result.width_px, result.height_px);
    double scale = 1.0;
    if (config.max_side_px > 0 && max_input_side > config.max_side_px) {
        scale = static_cast<double>(config.max_side_px) / static_cast<double>(max_input_side);
    }

    std::size_t output_width = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(static_cast<double>(result.width_px) * scale)));
    std::size_t output_height = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(static_cast<double>(result.height_px) * scale)));
    if (output_width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        output_height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Mapa raster excede dimensoes TIFF suportadas");
    }

    std::size_t map_window_size = chooseMapWindowSize(result, config);
    auto windows = selectMapWindows(result, map_window_size);
    std::vector<unsigned char> grays;
    grays.reserve(windows.size());
    if (config.mode == RasterClassificationMapConfig::Mode::kFeatureClasses) {
        grays = classifyWindowsByFeatures(windows, config.class_count);
    } else if (config.mode == RasterClassificationMapConfig::Mode::kHomogeneousPatches) {
        grays = classifyWindowsByHomogeneousPatches(windows, config.homogeneity_threshold);
    } else {
        bool binary_map = config.mode == RasterClassificationMapConfig::Mode::kScoreBinary;
        for (const auto* window : windows) {
            grays.push_back(scoreToGray(window->anomaly_score,
                                        binary_map,
                                        config.anomaly_threshold));
        }
    }

    // O(output width + windows), instead of O(width * height) bytes.
    struct Span { std::size_t x0, x1, y0, y1; unsigned char gray; };
    std::vector<Span> spans;
    spans.reserve(windows.size());
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& w = *windows[i];
        spans.push_back({scaledCoordinate(w.x, scale, output_width, false),
                         scaledCoordinate(w.x + w.width, scale, output_width, true),
                         scaledCoordinate(w.y, scale, output_height, false),
                         scaledCoordinate(w.y + w.height, scale, output_height, true), grays[i]});
    }
    std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.y0 < b.y0; });
    std::vector<std::size_t> active;
    std::size_t next = 0;
    std::vector<unsigned char> row(output_width, 0);

    std::unique_ptr<TIFF, decltype(&TIFFClose)> owner(sister_image::open_tiff(config.output_path.c_str(), "w8"), TIFFClose);
    TIFF* tif = owner.get();
    if (tif == nullptr) {
        throw std::runtime_error("Nao foi possivel criar mapa TIFF: " + config.output_path);
    }
    registerGeoTiffTags(tif);

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(output_width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(output_height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    if (config.write_geotiff_tags) {
        copyGeoTiffTags(result, tif, output_width, output_height);
    }

    for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(output_height); ++y) {
        std::fill(row.begin(), row.end(), 0);
        std::erase_if(active, [&](auto i) { return spans[i].y1 <= y; });
        while (next < spans.size() && spans[next].y0 <= y) active.push_back(next++);
        for (const auto i : active) {
            const auto& span = spans[i];
            for (auto x = span.x0; x < span.x1; ++x) row[x] = std::max(row[x], span.gray);
        }
        if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
            owner.reset();
            throw std::runtime_error("Falha escrevendo mapa TIFF");
        }
    }
    owner.reset();
}

} // namespace sister_image

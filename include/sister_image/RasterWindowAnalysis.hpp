#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sister_image {

struct RasterWindowAnalysisConfig {
    bool compute_anomalies{true};
    std::string raster_path;
    std::size_t min_window_size_px{512};
    std::size_t window_size_px{512};
    std::size_t warmup_windows{24};
    double z_threshold{3.0};
    double anomaly_threshold{0.30};
    double min_valid_fraction{0.25};
    bool ignore_transparent_pixels{true};
    bool ignore_zero_pixels{true};
    std::size_t min_region_windows{3};
    std::vector<double> nodata_values{9999.0};
    double nodata_epsilon{1e-6};
};

struct RasterWindowResult {
    std::size_t x{0};
    std::size_t y{0};
    std::size_t window_size_px{0};
    std::size_t width{0};
    std::size_t height{0};
    std::size_t valid_pixels{0};
    std::size_t ignored_pixels{0};
    double valid_fraction{0.0};
    double mean_intensity{0.0};
    double stddev_intensity{0.0};
    double mean_abs_dx{0.0};
    double anomaly_score{0.0};
};

struct RasterRegionResult {
    std::size_t window_size_px{0};
    std::size_t window_count{0};
    std::size_t x_min{0};
    std::size_t y_min{0};
    std::size_t x_max{0};
    std::size_t y_max{0};
    double mean_anomaly_score{0.0};
    double max_anomaly_score{0.0};
    double mean_intensity{0.0};
    double stddev_intensity{0.0};
    double mean_abs_dx{0.0};
    double mean_valid_fraction{0.0};
};

struct RasterScaleSummary {
    std::size_t window_size_px{0};
    std::size_t windows_x{0};
    std::size_t windows_y{0};
    std::size_t valid_windows{0};
    std::size_t skipped_windows{0};
    std::size_t anomalous_windows{0};
    double baseline_mean_intensity{0.0};
    double baseline_stddev_intensity{0.0};
    double baseline_mean_abs_dx{0.0};
    double max_anomaly_score{0.0};
};

struct RasterWindowAnalysisResult {
    std::string raster_path;
    std::size_t width_px{0};
    std::size_t height_px{0};
    std::size_t channels{0};
    std::size_t bits_per_sample{0};
    std::size_t window_size_px{0};
    std::size_t windows_x{0};
    std::size_t windows_y{0};
    std::size_t skipped_windows{0};
    std::size_t anomalous_windows{0};
    std::size_t valid_pixels{0};
    std::size_t ignored_pixels{0};
    double baseline_mean_intensity{0.0};
    double baseline_stddev_intensity{0.0};
    double baseline_mean_abs_dx{0.0};
    double max_anomaly_score{0.0};
    std::vector<RasterScaleSummary> scales;
    std::vector<RasterWindowResult> windows;
    std::vector<RasterRegionResult> regions;
};

struct RasterClassificationMapConfig {
    enum class Mode {
        kScoreContinuous,
        kScoreBinary,
        kFeatureClasses,
        kHomogeneousPatches,
    };

    std::string output_path;
    std::size_t max_side_px{0};
    std::size_t window_size_px{0};
    std::size_t class_count{5};
    Mode mode{Mode::kFeatureClasses};
    bool write_geotiff_tags{true};
    double anomaly_threshold{0.30};
    double homogeneity_threshold{1.25};
};

RasterWindowAnalysisResult analyzeRasterWindows(const RasterWindowAnalysisConfig& config);
void writeRasterClassificationMap(const RasterWindowAnalysisResult& result,
                                  const RasterClassificationMapConfig& config);

} // namespace sister_image

namespace appcore = sister_image;

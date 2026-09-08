// SPDX-License-Identifier: MIT
// SisTer Image — vector_shape.hpp

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace sister_image {

struct Point2D {
    double x{0.0};
    double y{0.0};
};

struct Polygon2D {
    std::vector<Point2D> outer_ring;
    std::vector<std::vector<Point2D>> inner_rings;
    double min_x{0.0}, min_y{0.0}, max_x{0.0}, max_y{0.0};

    void compute_bounds();
    [[nodiscard]] bool contains(double x, double y) const;
};

class VectorShape {
public:
    std::string name;
    std::vector<Polygon2D> polygons;
    double min_x{0.0}, min_y{0.0}, max_x{0.0}, max_y{0.0};

    void compute_bounds();
    [[nodiscard]] bool contains(double x, double y) const;
    [[nodiscard]] nlohmann::json to_geojson() const;

    static VectorShape parse_file(const std::filesystem::path& path);
    static VectorShape parse_shp(const std::filesystem::path& path);
    static VectorShape parse_kml(const std::filesystem::path& path);
    static VectorShape parse_kmz_or_zip(const std::filesystem::path& path);
    static VectorShape parse_geojson(const nlohmann::json& j);
};

} // namespace sister_image

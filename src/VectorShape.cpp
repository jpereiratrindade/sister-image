// SPDX-License-Identifier: MIT
// SisTer Image — VectorShape.cpp

#include "sister_image/vector_shape.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace sister_image {

namespace {

static uint32_t swap32(uint32_t val) {
    return ((val >> 24) & 0xff) |
           ((val >> 8) & 0xff00) |
           ((val << 8) & 0xff0000) |
           ((val << 24) & 0xff000000);
}

static double read_double_le(const char* buf) {
    double d;
    std::memcpy(&d, buf, sizeof(double));
    return d;
}

static int32_t read_int32_le(const char* buf) {
    int32_t i;
    std::memcpy(&i, buf, sizeof(int32_t));
    return i;
}

static int32_t read_int32_be(const char* buf) {
    uint32_t u;
    std::memcpy(&u, buf, sizeof(uint32_t));
    u = swap32(u);
    return static_cast<int32_t>(u);
}

} // namespace

void Polygon2D::compute_bounds() {
    if (outer_ring.empty()) return;
    min_x = max_x = outer_ring[0].x;
    min_y = max_y = outer_ring[0].y;
    for (const auto& p : outer_ring) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
}

bool Polygon2D::contains(double x, double y) const {
    if (x < min_x || x > max_x || y < min_y || y > max_y) return false;
    bool inside = false;
    std::size_t n = outer_ring.size();
    if (n < 3) return false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = outer_ring[i].x, yi = outer_ring[i].y;
        double xj = outer_ring[j].x, yj = outer_ring[j].y;
        bool intersect = ((yi > y) != (yj > y)) &&
                         (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi);
        if (intersect) inside = !inside;
    }
    if (!inside) return false;
    for (const auto& hole : inner_rings) {
        std::size_t hn = hole.size();
        if (hn < 3) continue;
        bool hole_inside = false;
        for (std::size_t i = 0, j = hn - 1; i < hn; j = i++) {
            double xi = hole[i].x, yi = hole[i].y;
            double xj = hole[j].x, yj = hole[j].y;
            bool intersect = ((yi > y) != (yj > y)) &&
                             (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi);
            if (intersect) hole_inside = !hole_inside;
        }
        if (hole_inside) return false;
    }
    return true;
}

void VectorShape::compute_bounds() {
    if (polygons.empty()) return;
    min_x = max_x = polygons[0].min_x;
    min_y = max_y = polygons[0].min_y;
    for (auto& poly : polygons) {
        poly.compute_bounds();
        min_x = std::min(min_x, poly.min_x);
        max_x = std::max(max_x, poly.max_x);
        min_y = std::min(min_y, poly.min_y);
        max_y = std::max(max_y, poly.max_y);
    }
}

bool VectorShape::contains(double x, double y) const {
    if (x < min_x || x > max_x || y < min_y || y > max_y) return false;
    for (const auto& poly : polygons) {
        if (poly.contains(x, y)) return true;
    }
    return false;
}

nlohmann::json VectorShape::to_geojson() const {
    nlohmann::json features = nlohmann::json::array();
    for (const auto& poly : polygons) {
        nlohmann::json rings = nlohmann::json::array();
        nlohmann::json outer = nlohmann::json::array();
        for (const auto& p : poly.outer_ring) {
            outer.push_back({p.x, p.y});
        }
        rings.push_back(outer);
        for (const auto& hole : poly.inner_rings) {
            nlohmann::json h = nlohmann::json::array();
            for (const auto& p : hole) {
                h.push_back({p.x, p.y});
            }
            rings.push_back(h);
        }
        features.push_back({
            {"type", "Feature"},
            {"geometry", {
                {"type", "Polygon"},
                {"coordinates", rings}
            }},
            {"properties", nlohmann::json::object()}
        });
    }
    return {
        {"type", "FeatureCollection"},
        {"bbox", {min_x, min_y, max_x, max_y}},
        {"features", features}
    };
}

VectorShape VectorShape::parse_geojson(const nlohmann::json& j) {
    VectorShape shape;
    auto parse_poly = [](const nlohmann::json& coords) {
        Polygon2D poly;
        if (coords.is_array() && !coords.empty()) {
            for (std::size_t r = 0; r < coords.size(); ++r) {
                std::vector<Point2D> ring;
                for (const auto& pt : coords[r]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        ring.push_back({pt[0].get<double>(), pt[1].get<double>()});
                    }
                }
                if (r == 0) poly.outer_ring = std::move(ring);
                else poly.inner_rings.push_back(std::move(ring));
            }
        }
        poly.compute_bounds();
        return poly;
    };

    if (j.contains("type")) {
        std::string type = j["type"];
        if (type == "FeatureCollection" && j.contains("features")) {
            for (const auto& feat : j["features"]) {
                if (feat.contains("geometry")) {
                    std::string gtype = feat["geometry"].value("type", "");
                    if (gtype == "Polygon") {
                        shape.polygons.push_back(parse_poly(feat["geometry"]["coordinates"]));
                    }
                }
            }
        } else if (type == "Feature" && j.contains("geometry")) {
            if (j["geometry"].value("type", "") == "Polygon") {
                shape.polygons.push_back(parse_poly(j["geometry"]["coordinates"]));
            }
        } else if (type == "Polygon" && j.contains("coordinates")) {
            shape.polygons.push_back(parse_poly(j["coordinates"]));
        }
    }
    shape.compute_bounds();
    return shape;
}

VectorShape VectorShape::parse_shp(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Falha ao abrir arquivo SHP: " + path.string());

    char header[100];
    if (!file.read(header, 100)) throw std::runtime_error("Arquivo SHP invalido ou truncado: " + path.string());

    int32_t file_code = read_int32_be(header);
    if (file_code != 9994) throw std::runtime_error("Codigo de arquivo SHP invalido");

    VectorShape shape;
    shape.name = path.stem().string();

    while (file) {
        char rec_hdr[8];
        if (!file.read(rec_hdr, 8)) break;
        int32_t content_length = read_int32_be(rec_hdr + 4) * 2;
        if (content_length <= 0) break;

        std::vector<char> record(content_length);
        if (!file.read(record.data(), content_length)) break;

        int32_t shape_type = read_int32_le(record.data());
        if (shape_type == 5 || shape_type == 3) { // Polygon (5) or PolyLine (3)
            if (content_length < 44) continue;
            int32_t num_parts = read_int32_le(record.data() + 36);
            int32_t num_points = read_int32_le(record.data() + 40);
            if (num_parts <= 0 || num_points <= 0) continue;

            std::size_t parts_offset = 44;
            std::size_t points_offset = parts_offset + num_parts * 4;
            if (points_offset + num_points * 16 > static_cast<std::size_t>(content_length)) continue;

            std::vector<int32_t> parts(num_parts);
            for (int i = 0; i < num_parts; ++i) {
                parts[i] = read_int32_le(record.data() + parts_offset + i * 4);
            }

            for (int i = 0; i < num_parts; ++i) {
                int32_t start_idx = parts[i];
                int32_t end_idx = (i + 1 < num_parts) ? parts[i + 1] : num_points;
                Polygon2D poly;
                for (int p = start_idx; p < end_idx; ++p) {
                    double px = read_double_le(record.data() + points_offset + p * 16);
                    double py = read_double_le(record.data() + points_offset + p * 16 + 8);
                    poly.outer_ring.push_back({px, py});
                }
                poly.compute_bounds();
                shape.polygons.push_back(std::move(poly));
            }
        }
    }
    shape.compute_bounds();
    return shape;
}

VectorShape VectorShape::parse_kml(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Falha ao abrir KML: " + path.string());

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    VectorShape shape;
    shape.name = path.stem().string();

    std::string tag_open = "<coordinates>";
    std::string tag_close = "</coordinates>";

    std::size_t pos = 0;
    while ((pos = content.find(tag_open, pos)) != std::string::npos) {
        pos += tag_open.length();
        std::size_t end_pos = content.find(tag_close, pos);
        if (end_pos == std::string::npos) break;

        std::string coords_str = content.substr(pos, end_pos - pos);
        std::stringstream ss(coords_str);
        std::string tuple;
        Polygon2D poly;

        while (ss >> tuple) {
            std::size_t c1 = tuple.find(',');
            if (c1 != std::string::npos) {
                double lon = std::stod(tuple.substr(0, c1));
                std::size_t c2 = tuple.find(',', c1 + 1);
                double lat = (c2 != std::string::npos) ?
                    std::stod(tuple.substr(c1 + 1, c2 - c1 - 1)) :
                    std::stod(tuple.substr(c1 + 1));
                poly.outer_ring.push_back({lon, lat});
            }
        }
        if (!poly.outer_ring.empty()) {
            poly.compute_bounds();
            shape.polygons.push_back(std::move(poly));
        }
        pos = end_pos + tag_close.length();
    }
    shape.compute_bounds();
    return shape;
}

VectorShape VectorShape::parse_kmz_or_zip(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Falha ao abrir arquivo zip/kmz: " + path.string());

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::string tag_open = "<coordinates>";
    std::string tag_close = "</coordinates>";

    std::size_t pos = content.find(tag_open);
    if (pos != std::string::npos) {
        // Embedded KML XML string in zip stream
        VectorShape shape;
        shape.name = path.stem().string();
        while (pos != std::string::npos) {
            pos += tag_open.length();
            std::size_t end_pos = content.find(tag_close, pos);
            if (end_pos == std::string::npos) break;

            std::string coords_str = content.substr(pos, end_pos - pos);
            std::stringstream ss(coords_str);
            std::string tuple;
            Polygon2D poly;

            while (ss >> tuple) {
                std::size_t c1 = tuple.find(',');
                if (c1 != std::string::npos) {
                    double lon = std::stod(tuple.substr(0, c1));
                    std::size_t c2 = tuple.find(',', c1 + 1);
                    double lat = (c2 != std::string::npos) ?
                        std::stod(tuple.substr(c1 + 1, c2 - c1 - 1)) :
                        std::stod(tuple.substr(c1 + 1));
                    poly.outer_ring.push_back({lon, lat});
                }
            }
            if (!poly.outer_ring.empty()) {
                poly.compute_bounds();
                shape.polygons.push_back(std::move(poly));
            }
            pos = content.find(tag_open, end_pos + tag_close.length());
        }
        shape.compute_bounds();
        if (!shape.polygons.empty()) return shape;
    }

    throw std::runtime_error("Nenhum vetor valido encontrado no arquivo KMZ/ZIP: " + path.string());
}

VectorShape VectorShape::parse_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".shp") return parse_shp(path);
    if (ext == ".kml") return parse_kml(path);
    if (ext == ".kmz" || ext == ".zip") return parse_kmz_or_zip(path);
    if (ext == ".geojson" || ext == ".json") {
        std::ifstream f(path);
        return parse_geojson(nlohmann::json::parse(f));
    }
    throw std::runtime_error("Formato vetorial nao suportado: " + ext);
}

} // namespace sister_image

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

double Polygon2D::area() const {
    if (outer_ring.size() < 3) return 0.0;
    double a = 0.0;
    std::size_t n = outer_ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        a += (outer_ring[j].x * outer_ring[i].y) - (outer_ring[i].x * outer_ring[j].y);
    }
    a = std::abs(a) * 0.5;

    for (const auto& hole : inner_rings) {
        if (hole.size() < 3) continue;
        double ha = 0.0;
        std::size_t hn = hole.size();
        for (std::size_t i = 0, j = hn - 1; i < hn; j = i++) {
            ha += (hole[j].x * hole[i].y) - (hole[i].x * hole[j].y);
        }
        a -= std::abs(ha) * 0.5;
    }
    return std::max(0.0, a);
}

double Polygon2D::perimeter() const {
    if (outer_ring.empty()) return 0.0;
    double p = 0.0;
    std::size_t n = outer_ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        double dx = outer_ring[i].x - outer_ring[j].x;
        double dy = outer_ring[i].y - outer_ring[j].y;
        p += std::sqrt(dx * dx + dy * dy);
    }
    return p;
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
        if (poly.outer_ring.empty()) continue;
        std::size_t sz = poly.outer_ring.size();
        bool is_closed = (sz >= 4 &&
                          poly.outer_ring.front().x == poly.outer_ring.back().x &&
                          poly.outer_ring.front().y == poly.outer_ring.back().y);

        if (sz == 1) {
            features.push_back({
                {"type", "Feature"},
                {"geometry", {
                    {"type", "Point"},
                    {"coordinates", {poly.outer_ring[0].x, poly.outer_ring[0].y}}
                }},
                {"properties", nlohmann::json::object()}
            });
        } else if (!is_closed && poly.inner_rings.empty()) {
            nlohmann::json line = nlohmann::json::array();
            for (const auto& p : poly.outer_ring) {
                line.push_back({p.x, p.y});
            }
            features.push_back({
                {"type", "Feature"},
                {"geometry", {
                    {"type", "LineString"},
                    {"coordinates", line}
                }},
                {"properties", nlohmann::json::object()}
            });
        } else {
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
    }
    return {
        {"type", "FeatureCollection"},
        {"bbox", {min_x, min_y, max_x, max_y}},
        {"features", features}
    };
}

nlohmann::json VectorShape::to_metrics_json() const {
    std::size_t total_points = 0;
    std::size_t poly_count = 0;
    std::size_t pt_count = 0;
    std::size_t line_count = 0;
    double total_area = 0.0;
    double total_perimeter = 0.0;

    for (const auto& poly : polygons) {
        std::size_t sz = poly.outer_ring.size();
        total_points += sz;
        for (const auto& hole : poly.inner_rings) total_points += hole.size();

        bool is_closed = (sz >= 4 &&
                          poly.outer_ring.front().x == poly.outer_ring.back().x &&
                          poly.outer_ring.front().y == poly.outer_ring.back().y);

        if (sz == 1) pt_count++;
        else if (!is_closed && poly.inner_rings.empty()) line_count++;
        else poly_count++;

        total_area += poly.area();
        total_perimeter += poly.perimeter();
    }

    return {
        {"name", name},
        {"polygons_count", poly_count},
        {"points_count", pt_count},
        {"lines_count", line_count},
        {"total_features", polygons.size()},
        {"total_points", total_points},
        {"bbox", {min_x, min_y, max_x, max_y}},
        {"centroid", {(min_x + max_x) / 2.0, (min_y + max_y) / 2.0}},
        {"approx_area", total_area},
        {"approx_perimeter", total_perimeter}
    };
}

VectorShape VectorShape::parse_geojson(const nlohmann::json& j) {
    VectorShape shape;

    auto add_poly = [&shape](Polygon2D&& poly) {
        poly.compute_bounds();
        shape.polygons.push_back(std::move(poly));
    };

    auto parse_feature_geom = [&](const nlohmann::json& geom) {
        if (!geom.is_object() || !geom.contains("type") || !geom.contains("coordinates")) return;
        std::string gtype = geom["type"];
        const auto& coords = geom["coordinates"];

        if (gtype == "Point") {
            if (coords.is_array() && coords.size() >= 2) {
                Polygon2D p;
                p.outer_ring.push_back({coords[0].get<double>(), coords[1].get<double>()});
                add_poly(std::move(p));
            }
        } else if (gtype == "MultiPoint" || gtype == "LineString") {
            if (coords.is_array()) {
                Polygon2D p;
                for (const auto& pt : coords) {
                    if (pt.is_array() && pt.size() >= 2) {
                        p.outer_ring.push_back({pt[0].get<double>(), pt[1].get<double>()});
                    }
                }
                if (!p.outer_ring.empty()) add_poly(std::move(p));
            }
        } else if (gtype == "MultiLineString") {
            if (coords.is_array()) {
                for (const auto& line : coords) {
                    if (line.is_array()) {
                        Polygon2D p;
                        for (const auto& pt : line) {
                            if (pt.is_array() && pt.size() >= 2) {
                                p.outer_ring.push_back({pt[0].get<double>(), pt[1].get<double>()});
                            }
                        }
                        if (!p.outer_ring.empty()) add_poly(std::move(p));
                    }
                }
            }
        } else if (gtype == "Polygon") {
            if (coords.is_array() && !coords.empty()) {
                Polygon2D poly;
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
                add_poly(std::move(poly));
            }
        } else if (gtype == "MultiPolygon") {
            if (coords.is_array()) {
                for (const auto& poly_coords : coords) {
                    if (poly_coords.is_array()) {
                        Polygon2D poly;
                        for (std::size_t r = 0; r < poly_coords.size(); ++r) {
                            std::vector<Point2D> ring;
                            for (const auto& pt : poly_coords[r]) {
                                if (pt.is_array() && pt.size() >= 2) {
                                    ring.push_back({pt[0].get<double>(), pt[1].get<double>()});
                                }
                            }
                            if (r == 0) poly.outer_ring = std::move(ring);
                            else poly.inner_rings.push_back(std::move(ring));
                        }
                        add_poly(std::move(poly));
                    }
                }
            }
        }
    };

    if (j.contains("type")) {
        std::string type = j["type"];
        if (type == "FeatureCollection" && j.contains("features")) {
            for (const auto& feat : j["features"]) {
                if (feat.contains("geometry")) parse_feature_geom(feat["geometry"]);
            }
        } else if (type == "Feature" && j.contains("geometry")) {
            parse_feature_geom(j["geometry"]);
        } else {
            parse_feature_geom(j);
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
        bool is_point = (shape_type == 1 || shape_type == 11 || shape_type == 21);
        bool is_multipoint = (shape_type == 8 || shape_type == 18 || shape_type == 28);
        bool is_polyline_or_poly = (shape_type == 3 || shape_type == 13 || shape_type == 23 ||
                                    shape_type == 5 || shape_type == 15 || shape_type == 25);

        if (is_point) {
            if (content_length >= 20) {
                double px = read_double_le(record.data() + 4);
                double py = read_double_le(record.data() + 12);
                Polygon2D poly;
                poly.outer_ring.push_back({px, py});
                poly.compute_bounds();
                shape.polygons.push_back(std::move(poly));
            }
        } else if (is_multipoint) {
            if (content_length >= 40) {
                int32_t num_points = read_int32_le(record.data() + 36);
                if (num_points > 0 && 40 + num_points * 16 <= content_length) {
                    for (int p = 0; p < num_points; ++p) {
                        double px = read_double_le(record.data() + 40 + p * 16);
                        double py = read_double_le(record.data() + 40 + p * 16 + 8);
                        Polygon2D poly;
                        poly.outer_ring.push_back({px, py});
                        poly.compute_bounds();
                        shape.polygons.push_back(std::move(poly));
                    }
                }
            }
        } else if (is_polyline_or_poly) {
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
                if (start_idx < 0 || end_idx > num_points || start_idx >= end_idx) continue;
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

static std::vector<Point2D> parse_kml_coords_str(const std::string& raw) {
    std::vector<Point2D> pts;
    std::vector<double> nums;
    std::string current;
    for (char c : raw) {
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            current += c;
        } else {
            if (!current.empty()) {
                try { nums.push_back(std::stod(current)); } catch (...) {}
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        try { nums.push_back(std::stod(current)); } catch (...) {}
    }

    if (nums.size() >= 3 && nums.size() % 3 == 0) {
        for (std::size_t i = 0; i + 2 < nums.size(); i += 3) {
            pts.push_back({nums[i], nums[i + 1]});
        }
    } else if (nums.size() >= 2) {
        for (std::size_t i = 0; i + 1 < nums.size(); i += 2) {
            pts.push_back({nums[i], nums[i + 1]});
        }
    }
    return pts;
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
        Polygon2D poly;
        poly.outer_ring = parse_kml_coords_str(coords_str);

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
        VectorShape shape;
        shape.name = path.stem().string();
        while (pos != std::string::npos) {
            pos += tag_open.length();
            std::size_t end_pos = content.find(tag_close, pos);
            if (end_pos == std::string::npos) break;

            std::string coords_str = content.substr(pos, end_pos - pos);
            Polygon2D poly;
            poly.outer_ring = parse_kml_coords_str(coords_str);

            if (!poly.outer_ring.empty()) {
                poly.compute_bounds();
                shape.polygons.push_back(std::move(poly));
            }
            pos = content.find(tag_open, end_pos + tag_close.length());
        }
        shape.compute_bounds();
        if (!shape.polygons.empty()) return shape;
    }

    // Try finding SHP header (9994 BE: 0x00 0x00 0x27 0x0a) inside ZIP content
    std::string shp_magic = "\x00\x00\x27\x0a";
    std::size_t shp_pos = content.find(shp_magic);
    if (shp_pos != std::string::npos && shp_pos + 100 <= content.size()) {
        std::filesystem::path temp_shp = path.parent_path() / (path.stem().string() + "_zip_inner.shp");
        std::ofstream out(temp_shp, std::ios::binary);
        out.write(content.data() + shp_pos, content.size() - shp_pos);
        out.close();
        try {
            VectorShape shape = parse_shp(temp_shp);
            std::filesystem::remove(temp_shp);
            if (!shape.polygons.empty()) return shape;
        } catch (...) {
            std::filesystem::remove(temp_shp);
        }
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

    // Automatic format detection for temporary uploads or unknown file extensions (.tmp)
    // 1. Check SHP magic header (9994 BE: 0x00 0x00 0x27 0x0a)
    {
        std::ifstream f(path, std::ios::binary);
        char hdr[4];
        if (f.read(hdr, 4)) {
            if (static_cast<unsigned char>(hdr[0]) == 0x00 &&
                static_cast<unsigned char>(hdr[1]) == 0x00 &&
                static_cast<unsigned char>(hdr[2]) == 0x27 &&
                static_cast<unsigned char>(hdr[3]) == 0x0a) {
                return parse_shp(path);
            }
        }
    }

    // 2. Try SHP
    try {
        VectorShape shape = parse_shp(path);
        if (!shape.polygons.empty()) return shape;
    } catch (...) {}

    // 3. Try KML
    try {
        VectorShape shape = parse_kml(path);
        if (!shape.polygons.empty()) return shape;
    } catch (...) {}

    // 4. Try KMZ or ZIP containing SHP/KML
    try {
        VectorShape shape = parse_kmz_or_zip(path);
        if (!shape.polygons.empty()) return shape;
    } catch (...) {}

    // 5. Try GeoJSON
    try {
        std::ifstream f(path);
        nlohmann::json j = nlohmann::json::parse(f);
        VectorShape shape = parse_geojson(j);
        if (!shape.polygons.empty()) return shape;
    } catch (...) {}

    throw std::runtime_error("Formato vetorial nao suportado: " + ext);
}

} // namespace sister_image

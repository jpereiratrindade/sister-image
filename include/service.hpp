#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include "sister_image/raster_clip.hpp"
#include "sister_image/vector_shape.hpp"

namespace sister_image {
using Json = nlohmann::json;
std::string now();
std::string identifier();
std::string digest(const std::filesystem::path& path);
void save_json(const std::filesystem::path& path, const Json& value);
Json classify(const std::filesystem::path& directory, const Json& config);
void make_demo(const std::filesystem::path& path);
void original_preview(const std::filesystem::path& source, const std::filesystem::path& dest);
ClipResult clip_job(const std::filesystem::path& directory, const VectorShape& shape);
}

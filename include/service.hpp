#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace sister_image {
using Json = nlohmann::json;
std::string now();
std::string identifier();
std::string digest(const std::filesystem::path& path);
void save_json(const std::filesystem::path& path, const Json& value);
Json classify(const std::filesystem::path& directory, const Json& config);
void make_demo(const std::filesystem::path& path);
}

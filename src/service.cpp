#include "service.hpp"
#include "obce_gui/RasterWindowAnalysis.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <tiffio.h>
#include "tiff_resource.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace sister_image {
std::string now() {
    auto t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
std::string identifier() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Falha ao gerar identificador");
    std::ostringstream out;
    for (auto c : bytes) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
std::string digest(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Arquivo indisponivel para verificacao");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA256 indisponivel");
    std::array<char, 65536> buffer{};
    while (file) {
        file.read(buffer.data(), buffer.size());
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), file.gcount()) != 1) throw std::runtime_error("Falha SHA256");
    }
    if (!file.eof()) throw std::runtime_error("Falha lendo arquivo");
    std::array<unsigned char, 32> hash{};
    unsigned length{};
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &length) != 1) throw std::runtime_error("Falha SHA256");
    std::ostringstream out;
    out << "sha256:";
    for (auto c : hash) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
void save_json(const std::filesystem::path& path, const Json& value) {
    auto temp = path.string() + ".tmp";
    std::ofstream out(temp);
    out << value.dump(2) << '\n';
    out.close();
    if (!out) throw std::runtime_error("Falha salvando evidencia");
    std::filesystem::rename(temp, path);
}
static void preview(const std::filesystem::path& source, const std::filesystem::path& dest) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tif(sister_image::open_tiff(source.c_str(), "rm"), TIFFClose);
    if (!tif) throw std::runtime_error("Mapa indisponivel");
    std::uint32_t width{}, height{};
    TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height);
    const auto step = std::max<std::uint32_t>(1, (std::max(width, height) + 1023) / 1024);
    const auto pw = (width + step - 1) / step, ph = (height + step - 1) / step;
    std::ofstream out(dest, std::ios::binary);
    out << "P5\n" << pw << ' ' << ph << "\n255\n";
    std::vector<unsigned char> row(width), small(pw);
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tif.get(), row.data(), y) < 0) throw std::runtime_error("Falha lendo mapa");
        if (y % step) continue;
        for (std::uint32_t x = 0; x < pw; ++x) small[x] = row[x * step];
        out.write(reinterpret_cast<const char*>(small.data()), small.size());
    }
    out.close();
    if (!out) throw std::runtime_error("Falha salvando visualizacao");
}
Json classify(const std::filesystem::path& directory, const Json& config) {
    const auto begin = std::chrono::steady_clock::now();
    appcore::RasterWindowAnalysisConfig c;
    c.raster_path = (directory / "input.tif").string();
    c.window_size_px = c.min_window_size_px = config.at("window").get<std::size_t>();
    c.compute_anomalies = false;
    c.ignore_zero_pixels = config.at("ignore_zero").get<bool>();
    c.min_valid_fraction = config.at("min_valid").get<double>();
    c.nodata_values = config.at("nodata").get<std::vector<double>>();
    auto result = appcore::analyzeRasterWindows(c);
    appcore::RasterClassificationMapConfig m;
    m.output_path = (directory / "map.tif").string();
    m.class_count = config.at("classes").get<std::size_t>();
    m.homogeneity_threshold = config.at("homogeneity").get<double>();
    m.mode = config.at("mode") == "patches" ? appcore::RasterClassificationMapConfig::Mode::kHomogeneousPatches : appcore::RasterClassificationMapConfig::Mode::kFeatureClasses;
    appcore::writeRasterClassificationMap(result, m);
    preview(directory / "map.tif", directory / "preview.pgm");
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    Json report = {{"schema", "sister.image.result/1.0.0"}, {"algorithm", "obce-window-features/1.0.0"},
        {"configuration", config}, {"width", result.width_px}, {"height", result.height_px},
        {"windows", result.windows.size()}, {"skipped_windows", result.skipped_windows},
        {"valid_pixels", result.valid_pixels}, {"ignored_pixels", result.ignored_pixels},
        {"elapsed_seconds", elapsed}, {"completed_at", now()},
        {"input_digest", digest(directory / "input.tif")}, {"output_digest", digest(directory / "map.tif")},
        {"features", {"mean_intensity", "stddev_intensity", "mean_abs_dx"}},
        {"interpretation", "Classes visuais nao supervisionadas; tons nao sao categorias semanticas nem probabilidades."},
        {"export", "BigTIFF uint8; dimensoes originais e tags GeoTIFF basicas preservadas"}};
    save_json(directory / "report.json", report);
    return report;
}
void make_demo(const std::filesystem::path& path) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> t(sister_image::open_tiff(path.c_str(), "w"), TIFFClose);
    if (!t) throw std::runtime_error("Falha criando demonstracao");
    TIFFSetField(t.get(), TIFFTAG_IMAGEWIDTH, 512);
    TIFFSetField(t.get(), TIFFTAG_IMAGELENGTH, 384);
    TIFFSetField(t.get(), TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(t.get(), TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(t.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t.get(), TIFFTAG_ROWSPERSTRIP, 16);
    std::array<unsigned char, 512> row{};
    for (unsigned y = 0; y < 384; ++y) {
        for (unsigned x = 0; x < 512; ++x) row[x] = x < 170 ? 40 : x < 340 ? 125 + ((x + y) % 2) * 40 : 225;
        if (TIFFWriteScanline(t.get(), row.data(), y) < 0) throw std::runtime_error("Falha criando demonstracao");
    }
}
}

#include "service.hpp"
#include "sister_image/RasterWindowAnalysis.hpp"
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

void original_preview(const std::filesystem::path& source, const std::filesystem::path& dest) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tif(sister_image::open_tiff(source.c_str(), "r"), TIFFClose);
    if (!tif) throw std::runtime_error("Imagem original indisponivel");
    std::uint32_t width{}, height{};
    std::uint16_t spp{1}, bps{8};
    TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps);

    if (spp == 0) spp = 1;
    if (bps == 0) bps = 8;
    std::size_t sample_bytes = bps / 8;
    if (sample_bytes == 0) sample_bytes = 1;

    const auto step = std::max<std::uint32_t>(1, (std::max(width, height) + 1023) / 1024);
    const auto pw = (width + step - 1) / step, ph = (height + step - 1) / step;
    std::ofstream out(dest, std::ios::binary);
    out << "P5\n" << pw << ' ' << ph << "\n255\n";
    std::vector<unsigned char> row(width * spp * sample_bytes);
    std::vector<unsigned char> small(pw);
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tif.get(), row.data(), y) < 0) throw std::runtime_error("Falha lendo scanline da imagem original");
        if (y % step) continue;
        for (std::uint32_t x = 0; x < pw; ++x) {
            std::size_t sample_idx = (x * step) * spp;
            if (sample_bytes == 2) {
                const uint16_t* u16 = reinterpret_cast<const uint16_t*>(row.data());
                if (spp >= 3) {
                    double val = 0.299 * u16[sample_idx] + 0.587 * u16[sample_idx + 1] + 0.114 * u16[sample_idx + 2];
                    small[x] = static_cast<unsigned char>(std::clamp(val / 256.0, 0.0, 255.0));
                } else {
                    small[x] = static_cast<unsigned char>(std::clamp(static_cast<double>(u16[sample_idx]) / 256.0, 0.0, 255.0));
                }
            } else {
                if (spp >= 3) {
                    small[x] = static_cast<unsigned char>(0.299 * row[sample_idx] + 0.587 * row[sample_idx + 1] + 0.114 * row[sample_idx + 2]);
                } else {
                    small[x] = row[sample_idx];
                }
            }
        }
        out.write(reinterpret_cast<const char*>(small.data()), small.size());
    }
    out.close();
    if (!out) throw std::runtime_error("Falha salvando prévia da imagem original");
}

Json classify(const std::filesystem::path& directory, const Json& config) {
    const auto begin = std::chrono::steady_clock::now();
    sister_image::RasterWindowAnalysisConfig c;
    c.raster_path = (directory / "input.tif").string();
    c.window_size_px = c.min_window_size_px = config.at("window").get<std::size_t>();
    c.compute_anomalies = false;
    c.ignore_zero_pixels = config.at("ignore_zero").get<bool>();
    c.min_valid_fraction = config.at("min_valid").get<double>();
    c.nodata_values = config.at("nodata").get<std::vector<double>>();
    auto result = sister_image::analyzeRasterWindows(c);
    sister_image::RasterClassificationMapConfig m;
    m.output_path = (directory / "map.tif").string();
    m.class_count = config.at("classes").get<std::size_t>();
    m.homogeneity_threshold = config.at("homogeneity").get<double>();
    m.mode = config.at("mode") == "patches" ? sister_image::RasterClassificationMapConfig::Mode::kHomogeneousPatches : sister_image::RasterClassificationMapConfig::Mode::kFeatureClasses;
    sister_image::writeRasterClassificationMap(result, m);
    preview(directory / "map.tif", directory / "preview.pgm");
    original_preview(directory / "input.tif", directory / "original_preview.pgm");
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    Json report = {{"schema", "sister.image.result/1.0.0"}, {"algorithm", "sister-window-features/1.0.0"},
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

ClipResult clip_job(const std::filesystem::path& directory, const VectorShape& shape) {
    ClipConfig cfg;
    cfg.input_tiff = directory / "input.tif";
    cfg.output_tiff = directory / "clipped.tif";
    cfg.shape = shape;
    cfg.mask_outside = true;
    cfg.nodata_val = 0;
    auto res = clip_raster(cfg);
    original_preview(directory / "clipped.tif", directory / "clipped_preview.pgm");
    return res;
}

Json inspect_raster(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    bool is_jp2 = (ext == ".jp2" || ext == ".j2k");
    if (!is_jp2) {
        std::ifstream chk(path, std::ios::binary);
        char magic[8];
        if (chk.read(magic, 8)) {
            if ((magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 && magic[3] == 0x0c) ||
                (static_cast<unsigned char>(magic[0]) == 0xff && static_cast<unsigned char>(magic[1]) == 0x4f)) {
                is_jp2 = true;
            }
        }
    }

    if (is_jp2) {
        std::string cmd = "python3 scripts/jp2_converter.py inspect \"" + path.string() + "\"";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            std::string result;
            char buffer[512];
            while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
            pclose(pipe);
            if (!result.empty() && result.starts_with("{")) {
                auto j = Json::parse(result);
                j["digest"] = digest(path);
                return j;
            }
        }
    }

    std::unique_ptr<TIFF, decltype(&TIFFClose)> tif(sister_image::open_tiff(path.c_str(), "r"), TIFFClose);
    if (!tif) throw std::runtime_error("Nao foi possivel abrir imagem TIFF para inspecao");
    std::uint32_t width{}, height{};
    std::uint16_t spp{1}, bps{8}, photo{0};
    TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photo);

    double scale[3] = {1.0, 1.0, 0.0};
    double tie[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    uint32_t scale_count = 0, tie_count = 0;
    double *scale_ptr = nullptr, *tie_ptr = nullptr;

    bool has_scale = (TIFFGetField(tif.get(), 33550, &scale_count, &scale_ptr) == 1 && scale_count >= 2);
    bool has_tie = (TIFFGetField(tif.get(), 33922, &tie_count, &tie_ptr) == 1 && tie_count >= 6);

    if (has_scale) { scale[0] = scale_ptr[0]; scale[1] = scale_ptr[1]; }
    if (has_tie) { tie[3] = tie_ptr[3]; tie[4] = tie_ptr[4]; }

    return {
        {"width", width},
        {"height", height},
        {"channels", spp},
        {"bits_per_sample", bps},
        {"photometric", photo},
        {"has_geotiff_tags", has_scale && has_tie},
        {"scale_x", scale[0]},
        {"scale_y", scale[1]},
        {"tie_x", tie[3]},
        {"tie_y", tie[4]},
        {"digest", digest(path)}
    };
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

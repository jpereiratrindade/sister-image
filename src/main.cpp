#include "service.hpp"
#include <httplib.h>
#include <openssl/crypto.h>
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using sister_image::Json;
using namespace sister_image;
namespace {
std::string env(const char* name, std::string fallback = {}) {
    auto value = std::getenv(name);
    return value ? value : fallback;
}
Json read_json(const fs::path& p) { std::ifstream f(p); return Json::parse(f); }
void respond(httplib::Response& r, const Json& j, int code = 200) {
    r.status = code;
    r.set_content(j.dump(), "application/json; charset=utf-8");
}
void error(httplib::Response& r, int code, const std::string& message) {
    respond(r, {{"schema", "sister.subsystem.error/1.0.0"}, {"error", "request_rejected"},
        {"message", message.substr(0, 240)}, {"request_id", identifier()}}, code);
}
Json config(const httplib::Request& req) {
    auto param = [&](const char* key, const char* fallback) { return req.has_param(key) ? req.get_param_value(key) : std::string(fallback); };
    auto integer = [&](const char* key, const char* fallback, int lo, int hi) {
        auto text = param(key, fallback); std::size_t used{};
        auto value = std::stoi(text, &used);
        if (used != text.size() || value < lo || value > hi) throw std::invalid_argument(std::string("Parametro invalido: ") + key);
        return value;
    };
    auto number = [&](const char* key, const char* fallback, double lo, double hi) {
        auto text = param(key, fallback); std::size_t used{};
        auto value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value) || value < lo || value > hi) throw std::invalid_argument(std::string("Parametro invalido: ") + key);
        return value;
    };
    const auto mode = param("mode", "classes");
    if (mode != "classes" && mode != "patches") throw std::invalid_argument("Modo desconhecido");
    const auto ignore = param("ignore_zero", "true");
    if (ignore != "true" && ignore != "false") throw std::invalid_argument("ignore_zero invalido");
    Json nodata = Json::array();
    if (req.has_param("nodata") && !req.get_param_value("nodata").empty()) nodata.push_back(number("nodata", "0", 0, 255));
    return {{"window", integer("window", "128", 8, 8192)}, {"classes", integer("classes", "5", 2, 9)},
        {"homogeneity", number("homogeneity", "0.85", 0.01, 10)}, {"min_valid", number("min_valid", "0.25", 0, 1)},
        {"mode", mode}, {"ignore_zero", ignore == "true"}, {"nodata", nodata}};
}
}
int main(int argc, char** argv) {
    try {
        std::string bind = "127.0.0.1", port_text = "", data, web;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") { std::cout << "sister-image-http --bind 127.0.0.1 --port PORT --data DIR --web DIR\n"; return 0; }
            if (i + 1 >= argc) throw std::invalid_argument("Argumento sem valor");
            const std::string value = argv[++i];
            if (arg == "--bind") bind = value;
            else if (arg == "--port") port_text = value;
            else if (arg == "--data") data = value;
            else if (arg == "--web") web = value;
            else throw std::invalid_argument("Argumento desconhecido");
        }
        if (bind != "127.0.0.1") throw std::invalid_argument("Binding deve ser loopback; exposicao pertence ao sister-infra");
        std::size_t used{};
        const auto port = std::stoi(port_text, &used);
        if (used != port_text.size() || port < 1024 || port > 65535 || data.empty() || web.empty()) throw std::invalid_argument("Informe porta 1024..65535, data e web");
        const bool local = env("SISTER_IMAGE_ACCESS_MODE") == "local";
        const auto token = env("SISTER_IMAGE_PROXY_TOKEN");
        if (!local && token.size() < 32) throw std::invalid_argument("Modo proxy exige SISTER_IMAGE_PROXY_TOKEN com pelo menos 32 caracteres");
        const fs::path root = fs::absolute(data), assets = fs::absolute(web);
        if (!fs::exists(assets / "index.html")) throw std::invalid_argument("Interface web ausente");
        fs::create_directories(root);
        const int lock_fd = open((root / "instance.lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("Diretorio de dados ja esta em uso");
        const auto jobs = root / "jobs";
        fs::create_directories(jobs);
        for (const auto& entry : fs::directory_iterator(jobs)) {
            if (!entry.is_directory()) continue;
            const auto status = entry.path() / "status.json";
            if (!fs::exists(status)) continue;
            auto j = read_json(status);
            if (j.value("status", "") == "running" || j.value("status", "") == "uploading") {
                j["status"] = "failed"; j["message"] = "Execucao interrompida; envie novamente para reprocessar";
                save_json(status, j);
            }
        }
        auto manifest = read_json(assets.parent_path() / "contracts/manifest.json");
        manifest["transport"]["internal_endpoint"] = "http://" + bind + ":" + std::to_string(port);
        save_json(root / "manifest.json", manifest);
        const auto manifest_digest = digest(root / "manifest.json");
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 16); };
        server.set_payload_max_length(1024ULL * 1024 * 1024);
        server.set_read_timeout(30, 0);
        server.set_write_timeout(30, 0);
        server.set_keep_alive_max_count(10);
        server.set_default_headers({{"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "same-origin"},
            {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' blob:; frame-ancestors 'self'; object-src 'none'; base-uri 'self'"}});
        std::atomic<bool> busy{false};
        std::mutex state_mutex;
        std::thread worker;
        auto authorized = [&](const httplib::Request& req, httplib::Response& res, bool identity = false) {
            const auto supplied = req.get_header_value("X-Sister-Proxy-Token");
            const bool valid = token.size() >= 32 && supplied.size() == token.size() && CRYPTO_memcmp(supplied.data(), token.data(), token.size()) == 0;
            if (identity ? !valid : (!local && !valid)) { error(res, 401, "Mediacao autenticada pelo sisterd necessaria"); return false; }
            if (req.has_header("Origin")) {
                const auto origin = req.get_header_value("Origin");
                const auto host = req.get_header_value("Host");
                if (origin != "http://" + host && origin != "https://" + host) { error(res, 403, "Origem nao permitida"); return false; }
            }
            return true;
        };
        server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
            if ((req.path.starts_with("/api/") || req.path == "/identity" || req.path == "/echo") &&
                !authorized(req, res, req.path == "/identity" || req.path == "/echo")) {
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method != "GET" && req.method != "HEAD") {
                static const std::regex post_routes(R"(/api/(classify|demo|shapes/parse|clip|convert)|/echo)");
                static const std::regex clip_job_route(R"(/api/jobs/[0-9a-f]{32}/clip)");
                if (!std::regex_match(req.path, post_routes) && !std::regex_match(req.path, clip_job_route) && req.method != "DELETE") {
                    error(res, 404, "Rota desconhecida");
                    return httplib::Server::HandlerResponse::Handled;
                }
                if (req.path != "/api/classify" && req.path != "/api/clip") {
                    const auto size = req.get_header_value("Content-Length");
                    try {
                        if (req.has_header("Transfer-Encoding") || (!size.empty() && std::stoull(size) > 10 * 1024 * 1024)) {
                            error(res, 413, "Payload fora do limite desta operacao");
                            return httplib::Server::HandlerResponse::Handled;
                        }
                    } catch (...) {
                        error(res, 400, "Content-Length invalido");
                        return httplib::Server::HandlerResponse::Handled;
                    }
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/health", [&](const auto&, auto& r) { respond(r, {{"schema", "sister.subsystem.health/1.0.0"}, {"system_id", "sister_image"}, {"status", "ok"}, {"checked_at", now()}}); });
        server.Get("/ready", [&](const auto&, auto& r) { respond(r, {{"schema", "sister.subsystem.readiness/1.0.0"}, {"system_id", "sister_image"}, {"status", "ready"}, {"contract_version", "1.0.0"}, {"manifest_digest", manifest_digest}, {"dependencies", Json::object()}, {"degraded_capabilities", Json::array()}}); });
        server.Get("/manifest", [&](const auto&, auto& r) { respond(r, manifest); });
        server.Get("/capabilities", [&](const auto&, auto& r) {
            respond(r, {{"schema", "sister.subsystem.capabilities/1.0.0"}, {"system_id", "sister_image"}, {"contract", "sister.subsystem/1.0.0"}, {"generated_at", now()},
                {"capabilities", Json::array({
                    {{"id", "image.regions.classify"}, {"description", "Classificar regioes TIFF por intensidade e textura"}, {"risk", "low"}, {"observable_success", "Mapa TIFF e relatorio com hashes verificaveis"}},
                    {{"id", "image.shape.clip"}, {"description", "Recortar GeoTIFF por mascaras de poligonos SHP/KML/KMZ"}, {"risk", "low"}, {"observable_success", "GeoTIFF recortado e previa de bordas"}},
                    {{"id", "image.format.convert"}, {"description", "Converter formatos de visualizacao e transformar resolucao/coordenadas"}, {"risk", "low"}, {"observable_success", "Visualizacao PGM/GeoJSON produzida"}}
                })}});
        });
        server.Get("/identity", [&](const auto& req, auto& r) {
            if (!authorized(req, r, true)) return;
            Json out = {{"schema", "sister.subsystem.identity/1.0.0"}, {"origin", "sisterd"}};
            for (const auto& [header, field] : std::vector<std::pair<std::string, std::string>>{{"X-Sister-Subject","subject"},{"X-Sister-Name","name"},{"X-Sister-Email","email"},{"X-Sister-Role","role"},{"X-Request-ID","request_id"}}) {
                auto v = req.get_header_value(header);
                if (v.empty() || v.size() > 128) { error(r, 401, "Identidade mediada incompleta"); return; }
                out[field] = v;
            }
            respond(r, out);
        });
        server.Post("/echo", [&](const auto& req, auto& r) {
            if (!authorized(req, r, true)) return;
            if (req.body.size() > 70000) { error(r, 413, "Payload muito grande"); return; }
            try { auto value = Json::parse(req.body).at("value").template get<std::string>();
                if (value.size() > 65536) throw std::invalid_argument("value excede limite");
                respond(r, {{"schema", "sister.subsystem.echo/1.0.0"}, {"value", value}, {"processed_by", "sister_image"}});
            } catch (...) { error(r, 400, "Informe value como texto"); }
        });
        server.Get("/api/jobs", [&](const auto& req, auto& r) {
            if (!authorized(req, r)) return;
            std::lock_guard guard(state_mutex);
            Json list = Json::array();
            for (const auto& entry : fs::directory_iterator(jobs)) if (fs::exists(entry.path() / "status.json")) list.push_back(read_json(entry.path() / "status.json"));
            respond(r, {{"jobs", list}, {"busy", busy.load()}});
        });
        server.Get(R"(/api/jobs/([0-9a-f]{32}))", [&](const auto& req, auto& r) {
            if (!authorized(req, r)) return;
            std::lock_guard guard(state_mutex);
            const auto path = jobs / req.matches[1].str() / "status.json";
            if (!fs::exists(path)) { error(r, 404, "Execucao nao encontrada"); return; }
            respond(r, read_json(path));
        });
        server.Get(R"(/api/jobs/([0-9a-f]{32})/(map.tif|preview.pgm|original_preview.pgm|clipped.tif|clipped_preview.pgm|report.json))", [&](const auto& req, auto& r) {
            if (!authorized(req, r)) return;
            std::lock_guard guard(state_mutex);
            const auto dir = jobs / req.matches[1].str();
            if (!fs::exists(dir / "status.json") || read_json(dir / "status.json").value("status", "") != "completed") { error(r, 404, "Resultado ainda indisponivel"); return; }
            const auto name = req.matches[2].str();
            if (!fs::exists(dir / name)) { error(r, 404, "Arquivo nao encontrado"); return; }
            r.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            std::string content_type = "application/octet-stream";
            if (name.ends_with(".tif")) content_type = "image/tiff";
            else if (name.ends_with(".pgm")) content_type = "image/x-portable-graymap";
            else if (name.ends_with(".json")) content_type = "application/json";
            r.set_file_content((dir / name).string(), content_type);
        });
        server.Delete(R"(/api/jobs/([0-9a-f]{32}))", [&](const auto& req, auto& r) {
            if (!authorized(req, r)) return;
            std::lock_guard guard(state_mutex);
            const auto dir = jobs / req.matches[1].str();
            if (!fs::exists(dir / "status.json")) { error(r, 404, "Execucao nao encontrada"); return; }
            auto status = read_json(dir / "status.json").value("status", "");
            if (status == "running" || status == "uploading") { error(r, 409, "Aguarde o processamento"); return; }
            fs::remove_all(dir);
            respond(r, {{"deleted", true}});
        });

        // Endpoint para parsear vetores SHP/KML/KMZ/GeoJSON
        server.Post("/api/shapes/parse", [&](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) return;
            try {
                VectorShape shape;
                if (req.has_header("Content-Type") && req.get_header_value("Content-Type") == "application/json") {
                    shape = VectorShape::parse_geojson(Json::parse(req.body));
                } else {
                    const auto temp_path = root / ("temp_vector_" + identifier() + ".tmp");
                    std::ofstream temp(temp_path, std::ios::binary);
                    temp.write(req.body.data(), req.body.size());
                    temp.close();
                    try { shape = VectorShape::parse_file(temp_path); }
                    catch (...) { fs::remove(temp_path); throw; }
                    fs::remove(temp_path);
                }
                respond(res, {{"schema", "sister.image.shape/1.0.0"}, {"geojson", shape.to_geojson()}});
            } catch (const std::exception& e) {
                error(res, 400, std::string("Falha processando vetor: ") + e.what());
            }
        });

        // Endpoint para recortar GeoTIFF de um job por shape
        server.Post(R"(/api/jobs/([0-9a-f]{32})/clip)", [&](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req, res)) return;
            std::lock_guard guard(state_mutex);
            const auto dir = jobs / req.matches[1].str();
            if (!fs::exists(dir / "status.json") || read_json(dir / "status.json").value("status", "") != "completed") {
                error(res, 404, "Job nao concluido ou inexistente"); return;
            }
            try {
                VectorShape shape;
                if (req.body.starts_with("{")) shape = VectorShape::parse_geojson(Json::parse(req.body));
                else {
                    const auto temp_path = dir / "temp_clip_vector.tmp";
                    std::ofstream temp(temp_path, std::ios::binary);
                    temp.write(req.body.data(), req.body.size());
                    temp.close();
                    shape = VectorShape::parse_file(temp_path);
                    fs::remove(temp_path);
                }
                auto clip_res = clip_job(dir, shape);
                respond(res, {{"schema", "sister.image.clip_result/1.0.0"}, {"result", clip_res.to_json()}});
            } catch (const std::exception& e) {
                error(res, 400, std::string("Falha ao recortar TIFF: ") + e.what());
            }
        });

        auto submit = [&](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader* reader) {
            if (!authorized(req, res)) return;
            Json options;
            try { options = config(req); } catch (...) { error(res, 400, "Parametros invalidos: janela 8..8192; classes 2..9; homogeneidade 0.01..10; fracao 0..1"); return; }
            bool expected = false;
            if (!busy.compare_exchange_strong(expected, true)) { error(res, 409, "Ja existe uma tarefa em andamento"); return; }
            const auto id = identifier();
            const auto dir = jobs / id;
            try {
                {
                    std::lock_guard guard(state_mutex);
                    std::uintmax_t bytes = 0, count = 0;
                    for (const auto& e : fs::recursive_directory_iterator(jobs)) { if (e.is_regular_file()) bytes += e.file_size(); if (e.path().filename() == "status.json") ++count; }
                    if (count >= 32 || bytes > 8ULL * 1024 * 1024 * 1024 || fs::space(root).available < 6ULL * 1024 * 1024 * 1024) {
                        busy = false; error(res, 507, "Libere execucoes antigas ou espaco em disco antes de enviar"); return;
                    }
                    fs::create_directory(dir);
                    save_json(dir / "status.json", {{"schema", "sister.image.job/1.0.0"}, {"id", id}, {"status", "uploading"}, {"created_at", now()}, {"configuration", options}});
                }
                if (reader) {
                    if (req.get_header_value("Content-Type") != "image/tiff" && req.get_header_value("Content-Type") != "application/octet-stream") throw std::invalid_argument("Envie TIFF binario");
                    std::ofstream file(dir / "input.tif", std::ios::binary);
                    std::size_t bytes = 0;
                    bool ok = (*reader)([&](const char* ptr, std::size_t n) {
                        bytes += n;
                        if (bytes > 1024ULL * 1024 * 1024) return false;
                        file.write(ptr, n); return bool(file);
                    });
                    file.close();
                    if (!ok || !file || bytes < 8) throw std::invalid_argument("Upload incompleto, vazio ou superior a 1 GiB");
                } else make_demo(dir / "input.tif");
                Json status = {{"schema", "sister.image.job/1.0.0"}, {"id", id}, {"status", "running"}, {"created_at", now()}, {"configuration", options}};
                { std::lock_guard guard(state_mutex); save_json(dir / "status.json", status); }
                if (worker.joinable()) worker.join();
                worker = std::thread([&, dir, options, status]() mutable {
                    try { status["result"] = classify(dir, options); status["status"] = "completed"; }
                    catch (const std::exception& e) { status["status"] = "failed"; status["message"] = std::string(e.what()).substr(0, 240); }
                    try { std::lock_guard guard(state_mutex); save_json(dir / "status.json", status); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
                    busy = false;
                });
                respond(res, status, 202);
            } catch (const std::exception&) {
                { std::lock_guard guard(state_mutex); fs::remove_all(dir); }
                busy = false; error(res, 400, "Falha no envio: use TIFF binario ate 1 GiB");
            }
        };
        server.Post("/api/classify", [&](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) { submit(req, res, &reader); });
        server.Post("/api/demo", [&](const httplib::Request& req, httplib::Response& res) { submit(req, res, nullptr); });
        for (const std::string name : {"index.html", "app.js", "style.css"}) {
            server.Get(name == "index.html" ? "/" : "/" + name, [&, name](const auto&, auto& r) {
                r.set_file_content((assets / name).string(), name == "index.html" ? "text/html; charset=utf-8" : name == "app.js" ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
            });
        }
        server.set_error_handler([](const auto&, auto& r) { if (r.body.empty()) error(r, r.status, "Rota ou requisicao invalida"); });
        server.set_exception_handler([](const auto&, auto& r, std::exception_ptr) { error(r, 500, "Falha interna; consulte a evidencia da execucao"); });
        
        sigset_t signals;
        sigemptyset(&signals); sigaddset(&signals, SIGTERM); sigaddset(&signals, SIGINT);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if (!server.bind_to_port(bind, port)) throw std::runtime_error("Porta indisponivel");
        std::thread stop([&] { int signal{}; sigwait(&signals, &signal); server.stop(); });
        std::cout << "SisTer Image http://" << bind << ':' << port << std::endl;
        const bool ok = server.listen_after_bind();
        pthread_kill(stop.native_handle(), SIGTERM);
        stop.join();
        if (worker.joinable()) worker.join();
        close(lock_fd);
        return ok ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

#include "sip_gateway/server/rest_server.hpp"

#include "sip_gateway/logging.hpp"

namespace sip_gateway {

RestServer::RestServer(const Config& config, CallHandler on_call, TransferHandler on_transfer)
    : config_(config),
      on_call_(std::move(on_call)),
      on_transfer_(std::move(on_transfer)) {}

void RestServer::start() {
    server_ = std::make_unique<httplib::Server>();
    auto logger = logging::get_logger();

    server_->Get("/health", [logger](const httplib::Request&, httplib::Response& res) {
        nlohmann::json payload{{"status", "ok"}};
        res.set_content(payload.dump(), "application/json");
        logger->debug("Health check served");
    });

    server_->Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("", "text/plain");
    });

    server_->Post("/call", [this, logger](const httplib::Request& req, httplib::Response& res) {
        if (!authorize_request(req, res)) {
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& ex) {
            logger->error(with_kv(
                "Failed to parse /call request",
                {kv("error", ex.what())}));
            res.status = 400;
            res.set_content(R"({"message":"invalid request body"})", "application/json");
            return;
        }
        try {
            const auto payload = on_call_(body);
            write_json(res, payload);
        } catch (const std::exception& ex) {
            logger->error(with_kv(
                "Failed to handle /call request",
                {kv("error", ex.what())}));
            res.status = 500;
            res.set_content(R"({"message":"failed to start session"})", "application/json");
        }
    });

    server_->Post(R"(/transfer/([A-Za-z0-9_-]+))",
                  [this, logger](const httplib::Request& req, httplib::Response& res) {
        if (!authorize_request(req, res)) {
            return;
        }
        const auto session_id = req.matches[1].str();
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            try {
                body = nlohmann::json::parse(req.body);
            } catch (const std::exception& ex) {
                logger->error(with_kv(
                    "Failed to parse /transfer body",
                    {kv("error", ex.what())}));
                res.status = 400;
                res.set_content(R"({"message":"invalid request body"})", "application/json");
                return;
            }
        }
        try {
            const auto payload = on_transfer_(session_id, body);
            write_json(res, payload);
        } catch (const std::exception& ex) {
            logger->error(with_kv(
                "Failed to handle /transfer request",
                {kv("error", ex.what())}));
            res.status = 500;
            res.set_content(R"({"message":"transfer failed"})", "application/json");
        }
    });

    server_thread_ = std::thread([this, logger]() {
        logger->info(with_kv(
            "REST server listening",
            {kv("port", config_.sip_rest_api_port)}));
        server_->listen("0.0.0.0", config_.sip_rest_api_port);
    });
}

void RestServer::stop() {
    if (server_) {
        server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

bool RestServer::authorize_request(const httplib::Request& request,
                                   httplib::Response& response) const {
    if (!config_.authorization_token) {
        return true;
    }
    const auto it = request.headers.find("Authorization");
    if (it == request.headers.end()) {
        response.status = 401;
        response.set_content(R"({"message":"missing authorization"})", "application/json");
        return false;
    }
    const auto expected = "Bearer " + *config_.authorization_token;
    if (it->second != expected) {
        response.status = 403;
        response.set_content(R"({"message":"invalid authorization"})", "application/json");
        return false;
    }
    return true;
}

void RestServer::write_json(httplib::Response& response, const RestResponse& payload) const {
    response.status = payload.status;
    response.set_content(payload.body.dump(), "application/json");
}

}

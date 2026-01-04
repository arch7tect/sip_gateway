#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sip_gateway/config.hpp"

namespace sip_gateway {

struct RestResponse {
    int status = 200;
    nlohmann::json body;
};

class RestServer {
public:
    using CallHandler = std::function<RestResponse(const nlohmann::json&)>;
    using TransferHandler = std::function<RestResponse(const std::string&, const nlohmann::json&)>;

    RestServer(const Config& config, CallHandler on_call, TransferHandler on_transfer);

    void start();
    void stop();

private:
    bool authorize_request(const httplib::Request& request, httplib::Response& response) const;
    void write_json(httplib::Response& response, const RestResponse& payload) const;

    const Config& config_;
    CallHandler on_call_;
    TransferHandler on_transfer_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
};

}

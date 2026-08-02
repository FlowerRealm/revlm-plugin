#include <models/catalog.hpp>
#include <proxy/gateway.hpp>
#include <proxy/upstream.hpp>
#include <server/http_dispatch.hpp>
#include <server/http_server.hpp>
#include <util/json_util.hpp>
#include <util/strings.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "preload_chain.hpp"
#include "protocol_helpers.hpp"

namespace revlm
{
namespace
{

constexpr std::string_view k_channel_type = "anthropic";

using NextRouteSetup = void (*)(::httplib::Server &, const std::shared_ptr<std::atomic_bool> &);
using NextModelsForType = void (*)(std::string_view, std::vector<Model> &);
using NextAllModels = void (*)(std::vector<Model> &);
using NextPrepareUpstream = void (*)(const Channel &, const UpstreamRequest &, UpstreamPreparedRequest &);
using NextRetryUpstream = bool (*)(const Channel &, const UpstreamPreparedRequest &, const UpstreamResponse &,
                                   UpstreamPreparedRequest &);

std::vector<Model> catalog()
{
    return {
        Model(201, "claude-opus-4-8", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(202, "claude-opus-4-7", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(203, "claude-opus-4-6", "anthropic", 5, 25, 0.5, 10, 6.25, "/assets/model-icons/claude-color.svg"),
        Model(204, "claude-haiku-4-5-20251001", "anthropic", 1, 5, 0.1, 2, 1.25,
              "/assets/model-icons/claude-color.svg"),
        Model(205, "claude-sonnet-4-6", "anthropic", 3, 15, 0.3, 6, 3.75, "/assets/model-icons/claude-color.svg"),
        Model(206, "claude-sonnet-5", "anthropic", 2, 10, 0.2, 4, 3.75, "/assets/model-icons/claude-color.svg"),
    };
}

void append_unique_models(std::vector<Model> &models, const std::vector<Model> &extra)
{
    for (const Model &model : extra) {
        const auto duplicate = std::find_if(models.begin(), models.end(),
                                            [&](const Model &current) { return current.name == model.name; });
        if (duplicate == models.end()) {
            models.push_back(model);
        }
    }
}

class AnthropicMessagesGateway final : public Gateway {
public:
    explicit AnthropicMessagesGateway(ProxyRequest &request)
        : Gateway(request)
    {
    }

    void finalize(json &response) override
    {
        const json usage = response["usage"].is_object() ? response["usage"] : response["message"]["usage"];
        request.usage.input_tokens = static_cast<int>(usage["input_tokens"].as_int64().value());
        request.usage.output_tokens = static_cast<int>(usage["output_tokens"].as_int64().value());
        request.usage.cache_read_tokens = static_cast<int>(usage["cache_read_input_tokens"].as_int64().value_or(0));
        const json cache_creation = usage["cache_creation"];
        if (cache_creation.is_object()) {
            request.usage.cache_creation_1h_tokens =
                static_cast<int>(cache_creation["ephemeral_1h_input_tokens"].as_int64().value_or(0));
            request.usage.cache_creation_5m_tokens =
                static_cast<int>(cache_creation["ephemeral_5m_input_tokens"].as_int64().value_or(0));
        } else {
            request.usage.cache_creation_1h_tokens = 0;
            request.usage.cache_creation_5m_tokens = 0;
        }
        const json model_source = response["message"].is_object() ? response["message"] : response;
        if (const auto model = model_source["model"].as_string(); model.has_value() && !model->empty()) {
            request.upstream.model_name = *model;
        }
        if (const auto tier = usage["service_tier"].as_string(); tier.has_value()) {
            request.upstream.service_tier = *tier;
        } else if (const auto tier = model_source["service_tier"].as_string(); tier.has_value()) {
            request.upstream.service_tier = *tier;
        }
    }

protected:
    bool channel_ok(const Channel &channel) const override
    {
        return channel.status && channel.type == k_channel_type && !channel.api_key.empty();
    }

    GatewayFactory usage_gateway_factory() const override
    {
        return [](ProxyRequest &request) { return std::make_unique<AnthropicMessagesGateway>(request); };
    }

    std::string_view upstream_path() const override
    {
        return "/v1/messages";
    }
};

void register_anthropic_routes(::httplib::Server &server)
{
    server.Post("/v1/messages",
                v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy) {
                    if (const auto quota_error = paygo_balance_gate(proxy.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    proxy.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    if (proxy.is_stream) {
                        AnthropicMessagesGateway(proxy).run_stream(res, proxy_stream_commit_usage);
                        return;
                    }
                    write_proxy_result(res, AnthropicMessagesGateway(proxy).run());
                    finish_proxy_usage(res, proxy);
                }));
}

} // namespace

extern "C" void revlm_models_for_channel_type(std::string_view channel_type, std::vector<Model> &models)
{
    if (channel_type == k_channel_type) {
        models = catalog();
        return;
    }
    if (const auto next = revlm_plugin_common::next_symbol<NextModelsForType>("revlm_models_for_channel_type");
        next != nullptr) {
        next(channel_type, models);
        return;
    }
    models.clear();
}

extern "C" void revlm_all_models(std::vector<Model> &models)
{
    if (const auto next = revlm_plugin_common::next_symbol<NextAllModels>("revlm_all_models"); next != nullptr) {
        next(models);
    } else {
        models.clear();
    }
    append_unique_models(models, catalog());
}

extern "C" void revlm_prepare_upstream(const Channel &channel, const UpstreamRequest &downstream,
                                       UpstreamPreparedRequest &prepared)
{
    if (channel.type != k_channel_type) {
        if (const auto next = revlm_plugin_common::next_symbol<NextPrepareUpstream>("revlm_prepare_upstream");
            next != nullptr) {
            next(channel, downstream, prepared);
            return;
        }
        throw std::runtime_error("no plugin prepared this upstream request");
    }
    if (downstream.path != "/v1/messages") {
        throw std::invalid_argument("Anthropic upstream only supports /v1/messages");
    }
    revlm_plugin_common::prepare_common_headers(prepared);
    if (trim_ascii(revlm_plugin_common::header_value(prepared.headers, "anthropic-version")).empty()) {
        revlm_plugin_common::set_header(prepared.headers, "anthropic-version", "2023-06-01");
    }
    revlm_plugin_common::set_header(prepared.headers, "x-api-key", channel.api_key);
}

extern "C" bool revlm_retry_upstream_request(const Channel &channel, const UpstreamPreparedRequest &prepared,
                                             const UpstreamResponse &response, UpstreamPreparedRequest &retry)
{
    if (channel.type == k_channel_type) {
        return false;
    }
    if (const auto next = revlm_plugin_common::next_symbol<NextRetryUpstream>("revlm_retry_upstream_request");
        next != nullptr) {
        return next(channel, prepared, response, retry);
    }
    return false;
}

extern "C" void revlm_register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    if (const auto next = revlm_plugin_common::next_symbol<NextRouteSetup>("revlm_register_http_routes");
        next != nullptr) {
        next(server, draining);
    }
    register_anthropic_routes(server);
}

} // namespace revlm

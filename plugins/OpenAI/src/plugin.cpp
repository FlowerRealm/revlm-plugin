#include <models/catalog.hpp>
#include <proxy/gateway.hpp>
#include <proxy/upstream.hpp>
#include <server/http_dispatch.hpp>
#include <server/http_server.hpp>
#include <util/json.hpp>
#include <util/json_util.hpp>
#include <util/strings.hpp>

#include <algorithm>
#include <memory>
#include <regex>
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

constexpr std::string_view k_channel_type = "openai_compatible";

using NextRouteSetup = void (*)(::httplib::Server &, const std::shared_ptr<std::atomic_bool> &);
using NextModelsForType = void (*)(std::string_view, std::vector<Model> &);
using NextAllModels = void (*)(std::vector<Model> &);
using NextPrepareUpstream = void (*)(const Channel &, const UpstreamRequest &, UpstreamPreparedRequest &);
using NextRetryUpstream = bool (*)(const Channel &, const UpstreamPreparedRequest &, const UpstreamResponse &,
                                   UpstreamPreparedRequest &);

std::vector<Model> catalog()
{
    return {
        Model(101, "gpt-5.5", "openai", 5, 30, 0.5, 0, 0, "/assets/model-icons/openai.svg"),
        Model(102, "gpt-5.4", "openai", 2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg"),
        Model(103, "gpt-5.4-mini", "openai", 0.75, 4.5, 0.075, 0, 0, "/assets/model-icons/openai.svg"),
        Model(104, "gpt-5.3-codex", "openai", 1.75, 14, 0.175, 0, 0, "/assets/model-icons/openai.svg"),
        Model(105, "codex-auto-review", "openai", 2.5, 15, 0.25, 0, 0, "/assets/model-icons/openai.svg"),
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

std::string unsupported_parameter_name(std::string_view body)
{
    static const std::regex pattern("unsupported parameter[^a-z0-9_]+([a-z0-9_]+)", std::regex_constants::icase);
    std::smatch match;
    const std::string haystack{ body };
    if (std::regex_search(haystack, match, pattern) && match.size() >= 2) {
        return lowercase_ascii(match[1].str());
    }
    return {};
}

bool rewrite_body_field(std::string_view body, std::string_view source_name, std::string_view destination_name,
                        bool keep_destination, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object() || !doc->contains(source_name)) {
        return false;
    }
    json value = static_cast<const json &>(*doc)[source_name];
    doc->erase(source_name);
    if (!keep_destination || !doc->contains(destination_name)) {
        (*doc)[destination_name] = std::move(value);
    }
    out = doc->dump();
    return true;
}

bool remove_body_field(std::string_view body, std::string_view name, std::string &out)
{
    auto doc = json::parse(body);
    if (!doc || !doc->is_object() || !doc->contains(name)) {
        return false;
    }
    doc->erase(name);
    out = doc->dump();
    return true;
}

class OpenAIChatGateway final : public Gateway {
public:
    explicit OpenAIChatGateway(ProxyRequest &request)
        : Gateway(request)
    {
    }

    void finalize(json &response) override
    {
        const json usage = response["usage"];
        const long long prompt_tokens = usage["prompt_tokens"].as_int64().value();
        const long long completion_tokens = usage["completion_tokens"].as_int64().value();
        const json details = usage["prompt_tokens_details"];
        const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
        const long long cache_write_tokens =
            details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
        request.usage.input_tokens = static_cast<int>(prompt_tokens - cached_tokens);
        request.usage.output_tokens = static_cast<int>(completion_tokens);
        request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
        request.usage.cache_creation_1h_tokens = 0;
        request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
        if (const auto tier = response["service_tier"].as_string(); tier.has_value()) {
            request.upstream.service_tier = *tier;
        }
        if (const auto model = response["model"].as_string(); model.has_value() && !model->empty()) {
            request.upstream.model_name = *model;
        }
    }

protected:
    bool channel_ok(const Channel &channel) const override
    {
        return channel.status && channel.type == k_channel_type && !channel.api_key.empty();
    }

    GatewayFactory usage_gateway_factory() const override
    {
        return [](ProxyRequest &request) { return std::make_unique<OpenAIChatGateway>(request); };
    }

    std::string_view upstream_path() const override
    {
        return "/v1/chat/completions";
    }
};

class OpenAIResponsesGateway final : public Gateway {
public:
    explicit OpenAIResponsesGateway(ProxyRequest &request)
        : Gateway(request)
    {
    }

    void finalize(json &response) override
    {
        const json nested_response = response["response"];
        const json usage = (nested_response.is_object() && nested_response["usage"].is_object()) ?
                               nested_response["usage"] :
                               response["usage"];
        const long long input_tokens = usage["input_tokens"].as_int64().value();
        const long long output_tokens = usage["output_tokens"].as_int64().value();
        const json details = usage["input_tokens_details"];
        const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
        const long long cache_write_tokens =
            details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
        request.usage.input_tokens = static_cast<int>(input_tokens - cached_tokens);
        request.usage.output_tokens = static_cast<int>(output_tokens);
        request.usage.cache_read_tokens = static_cast<int>(cached_tokens);
        request.usage.cache_creation_1h_tokens = 0;
        request.usage.cache_creation_5m_tokens = static_cast<int>(cache_write_tokens);
        const json metadata = nested_response.is_object() ? nested_response : response;
        if (const auto tier = metadata["service_tier"].as_string(); tier.has_value()) {
            request.upstream.service_tier = *tier;
        }
        if (const auto model = metadata["model"].as_string(); model.has_value() && !model->empty()) {
            request.upstream.model_name = *model;
        }
    }

protected:
    bool channel_ok(const Channel &channel) const override
    {
        return channel.status && channel.type == k_channel_type && !channel.api_key.empty();
    }

    GatewayFactory usage_gateway_factory() const override
    {
        return [](ProxyRequest &request) { return std::make_unique<OpenAIResponsesGateway>(request); };
    }

    std::string_view upstream_path() const override
    {
        return request.http.path;
    }

    UpstreamRequest make_upstream(bool stream) const override
    {
        UpstreamRequest downstream;
        downstream.method = request.http.method;
        downstream.path = request.http.path;
        downstream.body = request.http.body;
        downstream.headers = {
            { "Content-Type", "application/json" },
            { "Accept", stream ? "text/event-stream" : "application/json" },
            { "X-Request-Id", request.request_id },
        };
        return downstream;
    }

    void fill_success_pricing(ProxyRequest &proxy, const Channel &channel) override
    {
        Gateway::fill_success_pricing(proxy, channel);
        const Model *model = channel.find_model(proxy.upstream.model_name);
        const int official_input_tokens = proxy.usage.input_tokens + proxy.usage.cache_read_tokens;
        if (proxy.upstream.service_tier == "priority") {
            proxy.upstream.tier_multiplier = 2.0;
        } else if (model != nullptr && model->owned_by == "openai" && official_input_tokens > 272000) {
            proxy.upstream.tier_multiplier = 2.0;
        }
    }

    bool should_bill_non_stream() const override
    {
        return request.http.path != "/v1/responses/input_tokens";
    }

    bool prepare(::httplib::Response &response) override
    {
        if (request.http.method == "POST") {
            return true;
        }
        write_upstream(response, 405, serialize(json{ { "error", json{ { "message", "method not allowed" } } } }),
                       { { "X-Request-Id", request.request_id },
                         { "Content-Type", "application/json; charset=utf-8" } });
        return false;
    }
};

void register_openai_routes(::httplib::Server &server)
{
    server.Post("/v1/chat/completions",
                v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy) {
                    if (const auto quota_error = paygo_balance_gate(proxy.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    proxy.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    if (proxy.is_stream) {
                        OpenAIChatGateway(proxy).run_stream(res, proxy_stream_commit_usage);
                        return;
                    }
                    write_proxy_result(res, OpenAIChatGateway(proxy).run());
                    finish_proxy_usage(res, proxy);
                }));
    server.Post("/v1/responses",
                v1_http([](const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy) {
                    if (const auto quota_error = paygo_balance_gate(proxy.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    proxy.is_stream = parse_json_bool_field(req.body, "stream").value_or(false);
                    Gateway::StreamOptions options;
                    if (proxy.is_stream) {
                        options.stream_response = &res;
                        options.on_usage = proxy_stream_commit_usage;
                    }
                    const Gateway::HandleResult result = OpenAIResponsesGateway(proxy).handle(res, options);
                    if (!result.handled_stream) {
                        finish_proxy_usage(res, proxy);
                    }
                }));
    server.Post("/v1/responses/input_tokens",
                v1_http([](const ::httplib::Request &, ::httplib::Response &res, ProxyRequest &proxy) {
                    if (const auto quota_error = paygo_balance_gate(proxy.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    (void)OpenAIResponsesGateway(proxy).handle(res);
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
    revlm_plugin_common::prepare_common_headers(prepared);
    revlm_plugin_common::set_header(prepared.headers, "Authorization", "Bearer " + channel.api_key);
}

extern "C" bool revlm_retry_upstream_request(const Channel &channel, const UpstreamPreparedRequest &prepared,
                                             const UpstreamResponse &response, UpstreamPreparedRequest &retry)
{
    if (channel.type != k_channel_type) {
        if (const auto next = revlm_plugin_common::next_symbol<NextRetryUpstream>("revlm_retry_upstream_request");
            next != nullptr) {
            return next(channel, prepared, response, retry);
        }
        return false;
    }
    if (prepared.retried_unsupported_parameter || response.status_code < 400 || response.status_code >= 500) {
        return false;
    }
    const std::string parameter = unsupported_parameter_name(response.body);
    std::string body;
    bool rewritten = false;
    if (parameter == "max_output_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_output_tokens", "max_tokens", true, body);
    } else if (parameter == "max_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_tokens", "max_output_tokens", false, body);
    } else if (parameter == "max_completion_tokens") {
        rewritten = rewrite_body_field(prepared.body, "max_completion_tokens", "max_tokens", true, body);
    } else if (parameter == "stream_options") {
        rewritten = remove_body_field(prepared.body, "stream_options", body);
    }
    if (!rewritten || body.empty() || body == prepared.body) {
        return false;
    }
    retry = prepared;
    retry.body = std::move(body);
    retry.retried_unsupported_parameter = true;
    return true;
}

extern "C" void revlm_register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    if (const auto next = revlm_plugin_common::next_symbol<NextRouteSetup>("revlm_register_http_routes");
        next != nullptr) {
        next(server, draining);
    }
    register_openai_routes(server);
}

} // namespace revlm

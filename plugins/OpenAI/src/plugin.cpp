#include <models/catalog.hpp>
#include <proxy/gateway.hpp>
#include <proxy/openai_chat.hpp>
#include <proxy/openai_responses.hpp>
#include <proxy/upstream.hpp>
#include <util/json_util.hpp>

#include <memory>
#include <string_view>
#include <utility>

#include "protocol_helpers.hpp"

namespace revlm
{
namespace
{

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
        const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
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
        return channel.status && channel.type == "openai_compatible" && !channel.api_key.empty();
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
        const json usage = (nested_response.is_object() && nested_response["usage"].is_object())
                               ? nested_response["usage"]
                               : response["usage"];
        const long long input_tokens = usage["input_tokens"].as_int64().value();
        const long long output_tokens = usage["output_tokens"].as_int64().value();
        const json details = usage["input_tokens_details"];
        const long long cached_tokens = details.is_object() ? details["cached_tokens"].as_int64().value_or(0) : 0;
        const long long cache_write_tokens = details.is_object() ? details["cache_write_tokens"].as_int64().value_or(0) : 0;
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
        return channel.status && channel.type == "openai_compatible" && !channel.api_key.empty();
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
                       { { "X-Request-Id", request.request_id }, { "Content-Type", "application/json; charset=utf-8" } });
        return false;
    }
};

} // namespace

std::vector<Model> openai_models()
{
    return {
        Model(101, "gpt-5.5", "openai", 5, 30, 0.5, 0, 0),
        Model(102, "gpt-5.4", "openai", 2.5, 15, 0.25, 0, 0),
        Model(103, "gpt-5.4-mini", "openai", 0.75, 4.5, 0.075, 0, 0),
        Model(104, "gpt-5.3-codex", "openai", 1.75, 14, 0.175, 0, 0),
        Model(105, "codex-auto-review", "openai", 2.5, 15, 0.25, 0, 0),
    };
}

void prepare_openai_upstream(const Channel &channel, const UpstreamRequest &, UpstreamPreparedRequest &prepared)
{
    revlm_plugin_common::prepare_common_headers(prepared);
    revlm_plugin_common::set_header(prepared.headers, "Authorization", "Bearer " + channel.api_key);
}

bool retry_openai_unsupported_parameter()
{
    return true;
}

json run_chat_completions(ProxyRequest &request)
{
    return OpenAIChatGateway(request).run();
}

void run_chat_completions_stream(::httplib::Response &response, ProxyRequest request,
                                 const std::function<void(ProxyRequest &)> &on_usage)
{
    OpenAIChatGateway(request).run_stream(response, on_usage);
}

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &request, ::httplib::Response &response)
{
    return handle_responses_proxy_request(request, response, ResponsesProxyExecuteOptions{});
}

ResponsesProxyResult handle_responses_proxy_request(ProxyRequest &request, ::httplib::Response &response,
                                                    const ResponsesProxyExecuteOptions &options)
{
    return OpenAIResponsesGateway(request).handle(response, options);
}

} // namespace revlm

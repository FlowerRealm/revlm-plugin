#include <models/catalog.hpp>
#include <proxy/anthropics_messages.hpp>
#include <proxy/gateway.hpp>
#include <proxy/upstream.hpp>
#include <util/json_util.hpp>
#include <util/strings.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>

#include "protocol_helpers.hpp"

namespace revlm
{
namespace
{

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
        return channel.status && channel.type == "anthropic" && !channel.api_key.empty();
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

} // namespace

std::vector<Model> anthropic_models()
{
    return {
        Model(201, "claude-opus-4-8", "anthropic", 5, 25, 0.5, 10, 6.25),
        Model(202, "claude-opus-4-7", "anthropic", 5, 25, 0.5, 10, 6.25),
        Model(203, "claude-opus-4-6", "anthropic", 5, 25, 0.5, 10, 6.25),
        Model(204, "claude-haiku-4-5-20251001", "anthropic", 1, 5, 0.1, 2, 1.25),
        Model(205, "claude-sonnet-4-6", "anthropic", 3, 15, 0.3, 6, 3.75),
        Model(206, "claude-sonnet-5", "anthropic", 2, 10, 0.2, 4, 3.75),
    };
}

void prepare_anthropic_upstream(const Channel &channel, const UpstreamRequest &downstream,
                                UpstreamPreparedRequest &prepared)
{
    if (downstream.path != "/v1/messages") {
        throw std::invalid_argument("anthropic upstream only supports /v1/messages");
    }
    revlm_plugin_common::prepare_common_headers(prepared);
    if (trim_ascii(revlm_plugin_common::header_value(prepared.headers, "anthropic-version")).empty()) {
        revlm_plugin_common::set_header(prepared.headers, "anthropic-version", "2023-06-01");
    }
    revlm_plugin_common::set_header(prepared.headers, "x-api-key", channel.api_key);
}

bool retry_anthropic_unsupported_parameter()
{
    return false;
}

json run_messages(ProxyRequest &request)
{
    return AnthropicMessagesGateway(request).run();
}

void run_messages_stream(::httplib::Response &response, ProxyRequest request,
                         const std::function<void(ProxyRequest &)> &on_usage)
{
    AnthropicMessagesGateway(request).run_stream(response, on_usage);
}

} // namespace revlm

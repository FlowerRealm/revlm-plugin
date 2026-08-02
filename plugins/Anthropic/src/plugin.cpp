#include <plugins/sdk.hpp>

#include <plugins/gateway.hpp>
#include <util/json_util.hpp>
#include <util/strings.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "protocol_helpers.hpp"

namespace revlm_anthropic_plugin
{

using namespace revlm;

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

class MessagesHandler final : public plugin::v1::DataPlaneHandler {
public:
    plugin::v1::DataPlaneResult handle(plugin::v1::AuthenticatedRequest &request, plugin::v1::ResponseWriter &response,
                                       plugin::v1::HostServices &host) override
    {
        ProxyRequest &proxy = request.proxy;
        proxy.is_stream = parse_json_bool_field(request.http.body, "stream").value_or(false);
        AnthropicMessagesGateway gateway(proxy);
        if (proxy.is_stream) {
            gateway.run_stream(response.native(), host.stream_usage_callback());
            return { .handled_stream = true, .commit_usage = false };
        }
        response.write_proxy_result(gateway.run());
        return {};
    }
};

plugin::v1::ChannelTypeDescriptor channel_type()
{
    plugin::v1::ChannelTypeDescriptor descriptor;
    descriptor.type_id = "anthropic";
    descriptor.display_name = "Anthropic";
    descriptor.icon = "ri-sparkling-2-line";
    descriptor.default_name = "Anthropic 渠道";
    descriptor.default_base_url = "https://api.anthropic.com";
    descriptor.models = anthropic_models();
    descriptor.frontend_schema = json({ { "fields", json::array() } });
    descriptor.retry_unsupported_parameter = false;
    descriptor.prepare_upstream = [](const Channel &channel, const UpstreamRequest &downstream,
                                     UpstreamPreparedRequest &prepared) {
        if (downstream.path != "/v1/messages") {
            throw std::invalid_argument("anthropic upstream only supports /v1/messages");
        }
        revlm_plugin_common::prepare_common_headers(prepared);
        if (trim_ascii(revlm_plugin_common::header_value(prepared.headers, "anthropic-version")).empty()) {
            revlm_plugin_common::set_header(prepared.headers, "anthropic-version", "2023-06-01");
        }
        revlm_plugin_common::set_header(prepared.headers, "x-api-key", channel.api_key);
    };
    return descriptor;
}

class AnthropicPlugin final : public plugin::v1::Plugin {
public:
    void register_with(plugin::v1::PluginRegistrar &registrar) override
    {
        registrar.register_channel_type(channel_type());
        registrar.register_data_plane_route({ "POST", "/v1/messages", true, true }, messages_);
    }

private:
    MessagesHandler messages_;
};

} // namespace revlm_anthropic_plugin

extern "C" revlm::plugin::v1::Plugin *revlm_plugin_create_v1()
{
    return new revlm_anthropic_plugin::AnthropicPlugin();
}

extern "C" void revlm_plugin_destroy_v1(revlm::plugin::v1::Plugin *plugin)
{
    delete plugin;
}

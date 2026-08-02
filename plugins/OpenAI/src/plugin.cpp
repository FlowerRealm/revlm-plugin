#include <plugins/sdk.hpp>

#include <plugins/gateway.hpp>
#include <util/json_util.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "protocol_helpers.hpp"

namespace revlm_openai_plugin
{

using namespace revlm;

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

class ModelsHandler final : public plugin::v1::DataPlaneHandler {
public:
    plugin::v1::DataPlaneResult handle(plugin::v1::AuthenticatedRequest &request, plugin::v1::ResponseWriter &response,
                                       plugin::v1::HostServices &host) override
    {
        try {
            response.write_json(200, host.list_models(request.proxy.auth.channel_group_id));
        } catch (const std::exception &) {
            response.write_json(502, json("查询模型目录失败"));
        }
        return { .commit_usage = false };
    }
};

class ModelHandler final : public plugin::v1::DataPlaneHandler {
public:
    plugin::v1::DataPlaneResult handle(plugin::v1::AuthenticatedRequest &request, plugin::v1::ResponseWriter &response,
                                       plugin::v1::HostServices &host) override
    {
        try {
            const auto it = request.http.path_params.find("model_id");
            if (it == request.http.path_params.end()) {
                throw std::runtime_error("missing model_id");
            }
            bool not_found = false;
            json body = host.retrieve_model(it->second, request.proxy.auth.channel_group_id, not_found);
            response.write_json(not_found ? 404 : 200, std::move(body));
        } catch (const std::exception &) {
            response.write_json(502, json("查询模型目录失败"));
        }
        return { .commit_usage = false };
    }
};

class ChatHandler final : public plugin::v1::DataPlaneHandler {
public:
    plugin::v1::DataPlaneResult handle(plugin::v1::AuthenticatedRequest &request, plugin::v1::ResponseWriter &response,
                                       plugin::v1::HostServices &host) override
    {
        ProxyRequest &proxy = request.proxy;
        proxy.is_stream = parse_json_bool_field(request.http.body, "stream").value_or(false);
        OpenAIChatGateway gateway(proxy);
        if (proxy.is_stream) {
            gateway.run_stream(response.native(), host.stream_usage_callback());
            return { .handled_stream = true, .commit_usage = false };
        }
        response.write_proxy_result(gateway.run());
        return {};
    }
};

class ResponsesHandler final : public plugin::v1::DataPlaneHandler {
public:
    explicit ResponsesHandler(bool input_tokens)
        : input_tokens_(input_tokens)
    {
    }

    plugin::v1::DataPlaneResult handle(plugin::v1::AuthenticatedRequest &request, plugin::v1::ResponseWriter &response,
                                       plugin::v1::HostServices &host) override
    {
        ProxyRequest &proxy = request.proxy;
        proxy.is_stream = !input_tokens_ && parse_json_bool_field(request.http.body, "stream").value_or(false);
        OpenAIResponsesGateway gateway(proxy);
        Gateway::StreamOptions options;
        if (proxy.is_stream) {
            options.stream_response = &response.native();
            options.on_usage = host.stream_usage_callback();
        }
        const Gateway::HandleResult result = gateway.handle(response.native(), options);
        if (result.handled_stream) {
            return { .handled_stream = true, .commit_usage = false };
        }
        return { .commit_usage = !input_tokens_ };
    }

private:
    bool input_tokens_ = false;
};

plugin::v1::ChannelTypeDescriptor channel_type()
{
    plugin::v1::ChannelTypeDescriptor descriptor;
    descriptor.type_id = "openai_compatible";
    descriptor.display_name = "OpenAI";
    descriptor.icon = "ri-openai-fill";
    descriptor.default_name = "OpenAI 渠道";
    descriptor.default_base_url = "https://api.openai.com/v1";
    descriptor.models = openai_models();
    descriptor.frontend_schema = json({ { "fields", json::array() } });
    descriptor.prepare_upstream = [](const Channel &channel, const UpstreamRequest &, UpstreamPreparedRequest &prepared) {
        revlm_plugin_common::prepare_common_headers(prepared);
        revlm_plugin_common::set_header(prepared.headers, "Authorization", "Bearer " + channel.api_key);
    };
    return descriptor;
}

class OpenAIPlugin final : public plugin::v1::Plugin {
public:
    void register_with(plugin::v1::PluginRegistrar &registrar) override
    {
        registrar.register_channel_type(channel_type());
        registrar.register_data_plane_route({ "GET", "/v1/models", false, false }, models_);
        registrar.register_data_plane_route({ "GET", "/v1/models/:model_id", false, false }, model_);
        registrar.register_data_plane_route({ "POST", "/v1/chat/completions", true, true }, chat_);
        registrar.register_data_plane_route({ "POST", "/v1/responses", true, true }, responses_);
        registrar.register_data_plane_route({ "POST", "/v1/responses/input_tokens", true, false }, input_tokens_);
    }

private:
    ModelsHandler models_;
    ModelHandler model_;
    ChatHandler chat_;
    ResponsesHandler responses_{ false };
    ResponsesHandler input_tokens_{ true };
};

} // namespace revlm_openai_plugin

extern "C" revlm::plugin::v1::Plugin *revlm_plugin_create_v1()
{
    return new revlm_openai_plugin::OpenAIPlugin();
}

extern "C" void revlm_plugin_destroy_v1(revlm::plugin::v1::Plugin *plugin)
{
    delete plugin;
}

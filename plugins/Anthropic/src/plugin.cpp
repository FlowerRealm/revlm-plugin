// Revlm v3 Anthropic channel plugin.
//
// Data-plane entry: extern "C" revlm_handle_v1. Owns the "anthropic"
// ChannelGroup.type: POST /v1/messages (streaming and non-streaming).
// Non-matching types chain to the next revlm_handle_v1 implementation via
// RTLD_NEXT.
//
// The plugin is an LD_PRELOAD module, so its definitions of the ordinary
// interposable symbols (revlm_handle_v1, revlm_prepare_upstream) win over the
// core's defaults. Billing: the plugin parses the Messages usage object, fills
// ProxyRequest::token_details and computes protocol_cost_usd from its own
// model pricing, then calls revlm_commit_request exactly once per request.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>

#include <proxy/gateway.hpp>
#include <proxy/upstream.hpp>
#include <request/proxy_request.hpp>
#include <util/json.hpp>
#include <util/json_util.hpp>

#include "plugin_common.hpp"
#include "plugin_stream.hpp"

namespace revlm_anthropic_plugin
{

using namespace revlm;
using revlm_plugin_common::AttemptOutcome;
using revlm_plugin_common::SseAccumulator;
using revlm_plugin_common::make_upstream_request;
using revlm_plugin_common::relay_upstream_stream;
using revlm_plugin_common::write_client_error;
using revlm_plugin_common::write_transport_error;

namespace
{

constexpr std::string_view kMessagesPath = "/v1/messages";
constexpr std::string_view kAnthropicVersion = "2023-06-01";

// ---------------------------------------------------------------------------
// Model catalog + pricing (owned by this plugin).
// ---------------------------------------------------------------------------

struct Pricing {
    double input_usd_per_1m = 0.0;
    double output_usd_per_1m = 0.0;
    double cache_read_usd_per_1m = 0.0;
    double cache_creation_1h_usd_per_1m = 0.0;
    double cache_creation_5m_usd_per_1m = 0.0;
};

const std::unordered_map<std::string, Pricing> &anthropic_pricing()
{
    static const std::unordered_map<std::string, Pricing> pricing = {
        { "claude-opus-4-8", { 5.0, 25.0, 0.5, 6.25, 10.0 } },
        { "claude-opus-4-7", { 5.0, 25.0, 0.5, 6.25, 10.0 } },
        { "claude-opus-4-6", { 5.0, 25.0, 0.5, 6.25, 10.0 } },
        { "claude-haiku-4-5-20251001", { 1.0, 5.0, 0.1, 1.25, 2.0 } },
        { "claude-sonnet-4-6", { 3.0, 15.0, 0.3, 3.75, 6.0 } },
        { "claude-sonnet-5", { 2.0, 10.0, 0.2, 3.75, 4.0 } },
    };
    return pricing;
}

const std::vector<std::string> &anthropic_model_ids()
{
    static const std::vector<std::string> ids = {
        "claude-opus-4-8", "claude-opus-4-7", "claude-opus-4-6", "claude-haiku-4-5-20251001",
        "claude-sonnet-4-6", "claude-sonnet-5",
    };
    return ids;
}

std::optional<Pricing> find_pricing(std::string_view model)
{
    const auto &table = anthropic_pricing();
    const auto it = table.find(std::string{ model });
    if (it == table.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string default_model()
{
    return anthropic_model_ids().front();
}

// ---------------------------------------------------------------------------
// Usage extraction (Anthropic Messages usage object).
// ---------------------------------------------------------------------------

struct UsageBreakdown {
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_input_tokens = 0;
    long long cache_creation_1h_tokens = 0;
    long long cache_creation_5m_tokens = 0;
    std::string model;
};

long long json_ll(const json &value)
{
    return value.as_int64().value_or(0);
}

UsageBreakdown parse_messages_usage(const json &usage)
{
    UsageBreakdown out;
    if (!usage.is_object()) {
        return out;
    }
    out.input_tokens = json_ll(usage["input_tokens"]);
    out.output_tokens = json_ll(usage["output_tokens"]);
    out.cache_read_input_tokens = json_ll(usage["cache_read_input_tokens"]);
    const json cache_creation = usage["cache_creation"];
    if (cache_creation.is_object()) {
        out.cache_creation_1h_tokens = json_ll(cache_creation["ephemeral_1h_input_tokens"]);
        out.cache_creation_5m_tokens = json_ll(cache_creation["ephemeral_5m_input_tokens"]);
    }
    return out;
}

// Build the canonical token_details JSON (the core's usage_tokens and
// compute_pricing_breakdown read input_tokens, output_tokens,
// cache_read_input_tokens and cache_creation.ephemeral_*).
std::string build_token_details(const UsageBreakdown &usage)
{
    json details;
    json usage_json;
    usage_json["input_tokens"] = usage.input_tokens;
    usage_json["output_tokens"] = usage.output_tokens;
    usage_json["cache_read_input_tokens"] = usage.cache_read_input_tokens;
    if (usage.cache_creation_1h_tokens > 0 || usage.cache_creation_5m_tokens > 0) {
        json cache_creation;
        if (usage.cache_creation_1h_tokens > 0) {
            cache_creation["ephemeral_1h_input_tokens"] = usage.cache_creation_1h_tokens;
        }
        if (usage.cache_creation_5m_tokens > 0) {
            cache_creation["ephemeral_5m_input_tokens"] = usage.cache_creation_5m_tokens;
        }
        usage_json["cache_creation"] = std::move(cache_creation);
    }
    details["usage"] = std::move(usage_json);
    return details.dump();
}

double compute_protocol_cost(const UsageBreakdown &usage, std::string_view model_name)
{
    const auto pricing = find_pricing(model_name);
    if (!pricing.has_value()) {
        return 0.0;
    }
    const long long billable_input =
        usage.input_tokens - usage.cache_read_input_tokens - usage.cache_creation_1h_tokens -
        usage.cache_creation_5m_tokens;
    const double input_usd = std::max(0LL, billable_input) / 1.0e6 * pricing->input_usd_per_1m;
    const double output_usd = usage.output_tokens / 1.0e6 * pricing->output_usd_per_1m;
    const double cache_read_usd = usage.cache_read_input_tokens / 1.0e6 * pricing->cache_read_usd_per_1m;
    const double cache_1h_usd = usage.cache_creation_1h_tokens / 1.0e6 * pricing->cache_creation_1h_usd_per_1m;
    const double cache_5m_usd = usage.cache_creation_5m_tokens / 1.0e6 * pricing->cache_creation_5m_usd_per_1m;
    return input_usd + output_usd + cache_read_usd + cache_1h_usd + cache_5m_usd;
}

// ---------------------------------------------------------------------------
// Upstream request construction.
// ---------------------------------------------------------------------------

struct UpstreamContext {
    std::string auth_header;   // "x-api-key: <key>"
    std::string version_value; // "anthropic-version: <version>"
};

std::optional<UpstreamContext> make_upstream_context(const ProxyRequest &proxy)
{
    const Channel *channel = revlm_plugin_common::selected_channel(proxy);
    if (channel == nullptr || channel->api_key.empty()) {
        return std::nullopt;
    }
    return UpstreamContext{ channel->api_key, std::string{ kAnthropicVersion } };
}

UpstreamRequest make_messages_upstream(const ProxyRequest &proxy, const UpstreamContext &ctx, bool stream)
{
    UpstreamRequest downstream = make_upstream_request(proxy, kMessagesPath,
                                                       { { "X-Api-Key", ctx.auth_header },
                                                         { "anthropic-version", ctx.version_value },
                                                         { "Accept", stream ? "text/event-stream" : "application/json" } },
                                                       stream);
    return downstream;
}

// ---------------------------------------------------------------------------
// Billing termination.
// ---------------------------------------------------------------------------

struct NonStreamResult {
    int status = 0;
    std::string body;
    std::vector<UpstreamHeader> headers;
};

void commit_and_respond(::httplib::Response &res, ProxyRequest &proxy, NonStreamResult result,
                        const UsageBreakdown &usage, std::string_view model_name, int latency_ms)
{
    const std::string response_id = upstream_response_id_from_headers(result.headers);
    if (result.status >= 200 && result.status < 300) {
        proxy.token_details = build_token_details(usage);
        proxy.upstream.model_name = !usage.model.empty() ? usage.model : std::string{ model_name };
        proxy.protocol_cost_usd = compute_protocol_cost(usage, proxy.upstream.model_name);
    } else {
        proxy.error_class = "upstream";
        proxy.error_message = result.status >= 500 ? "upstream error" : "upstream rejected request";
    }
    revlm_plugin_common::fill_commit_fields(proxy, result.status, latency_ms, 0, response_id);
    (void)revlm_commit_request(proxy);
    write_upstream(res, result.status, result.body, result.headers);
}

std::vector<UpstreamHeader> with_request_id(ProxyRequest &proxy, std::vector<UpstreamHeader> headers)
{
    headers.push_back({ "X-Request-Id", proxy.request_id });
    return headers;
}

// ---------------------------------------------------------------------------
// POST /v1/messages (non-streaming).
// ---------------------------------------------------------------------------

void handle_messages_non_stream(::httplib::Response &res, ProxyRequest &proxy)
{
    const auto ctx = make_upstream_context(proxy);
    if (!ctx.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(default_model());
    UpstreamRequest downstream = make_messages_upstream(proxy, *ctx, /*stream=*/false);
    UpstreamExecutionResult executed;
    const auto started = std::chrono::steady_clock::now();
    const AttemptOutcome outcome = revlm_plugin_common::execute_non_stream(proxy, std::move(downstream), executed);
    const int latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - started)
                                                .count());
    if (outcome == AttemptOutcome::Exhausted) {
        write_transport_error(res, proxy);
        return;
    }
    UsageBreakdown usage;
    if (outcome != AttemptOutcome::Fatal) {
        const auto parsed = json::parse(executed.response.body);
        if (parsed.has_value() && parsed->is_object()) {
            const json message = (*parsed)["message"];
            const json usage_source = (*parsed)["usage"].is_object() ? (*parsed)["usage"] :
                                                                       (message.is_object() ? message["usage"] : json{});
            usage = parse_messages_usage(usage_source);
            const json model_source = message.is_object() ? message : *parsed;
            if (const auto m = model_source["model"].as_string(); m.has_value() && !m->empty()) {
                usage.model = *m;
            }
        }
    }
    if (outcome == AttemptOutcome::Fatal || executed.response.status_code >= 500) {
        commit_and_respond(res, proxy, { executed.response.status_code, executed.response.body,
                                         with_request_id(proxy, executed.response.headers) },
                           UsageBreakdown{}, model, latency_ms);
        return;
    }
    commit_and_respond(res, proxy, { executed.response.status_code, executed.response.body,
                                     with_request_id(proxy, executed.response.headers) },
                       usage, model, latency_ms);
}

// ---------------------------------------------------------------------------
// POST /v1/messages (streaming). The upstream returns SSE frames; the final
// message_delta event carries the usage object. The relay runs inside the
// chunked content provider; the plugin scans data frames for usage and commits
// it in after_pump.
// ---------------------------------------------------------------------------

struct StreamUsageScan {
    UsageBreakdown usage;
    bool saw_usage = false;
};

bool parse_stream_message(const std::string &payload, StreamUsageScan &scan)
{
    const auto parsed = json::parse(payload);
    if (!parsed.has_value() || !parsed->is_object()) {
        return false;
    }
    const auto type = (*parsed)["type"].as_string();
    if (!type.has_value()) {
        return false;
    }
    if (*type == "message_delta") {
        const json usage = (*parsed)["usage"];
        if (usage.is_object()) {
            const UsageBreakdown parsed_usage = parse_messages_usage(usage);
            if (parsed_usage.input_tokens > 0 || parsed_usage.output_tokens > 0 || parsed_usage.cache_read_input_tokens > 0) {
                scan.usage = parsed_usage;
                scan.saw_usage = true;
            }
        }
    } else if (*type == "message_start") {
        const json message = (*parsed)["message"];
        if (message.is_object()) {
            if (const auto m = message["model"].as_string(); m.has_value() && !m->empty()) {
                scan.usage.model = *m;
            }
        }
    }
    return true;
}

void handle_messages_stream(::httplib::Response &res, ProxyRequest &proxy)
{
    proxy.is_stream = true;
    const auto ctx = make_upstream_context(proxy);
    if (!ctx.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(default_model());
    UpstreamRequest downstream = make_messages_upstream(proxy, *ctx, /*stream=*/true);
    UpstreamStreamResponse upstream;
    const AttemptOutcome outcome = revlm_plugin_common::open_stream(proxy, std::move(downstream), upstream);
    if (outcome == AttemptOutcome::Exhausted) {
        write_transport_error(res, proxy);
        return;
    }
    if (outcome == AttemptOutcome::Fatal || upstream.status_code >= 500) {
        std::string body = upstream.initial_body;
        body += read_remaining_stream(upstream.stream);
        if (upstream.stream.close) {
            upstream.stream.close();
        }
        proxy.error_class = "upstream";
        proxy.error_message = upstream.status_code >= 500 ? "upstream error" : "upstream rejected request";
        revlm_plugin_common::fill_commit_fields(proxy, upstream.status_code, 0, 0,
                                                upstream_response_id_from_headers(upstream.headers));
        (void)revlm_commit_request(proxy);
        write_upstream(res, upstream.status_code, body, with_request_id(proxy, upstream.headers));
        return;
    }

    auto scan = std::make_shared<StreamUsageScan>();
    auto sse = std::make_shared<SseAccumulator>();
    const std::string fallback_model = model;

    revlm_plugin_common::StreamRelayOptions options;
    options.on_chunk = [scan, sse](std::string_view bytes, const GatewayStreamPump & /* pump */) {
        const std::vector<std::string> payloads = sse->feed(bytes);
        for (const std::string &payload : payloads) {
            (void)parse_stream_message(payload, *scan);
        }
    };
    options.after_pump = [scan, fallback_model](::httplib::DataSink & /* sink */, const GatewayStreamResult &result,
                                                ProxyRequest &proxy, int latency_ms) {
        const int status = proxy.upstream.status_code > 0 ? proxy.upstream.status_code : 200;
        revlm_plugin_common::fill_commit_fields(proxy, status, latency_ms, result.pump.first_token_latency_ms,
                                                proxy.upstream.response_id);
        proxy.upstream.model_name = !scan->usage.model.empty() ? scan->usage.model : fallback_model;
        proxy.token_details = build_token_details(scan->usage);
        proxy.protocol_cost_usd = scan->saw_usage ? compute_protocol_cost(scan->usage, proxy.upstream.model_name) : 0.0;
        (void)revlm_commit_request(proxy);
    };

    revlm_plugin_common::relay_upstream_stream(res, std::move(upstream), std::move(proxy), options);
}

} // namespace

// ---------------------------------------------------------------------------
// Interposable model catalog (Anthropic owns the "anthropic" type).
// ---------------------------------------------------------------------------

std::vector<Model> anthropic_catalog_models()
{
    std::vector<Model> out;
    out.reserve(anthropic_model_ids().size());
    for (const std::string &id : anthropic_model_ids()) {
        json pricing;
        const auto price = find_pricing(id);
        if (price.has_value()) {
            pricing["input"] = price->input_usd_per_1m;
            pricing["output"] = price->output_usd_per_1m;
            pricing["cache_read"] = price->cache_read_usd_per_1m;
            pricing["cache_creation_1h"] = price->cache_creation_1h_usd_per_1m;
            pricing["cache_creation_5m"] = price->cache_creation_5m_usd_per_1m;
        }
        out.emplace_back(static_cast<int>(out.size() + 1), id, std::move(pricing));
    }
    return out;
}

extern "C" void revlm_models_for_channel_type(std::string_view channel_type, std::vector<Model> &models)
{
    revlm_plugin_common::chain_models_for_channel_type("anthropic", channel_type, anthropic_catalog_models(), models);
}

extern "C" void revlm_all_models(std::vector<Model> &models)
{
    revlm_plugin_common::chain_all_models(anthropic_catalog_models(), models);
}

// The data-plane hook. Non-target ChannelGroup.types chain via RTLD_NEXT.
extern "C" void revlm_handle_v1(const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy)
{
    if (proxy.channel_group.type != "anthropic") {
        using Handler = void (*)(const ::httplib::Request &, ::httplib::Response &, ProxyRequest &);
        Handler next = reinterpret_cast<Handler>(::dlsym(RTLD_NEXT, "revlm_handle_v1"));
        if (next != nullptr && next != &revlm_handle_v1) {
            next(req, res, proxy);
        } else {
            write_upstream(res, 500,
                           serialize(json{ { "error", json{ { "message", "no matching protocol plugin" } } } }),
                           { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
        }
        return;
    }

    const std::string &path = proxy.http.path;
    if (proxy.http.method == "POST" && path == kMessagesPath) {
        const bool is_stream = parse_json_bool_field(proxy.http.body, "stream").value_or(false);
        if (is_stream) {
            handle_messages_stream(res, proxy);
        } else {
            handle_messages_non_stream(res, proxy);
        }
        return;
    }

    write_client_error(res, proxy, 404, "unknown anthropic endpoint");
}

} // namespace revlm_anthropic_plugin

// Upstream request preparation replacement (see plugin_common.hpp).
extern "C" void revlm_prepare_upstream(const revlm::Channel &channel, const revlm::UpstreamRequest &downstream,
                                       revlm::UpstreamPreparedRequest &prepared)
{
    revlm_plugin_common::prepare_upstream_request(channel, downstream, prepared);
}

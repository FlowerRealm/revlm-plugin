// Revlm v3 OpenAI channel plugin.
//
// Data-plane entry: extern "C" revlm_handle_v1. Owns the openai_compatible
// ChannelGroup.type: /v1/models, /v1/models/:id, /v1/chat/completions,
// /v1/responses and /v1/responses/input_tokens. Non-matching types chain to
// the next revlm_handle_v1 implementation via RTLD_NEXT.
//
// The plugin is an LD_PRELOAD module, so its definitions of the ordinary
// interposable symbols (revlm_handle_v1, revlm_prepare_upstream) win over the
// core's defaults. Billing: the plugin parses protocol usage, fills
// ProxyRequest::token_details with the canonical usage JSON and computes
// protocol_cost_usd from its own model pricing, then calls revlm_commit_request
// exactly once per client request.

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

namespace revlm_openai_plugin
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

// ---------------------------------------------------------------------------
// Model catalog + pricing (owned by this plugin).
// ---------------------------------------------------------------------------

struct Pricing {
    double input_usd_per_1m = 0.0;
    double output_usd_per_1m = 0.0;
    double cache_read_usd_per_1m = 0.0;
    double cache_write_usd_per_1m = 0.0;
};

const std::unordered_map<std::string, Pricing> &openai_pricing()
{
    static const std::unordered_map<std::string, Pricing> pricing = {
        { "gpt-5.5", { 5.0, 30.0, 0.5, 0.0 } },
        { "gpt-5.4", { 2.5, 15.0, 0.25, 0.0 } },
        { "gpt-5.4-mini", { 0.75, 4.5, 0.075, 0.0 } },
        { "gpt-5.3-codex", { 1.75, 14.0, 0.175, 0.0 } },
        { "codex-auto-review", { 2.5, 15.0, 0.25, 0.0 } },
    };
    return pricing;
}

const std::vector<std::string> &openai_model_ids()
{
    static const std::vector<std::string> ids = {
        "gpt-5.5", "gpt-5.4", "gpt-5.4-mini", "gpt-5.3-codex", "codex-auto-review",
    };
    return ids;
}

std::optional<Pricing> find_pricing(std::string_view model)
{
    const auto &table = openai_pricing();
    const auto it = table.find(std::string{ model });
    if (it == table.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string default_model()
{
    return openai_model_ids().front();
}

// ---------------------------------------------------------------------------
// Usage extraction.
// ---------------------------------------------------------------------------

struct UsageBreakdown {
    long long input_tokens = 0;       // total input (including cache)
    long long output_tokens = 0;
    long long cache_read_input_tokens = 0;
    long long cache_creation_5m_tokens = 0;
    long long cache_creation_1h_tokens = 0;
    std::string model;
    std::string service_tier;
};

long long json_ll(const json &value)
{
    return value.as_int64().value_or(0);
}

// Normalize a chat/completions usage object (prompt/completion_tokens).
UsageBreakdown parse_chat_usage(const json &usage)
{
    UsageBreakdown out;
    if (!usage.is_object()) {
        return out;
    }
    out.input_tokens = json_ll(usage["prompt_tokens"]);
    out.output_tokens = json_ll(usage["completion_tokens"]);
    const json details = usage["prompt_tokens_details"];
    if (details.is_object()) {
        out.cache_read_input_tokens = json_ll(details["cached_tokens"]);
        out.cache_creation_5m_tokens = json_ll(details["cache_write_tokens"]);
    }
    if (const auto tier = usage["service_tier"].as_string(); tier.has_value()) {
        out.service_tier = *tier;
    }
    return out;
}

// Normalize a Responses usage object (input/output_tokens + details).
UsageBreakdown parse_responses_usage(const json &usage)
{
    UsageBreakdown out;
    if (!usage.is_object()) {
        return out;
    }
    out.input_tokens = json_ll(usage["input_tokens"]);
    out.output_tokens = json_ll(usage["output_tokens"]);
    const json details = usage["input_tokens_details"];
    if (details.is_object()) {
        out.cache_read_input_tokens = json_ll(details["cached_tokens"]);
        out.cache_creation_5m_tokens = json_ll(details["cache_write_tokens"]);
    }
    if (const auto tier = usage["service_tier"].as_string(); tier.has_value()) {
        out.service_tier = *tier;
    }
    return out;
}

// Build the canonical token_details JSON that the core displays (usage_tokens /
// compute_pricing_breakdown read input_tokens, output_tokens,
// cache_read_input_tokens and cache_creation.ephemeral_*).
std::string build_token_details(const UsageBreakdown &usage)
{
    json details;
    json usage_json;
    usage_json["input_tokens"] = usage.input_tokens;
    usage_json["output_tokens"] = usage.output_tokens;
    usage_json["cache_read_input_tokens"] = usage.cache_read_input_tokens;
    json cache_creation;
    if (usage.cache_creation_5m_tokens > 0 || usage.cache_creation_1h_tokens > 0) {
        cache_creation["ephemeral_5m_input_tokens"] = usage.cache_creation_5m_tokens;
        cache_creation["ephemeral_1h_input_tokens"] = usage.cache_creation_1h_tokens;
    }
    if (cache_creation.is_object() && cache_creation.size() > 0) {
        usage_json["cache_creation"] = std::move(cache_creation);
    }
    if (!usage.service_tier.empty()) {
        usage_json["service_tier"] = usage.service_tier;
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
        usage.input_tokens - usage.cache_read_input_tokens - usage.cache_creation_5m_tokens -
        usage.cache_creation_1h_tokens;
    const double input_usd = std::max(0LL, billable_input) / 1.0e6 * pricing->input_usd_per_1m;
    const double output_usd = usage.output_tokens / 1.0e6 * pricing->output_usd_per_1m;
    const double cache_read_usd = usage.cache_read_input_tokens / 1.0e6 * pricing->cache_read_usd_per_1m;
    const double cache_write_usd =
        (usage.cache_creation_5m_tokens + usage.cache_creation_1h_tokens) / 1.0e6 * pricing->cache_write_usd_per_1m;
    return input_usd + output_usd + cache_read_usd + cache_write_usd;
}

// ---------------------------------------------------------------------------
// /v1/models responses.
// ---------------------------------------------------------------------------

void write_models_list(::httplib::Response &res, ProxyRequest &proxy)
{
    json data = json::array();
    for (const std::string &id : openai_model_ids()) {
        json item;
        item["id"] = id;
        item["object"] = "model";
        item["created"] = 1735689600;
        item["owned_by"] = "openai";
        data.push_back(std::move(item));
    }
    json body;
    body["object"] = "list";
    body["data"] = std::move(data);
    write_upstream(res, 200, serialize(body),
                   { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
}

void write_model_detail(::httplib::Response &res, ProxyRequest &proxy, std::string_view model_id)
{
    const auto &ids = openai_model_ids();
    const bool found = std::find(ids.begin(), ids.end(), model_id) != ids.end();
    if (!found) {
        write_upstream(res, 404,
                       serialize(json{ { "error", json{ { "message", "model not found" } } } }),
                       { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
        return;
    }
    json body;
    body["id"] = std::string{ model_id };
    body["object"] = "model";
    body["created"] = 1735689600;
    body["owned_by"] = "openai";
    write_upstream(res, 200, serialize(body),
                   { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
}

// ---------------------------------------------------------------------------
// Billing termination helpers. Every path calls revlm_commit_request exactly
// once. protocol_cost_usd is 0 for failures (no usage); status, latency and
// error info are preserved per ADR-0004.
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

std::optional<std::string> openai_auth_header(const ProxyRequest &proxy)
{
    const Channel *channel = revlm_plugin_common::selected_channel(proxy);
    if (channel == nullptr || channel->api_key.empty()) {
        return std::nullopt;
    }
    return "Bearer " + channel->api_key;
}

// ---------------------------------------------------------------------------
// POST /v1/chat/completions (non-streaming).
// ---------------------------------------------------------------------------

void handle_chat_non_stream(const ::httplib::Request & /* req */, ::httplib::Response &res, ProxyRequest &proxy)
{
    const auto auth = openai_auth_header(proxy);
    if (!auth.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(default_model());
    UpstreamRequest downstream =
        make_upstream_request(proxy, "/v1/chat/completions", { { "Authorization", *auth } }, /*stream=*/false);
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
    if (outcome == AttemptOutcome::Fatal) {
        commit_and_respond(res, proxy, { executed.response.status_code, executed.response.body,
                                         with_request_id(proxy, executed.response.headers) },
                           UsageBreakdown{}, model, latency_ms);
        return;
    }
    const auto parsed = json::parse(executed.response.body);
    UsageBreakdown usage;
    if (parsed.has_value() && parsed->is_object()) {
        usage = parse_chat_usage((*parsed)["usage"]);
        if (const auto m = (*parsed)["model"].as_string(); m.has_value() && !m->empty()) {
            usage.model = *m;
        }
    }
    if (executed.response.status_code >= 500) {
        // Last candidate produced a 5xx: report it, zero billing.
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
// POST /v1/responses (non-streaming).
// ---------------------------------------------------------------------------

void handle_responses_non_stream(const ::httplib::Request & /* req */, ::httplib::Response &res, ProxyRequest &proxy)
{
    const auto auth = openai_auth_header(proxy);
    if (!auth.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(default_model());
    UpstreamRequest downstream =
        make_upstream_request(proxy, "/v1/responses", { { "Authorization", *auth } }, /*stream=*/false);
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
            const json nested = (*parsed)["response"];
            const json usage_source = (nested.is_object() && nested["usage"].is_object()) ? nested["usage"] :
                                                                                            (*parsed)["usage"];
            usage = parse_responses_usage(usage_source);
            const json metadata = nested.is_object() ? nested : *parsed;
            if (const auto m = metadata["model"].as_string(); m.has_value() && !m->empty()) {
                usage.model = *m;
            }
            if (const auto tier = metadata["service_tier"].as_string(); tier.has_value()) {
                usage.service_tier = *tier;
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
// POST /v1/responses/input_tokens: a billing-only probe. The upstream returns
// input usage; the plugin commits that usage (protocol_cost_usd covers the
// input tokens) and relays the response.
// ---------------------------------------------------------------------------

void handle_input_tokens(const ::httplib::Request & /* req */, ::httplib::Response &res, ProxyRequest &proxy)
{
    const auto auth = openai_auth_header(proxy);
    if (!auth.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(default_model());
    UpstreamRequest downstream =
        make_upstream_request(proxy, "/v1/responses/input_tokens", { { "Authorization", *auth } }, /*stream=*/false);
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
            usage = parse_responses_usage((*parsed)["usage"]);
            if (const auto m = (*parsed)["model"].as_string(); m.has_value() && !m->empty()) {
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
// Streaming: /v1/chat/completions and /v1/responses with "stream": true.
// The relay runs pump_upstream_stream inside httplib's chunked content
// provider (after the hook returns); the plugin scans SSE data frames for the
// final usage and commits it in after_pump.
// ---------------------------------------------------------------------------

struct StreamUsageScan {
    UsageBreakdown usage;
    bool saw_usage = false;
    bool is_responses = false;
};

bool parse_stream_data(const std::string &payload, bool is_responses, StreamUsageScan &scan)
{
    const auto parsed = json::parse(payload);
    if (!parsed.has_value() || !parsed->is_object()) {
        return false;
    }
    if (is_responses) {
        const json response = (*parsed)["response"];
        const json usage_source = (response.is_object() && response["usage"].is_object()) ? response["usage"] :
                                                                                            (*parsed)["usage"];
        if (usage_source.is_object()) {
            UsageBreakdown parsed_usage = parse_responses_usage(usage_source);
            if (parsed_usage.input_tokens > 0 || parsed_usage.output_tokens > 0 || parsed_usage.cache_read_input_tokens > 0) {
                scan.usage = parsed_usage;
                scan.saw_usage = true;
            }
        }
        const json metadata = response.is_object() ? response : *parsed;
        if (const auto m = metadata["model"].as_string(); m.has_value() && !m->empty()) {
            scan.usage.model = *m;
        }
        if (const auto tier = metadata["service_tier"].as_string(); tier.has_value()) {
            scan.usage.service_tier = *tier;
        }
    } else {
        const json usage = (*parsed)["usage"];
        if (usage.is_object()) {
            UsageBreakdown parsed_usage = parse_chat_usage(usage);
            if (parsed_usage.input_tokens > 0 || parsed_usage.output_tokens > 0 || parsed_usage.cache_read_input_tokens > 0) {
                scan.usage = parsed_usage;
                scan.saw_usage = true;
            }
        }
        if (const auto m = (*parsed)["model"].as_string(); m.has_value() && !m->empty()) {
            scan.usage.model = *m;
        }
        if (const auto tier = (*parsed)["service_tier"].as_string(); tier.has_value()) {
            scan.usage.service_tier = *tier;
        }
    }
    return true;
}

void handle_stream(::httplib::Response &res, ProxyRequest &proxy,
                   std::string_view upstream_path, std::string_view model_default, bool is_responses)
{
    proxy.is_stream = true;
    const std::string model = parse_json_string_field(proxy.http.body, "model").value_or(std::string{ model_default });
    const auto auth = openai_auth_header(proxy);
    if (!auth.has_value()) {
        write_transport_error(res, proxy);
        return;
    }
    UpstreamRequest downstream =
        make_upstream_request(proxy, upstream_path, { { "Authorization", *auth } }, /*stream=*/true);
    UpstreamStreamResponse upstream;
    const AttemptOutcome outcome = revlm_plugin_common::open_stream(proxy, std::move(downstream), upstream);
    if (outcome == AttemptOutcome::Exhausted) {
        write_transport_error(res, proxy);
        return;
    }
    if (outcome == AttemptOutcome::Fatal) {
        std::string body = upstream.initial_body;
        body += read_remaining_stream(upstream.stream);
        if (upstream.stream.close) {
            upstream.stream.close();
        }
        proxy.error_class = "upstream";
        proxy.error_message = "upstream rejected request";
        revlm_plugin_common::fill_commit_fields(proxy, upstream.status_code, 0, 0,
                                                upstream_response_id_from_headers(upstream.headers));
        (void)revlm_commit_request(proxy);
        write_upstream(res, upstream.status_code, body, with_request_id(proxy, upstream.headers));
        return;
    }
    if (upstream.status_code >= 500) {
        // Last candidate produced a 5xx with a non-SSE body; drain and relay.
        std::string body = upstream.initial_body;
        body += read_remaining_stream(upstream.stream);
        if (upstream.stream.close) {
            upstream.stream.close();
        }
        proxy.error_class = "upstream";
        proxy.error_message = "upstream error";
        revlm_plugin_common::fill_commit_fields(proxy, upstream.status_code, 0, 0,
                                                upstream_response_id_from_headers(upstream.headers));
        (void)revlm_commit_request(proxy);
        write_upstream(res, upstream.status_code, body, with_request_id(proxy, upstream.headers));
        return;
    }

    auto scan = std::make_shared<StreamUsageScan>();
    scan->is_responses = is_responses;
    auto sse = std::make_shared<SseAccumulator>();
    const std::string fallback_model = model;

    revlm_plugin_common::StreamRelayOptions options;
    options.on_chunk = [scan, sse, is_responses](std::string_view bytes, const GatewayStreamPump & /* pump */) {
        const std::vector<std::string> payloads = sse->feed(bytes);
        for (const std::string &payload : payloads) {
            (void)parse_stream_data(payload, is_responses, *scan);
        }
    };
    options.after_pump = [scan, fallback_model](::httplib::DataSink & /* sink */, const GatewayStreamResult &result,
                                                ProxyRequest &proxy, int latency_ms) {
        const int status = proxy.upstream.status_code > 0 ? proxy.upstream.status_code : 200;
        revlm_plugin_common::fill_commit_fields(proxy, status, latency_ms, result.pump.first_token_latency_ms,
                                                proxy.upstream.response_id);
        if (!scan->usage.model.empty()) {
            proxy.upstream.model_name = scan->usage.model;
        } else {
            proxy.upstream.model_name = fallback_model;
        }
        proxy.token_details = build_token_details(scan->usage);
        proxy.protocol_cost_usd = scan->saw_usage ? compute_protocol_cost(scan->usage, proxy.upstream.model_name) : 0.0;
        (void)revlm_commit_request(proxy);
    };

    revlm_plugin_common::relay_upstream_stream(res, std::move(upstream), std::move(proxy), options);
}

// ---------------------------------------------------------------------------
// /v1/models/:id path parameter. The v1 hook receives the raw request (the
// core registers /v1/.* as a regex route, which does not populate
// path_params), so the id is extracted from the path.
// ---------------------------------------------------------------------------

std::string path_model_id(const ProxyRequest &proxy)
{
    constexpr std::string_view prefix = "/v1/models/";
    if (proxy.http.path.rfind(prefix, 0) != 0) {
        return {};
    }
    return proxy.http.path.substr(prefix.size());
}

} // namespace

// ---------------------------------------------------------------------------
// Interposable model catalog. The core's /api/user/models/detail and
// /api/admin/dashboard call revlm_all_models; per-ChannelGroup.type lookups
// go through revlm_models_for_channel_type. This plugin owns the
// openai_compatible catalog and chains to the next module for other types.
// ---------------------------------------------------------------------------

std::vector<Model> openai_catalog_models()
{
    std::vector<Model> out;
    out.reserve(openai_model_ids().size());
    for (const std::string &id : openai_model_ids()) {
        json pricing;
        const auto price = find_pricing(id);
        if (price.has_value()) {
            pricing["input"] = price->input_usd_per_1m;
            pricing["output"] = price->output_usd_per_1m;
            pricing["cache_read"] = price->cache_read_usd_per_1m;
            pricing["cache_write"] = price->cache_write_usd_per_1m;
        }
        out.emplace_back(static_cast<int>(out.size() + 1), id, std::move(pricing));
    }
    return out;
}

extern "C" void revlm_models_for_channel_type(std::string_view channel_type, std::vector<Model> &models)
{
    revlm_plugin_common::chain_models_for_channel_type("openai_compatible", channel_type, openai_catalog_models(),
                                                       models);
}

extern "C" void revlm_all_models(std::vector<Model> &models)
{
    revlm_plugin_common::chain_all_models(openai_catalog_models(), models);
}

// The data-plane hook. Non-target ChannelGroup.types chain via RTLD_NEXT.
extern "C" void revlm_handle_v1(const ::httplib::Request &req, ::httplib::Response &res, ProxyRequest &proxy)
{
    if (proxy.channel_group.type != "openai_compatible") {
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
    const bool is_stream = parse_json_bool_field(proxy.http.body, "stream").value_or(false);

    if (proxy.http.method == "GET" && path == "/v1/models") {
        write_models_list(res, proxy);
        return;
    }
    if (proxy.http.method == "GET" && path.rfind("/v1/models/", 0) == 0) {
        write_model_detail(res, proxy, path_model_id(proxy));
        return;
    }
    if (proxy.http.method == "POST" && path == "/v1/chat/completions") {
        if (is_stream) {
            handle_stream(res, proxy, "/v1/chat/completions", default_model(), /*is_responses=*/false);
        } else {
            handle_chat_non_stream(req, res, proxy);
        }
        return;
    }
    if (proxy.http.method == "POST" && path == "/v1/responses") {
        if (is_stream) {
            handle_stream(res, proxy, "/v1/responses", default_model(), /*is_responses=*/true);
        } else {
            handle_responses_non_stream(req, res, proxy);
        }
        return;
    }
    if (proxy.http.method == "POST" && path == "/v1/responses/input_tokens") {
        handle_input_tokens(req, res, proxy);
        return;
    }

    write_client_error(res, proxy, 404, "unknown openai endpoint");
}

} // namespace revlm_openai_plugin

// Upstream request preparation replacement (see plugin_common.hpp). The core's
// UpstreamExecutor::prepare calls this interposable symbol and throws if no
// module provides it. Both channel plugins export the same implementation; the
// one that wins preload order delegates to the shared helper, which preserves
// whatever auth header the owning data-plane hook already stamped.
extern "C" void revlm_prepare_upstream(const revlm::Channel &channel, const revlm::UpstreamRequest &downstream,
                                       revlm::UpstreamPreparedRequest &prepared)
{
    revlm_plugin_common::prepare_upstream_request(channel, downstream, prepared);
}

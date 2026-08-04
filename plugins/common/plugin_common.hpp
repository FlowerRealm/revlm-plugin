#pragma once

// Shared v3 plugin helpers for the OpenAI / Anthropic channel plugins.
//
// These plugins are LD_PRELOAD / DYLD_INSERT_LIBRARIES modules: each provides
// the extern "C" data-plane hook `revlm_handle_v1`, plus the interposable
// model-catalog and upstream-prepare replacements. The helpers here are the
// shared mechanics (channel selection, upstream execution with round-robin
// failover, SSE relay, billing commit). Protocol-specific usage parsing and
// pricing live in each plugin's own TU.

#include <chrono>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httplib.h>

#include <proxy/gateway.hpp>
#include <proxy/upstream.hpp>
#include <request/proxy_request.hpp>
#include <config/config.hpp>
#include <models/catalog.hpp>
#include <util/json.hpp>
#include <util/strings.hpp>

#include "protocol_helpers.hpp"
#include "plugin_stream.hpp"

namespace revlm_plugin_common
{

using namespace revlm;

// ---------------------------------------------------------------------------
// RTLD_NEXT chaining for the shared interposable symbols.
// ---------------------------------------------------------------------------

template <typename Fn>
Fn next_symbol(const char *name)
{
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

// ---------------------------------------------------------------------------
// Channel selection from the ChannelGroup snapshot.
// ---------------------------------------------------------------------------

inline const Channel *selected_channel(const ProxyRequest &proxy)
{
    if (proxy.channel_group.channels.empty()) {
        return nullptr;
    }
    if (proxy.upstream.channel_id > 0) {
        for (const Channel &candidate : proxy.channel_group.channels) {
            if (candidate.id == proxy.upstream.channel_id) {
                return &candidate;
            }
        }
    }
    for (const Channel &candidate : proxy.channel_group.channels) {
        if (candidate.status) {
            return &candidate;
        }
    }
    return &proxy.channel_group.channels.front();
}

inline std::size_t active_channel_count(const ProxyRequest &proxy)
{
    std::size_t count = 0;
    for (const Channel &candidate : proxy.channel_group.channels) {
        if (candidate.status) {
            ++count;
        }
    }
    return count > 0 ? count : 1;
}

// ---------------------------------------------------------------------------
// Upstream request construction. The client body (already authenticated and
// normalized by the core) is forwarded verbatim; protocol auth headers are
// stamped here by the plugin that owns the ChannelGroup.type, and the shared
// revlm_prepare_upstream replacement below leaves them untouched.
// ---------------------------------------------------------------------------

inline UpstreamRequest make_upstream_request(const ProxyRequest &proxy, std::string_view path,
                                             std::vector<UpstreamHeader> extra_headers, bool stream)
{
    UpstreamRequest downstream = build_proxy_upstream_request(proxy, path);
    set_header(downstream.headers, "Content-Type", "application/json");
    set_header(downstream.headers, "Accept", stream ? "text/event-stream" : "application/json");
    for (const UpstreamHeader &header : extra_headers) {
        set_header(downstream.headers, header.name, header.value);
    }
    return downstream;
}

// ---------------------------------------------------------------------------
// Shared upstream-prepare replacement. The core's UpstreamExecutor::prepare
// requires an interposable revlm_prepare_upstream to exist (the core default
// throws). The data-plane hook already stamped the protocol-specific auth
// header onto the UpstreamRequest, so this replacement only ensures some auth
// is present; it never overrides what the owning plugin set.
// ---------------------------------------------------------------------------

inline void prepare_upstream_request(const Channel &channel, const UpstreamRequest & /* downstream */,
                                     UpstreamPreparedRequest &prepared)
{
    if (channel.api_key.empty()) {
        return;
    }
    if (header_value(prepared.headers, "Authorization").empty() &&
        header_value(prepared.headers, "X-Api-Key").empty()) {
        set_header(prepared.headers, "Authorization", "Bearer " + channel.api_key);
    }
}

// ---------------------------------------------------------------------------
// Non-stream upstream execution with round-robin failover (ADR-0003).
//
// Returns true when an upstream result is available (success or a
// non-retryable 4xx). On transport errors and 5xx it advances to the next
// candidate; after every active channel has been tried once it returns false.
// ---------------------------------------------------------------------------

enum class AttemptOutcome {
    Succeeded, // 2xx
    Fatal,     // non-retryable 4xx: report to the client, do not switch
    Exhausted, // every candidate produced a transport error / 5xx
};

inline AttemptOutcome execute_non_stream(ProxyRequest &proxy, UpstreamRequest downstream,
                                         UpstreamExecutionResult &result)
{
    const std::size_t max_attempts = active_channel_count(proxy);
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        const long long channel_id = proxy.upstream.channel_id;
        if (channel_id <= 0) {
            return AttemptOutcome::Exhausted;
        }
        ScheduledUpstreamExecution scheduled = execute_scheduled_upstream(channel_id, downstream);
        if (scheduled.transport_error.has_value()) {
            if (revlm_next_candidate(proxy) == nullptr) {
                return AttemptOutcome::Exhausted;
            }
            continue;
        }
        result = std::move(*scheduled.result);
        const int status = result.response.status_code;
        if (status >= 400 && status < 500) {
            return AttemptOutcome::Fatal;
        }
        if (status >= 500) {
            if (revlm_next_candidate(proxy) == nullptr) {
                return AttemptOutcome::Succeeded; // last channel; report its body
            }
            continue;
        }
        return AttemptOutcome::Succeeded;
    }
    return AttemptOutcome::Exhausted;
}

inline AttemptOutcome open_stream(ProxyRequest &proxy, UpstreamRequest downstream,
                                  UpstreamStreamResponse &result)
{
    const std::size_t max_attempts = active_channel_count(proxy);
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        const long long channel_id = proxy.upstream.channel_id;
        if (channel_id <= 0) {
            return AttemptOutcome::Exhausted;
        }
        ScheduledUpstreamStreamExecution scheduled = open_scheduled_upstream_stream(channel_id, downstream);
        if (scheduled.transport_error.has_value()) {
            if (revlm_next_candidate(proxy) == nullptr) {
                return AttemptOutcome::Exhausted;
            }
            continue;
        }
        result = std::move(*scheduled.result);
        const int status = result.status_code;
        if (status >= 400 && status < 500) {
            return AttemptOutcome::Fatal;
        }
        if (status >= 500) {
            if (result.stream.close) {
                result.stream.close();
            }
            if (revlm_next_candidate(proxy) == nullptr) {
                return AttemptOutcome::Succeeded; // last channel; report its body
            }
            continue;
        }
        return AttemptOutcome::Succeeded;
    }
    return AttemptOutcome::Exhausted;
}

// ---------------------------------------------------------------------------
// SSE relay to the client. Runs inside the chunked content provider (which the
// httplib server invokes on the handler thread after the hook returns), so all
// state the plugin needs after the stream must live in the shared object. The
// caller's ProxyRequest is copied into it for the final revlm_commit_request.
// ---------------------------------------------------------------------------

inline void fill_commit_fields(ProxyRequest &proxy, int status_code, int latency_ms, int first_token_latency_ms,
                               std::string_view response_id)
{
    proxy.upstream.status_code = status_code;
    proxy.upstream.latency_ms = latency_ms;
    proxy.upstream.first_token_latency_ms = std::min(std::max(first_token_latency_ms, 0), std::max(latency_ms, 0));
    if (!response_id.empty()) {
        proxy.upstream.response_id = std::string{ response_id };
    }
}

inline void write_transport_error(::httplib::Response &res, ProxyRequest &proxy)
{
    proxy.error_class = "upstream";
    proxy.error_message = "upstream connect failed";
    write_upstream(res, 502, serialize(json{ { "error", json{ { "message", "upstream connect failed" } } } }),
                   { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
}

inline void write_client_error(::httplib::Response &res, ProxyRequest &proxy, int status, std::string_view message)
{
    proxy.error_class = "protocol";
    proxy.error_message = std::string{ message };
    write_upstream(res, status, serialize(json{ { "error", json{ { "message", std::string{ message } } } } }),
                   { { "Content-Type", "application/json; charset=utf-8" }, { "X-Request-Id", proxy.request_id } });
}

// ---------------------------------------------------------------------------
// SSE relay. `on_chunk` scans raw bytes as the pump relays them (the plugin
// accumulates protocol usage from data frames); `after_pump` runs once the
// stream is fully drained and is responsible for filling the copied
// ProxyRequest's token_details/protocol_cost_usd and calling
// revlm_commit_request. The relay itself never touches billing.
// ---------------------------------------------------------------------------

struct StreamRelayOptions {
    std::function<void(std::string_view bytes, const GatewayStreamPump &pump)> on_chunk;
    std::function<void(::httplib::DataSink &sink, const GatewayStreamResult &result, ProxyRequest &proxy,
                       int latency_ms)>
        after_pump;
    int idle_timeout_ms = 0;
};

inline void relay_upstream_stream(::httplib::Response &res, UpstreamStreamResponse upstream, ProxyRequest proxy,
                                  const StreamRelayOptions &options)
{
    struct Shared {
        UpstreamStreamResponse upstream;
        ProxyRequest proxy;
        int idle_timeout_ms = 0;
        StreamRelayOptions options;
    };
    auto shared = std::make_shared<Shared>();
    shared->upstream = std::move(upstream);
    shared->proxy = std::move(proxy);
    shared->idle_timeout_ms =
        options.idle_timeout_ms > 0 ? options.idle_timeout_ms : std::max(1000, config().proxy_upstream_timeout_seconds * 1000);
    shared->options = options;

    res.status = shared->upstream.status_code;
    res.reason = (shared->upstream.status_code >= 200 && shared->upstream.status_code < 300) ? "OK" : "Upstream";
    std::string content_type = "text/event-stream; charset=utf-8";
    for (const UpstreamHeader &header : shared->upstream.headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            content_type = header.value;
            continue;
        }
        res.set_header(header.name, header.value);
    }
    const std::string response_id = upstream_response_id_from_headers(shared->upstream.headers);
    if (!response_id.empty()) {
        res.set_header("X-Response-Id", response_id);
        // Propagate to the copied ProxyRequest so after_pump commits it.
        shared->proxy.upstream.response_id = response_id;
    }
    // Propagate the upstream status so after_pump records the real status.
    shared->proxy.upstream.status_code = shared->upstream.status_code;

    res.set_chunked_content_provider(content_type, [shared](size_t offset, ::httplib::DataSink &sink) {
        if (offset != 0) {
            return false;
        }
        const auto started_at = std::chrono::steady_clock::now();
        auto write = [&sink](std::string_view data) { return sink.write(data.data(), data.size()); };
        const GatewayStreamResult result = pump_upstream_stream(shared->upstream.stream.read, write,
                                                                shared->upstream.initial_body, shared->idle_timeout_ms,
                                                                shared->upstream.stream.poll_fd, shared->options.on_chunk);
        if (shared->upstream.stream.close) {
            shared->upstream.stream.close();
        }
        const int latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                .count());
        if (shared->options.after_pump) {
            shared->options.after_pump(sink, result, shared->proxy, latency_ms);
        }
        sink.done();
        return true;
    });
}

// ---------------------------------------------------------------------------
// Model-catalog interposition chaining. Each channel plugin owns the models
// for its ChannelGroup.type and appends its own list to the RTLD_NEXT chain.
// ---------------------------------------------------------------------------

inline void chain_models_for_channel_type(std::string_view my_type, std::string_view channel_type,
                                          const std::vector<Model> &my_models, std::vector<Model> &out)
{
    if (channel_type == my_type) {
        out.insert(out.end(), my_models.begin(), my_models.end());
        return;
    }
    auto next = next_symbol<void (*)(std::string_view, std::vector<Model> &)>("revlm_models_for_channel_type");
    if (next != nullptr) {
        next(channel_type, out);
    }
}

inline void chain_all_models(const std::vector<Model> &my_models, std::vector<Model> &out)
{
    out.insert(out.end(), my_models.begin(), my_models.end());
    auto next = next_symbol<void (*)(std::vector<Model> &)>("revlm_all_models");
    if (next != nullptr) {
        next(out);
    }
}

} // namespace revlm_plugin_common

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <proxy/gateway.hpp>

namespace revlm_plugin_common
{

// SSE event framing. The upstream stream is delivered as arbitrary byte
// chunks; the plugin scans them for complete "data:" lines (OpenAI /
// Responses / Anthropic all use SSE frames whose payload is a single JSON
// object per "data:" line). Parsing is incremental across chunk boundaries.
class SseAccumulator {
public:
    // Feed a stream chunk; returns the complete JSON payloads observed.
    // Each returned string is the raw content of one "data:" line.
    std::vector<std::string> feed(std::string_view chunk);

    // Flush any trailing payload (after the stream ends). Returns complete
    // payloads that were missing a final newline.
    std::vector<std::string> finish();

private:
    std::string buffer_;
    std::vector<std::string> extract_complete();
};

inline std::vector<std::string> SseAccumulator::extract_complete()
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t newline = buffer_.find('\n', pos);
        if (newline == std::string::npos) {
            break;
        }
        const std::string_view line{ buffer_.data() + pos, newline - pos };
        // Strip a trailing '\r' from CRLF framing.
        const std::size_t end = !line.empty() && line.back() == '\r' ? line.size() - 1 : line.size();
        const std::string_view content = line.substr(0, end);
        if (content.size() > 5 && content.substr(0, 5) == "data:") {
            std::string payload{ content.substr(5) };
            if (!payload.empty() && payload.front() == ' ') {
                payload.erase(payload.begin());
            }
            if (!payload.empty() && payload != "[DONE]") {
                out.push_back(std::move(payload));
            }
        }
        pos = newline + 1;
    }
    buffer_.erase(0, pos);
    return out;
}

inline std::vector<std::string> SseAccumulator::feed(std::string_view chunk)
{
    buffer_.append(chunk.data(), chunk.size());
    return extract_complete();
}

inline std::vector<std::string> SseAccumulator::finish()
{
    if (buffer_.empty()) {
        return {};
    }
    const std::string tail = std::move(buffer_);
    buffer_.clear();
    std::vector<std::string> out;
    if (tail.size() > 5 && tail.substr(0, 5) == "data:") {
        std::string payload{ tail.substr(5) };
        if (!payload.empty() && payload.front() == ' ') {
            payload.erase(payload.begin());
        }
        if (!payload.empty() && payload != "[DONE]") {
            out.push_back(std::move(payload));
        }
    }
    return out;
}

} // namespace revlm_plugin_common

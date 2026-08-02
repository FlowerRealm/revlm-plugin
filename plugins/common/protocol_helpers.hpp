#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <proxy/upstream.hpp>

namespace revlm_plugin_common
{

inline bool iequals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

inline void erase_header(std::vector<revlm::UpstreamHeader> &headers, std::string_view name)
{
    headers.erase(std::remove_if(headers.begin(), headers.end(), [&](const revlm::UpstreamHeader &header) {
                      return iequals(header.name, name);
                  }),
                  headers.end());
}

inline std::string header_value(const std::vector<revlm::UpstreamHeader> &headers, std::string_view name)
{
    for (const revlm::UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            return header.value;
        }
    }
    return {};
}

inline void set_header(std::vector<revlm::UpstreamHeader> &headers, std::string_view name, std::string value)
{
    for (revlm::UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            header.name = std::string{ name };
            header.value = std::move(value);
            return;
        }
    }
    headers.push_back({ std::string{ name }, std::move(value) });
}

inline void prepare_common_headers(revlm::UpstreamPreparedRequest &prepared)
{
    erase_header(prepared.headers, "Authorization");
    erase_header(prepared.headers, "X-Api-Key");
    erase_header(prepared.headers, "Accept-Encoding");
    set_header(prepared.headers, "Accept-Encoding", "identity");
}

} // namespace revlm_plugin_common

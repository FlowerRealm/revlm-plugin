#pragma once

#include <bit>
#include <dlfcn.h>

namespace revlm_plugin_common
{

// This is ELF's normal symbol chain, not a core-owned registry. A module that
// replaces a composable core function calls the definition after itself; a
// module that wants total replacement simply does not call this helper.
template <typename Function> Function next_symbol(const char *name)
{
    static_assert(sizeof(Function) == sizeof(void *));
    return std::bit_cast<Function>(::dlsym(RTLD_NEXT, name));
}

} // namespace revlm_plugin_common

# revlm-plugin

The official preload plugins for Revlm:

- `OpenAI`: OpenAI-compatible models, Chat Completions, and Responses.
- `Anthropic`: Messages.

This is deliberately not an SDK extension system. A trusted plugin is a normal
Linux shared library compiled against the full Revlm C++ ABI. At worker start,
Revlm puts enabled libraries in `LD_PRELOAD`; a plugin can provide the same C++
symbol as the core and replace it. That includes a small protocol helper, the
whole upstream executor, or `revlm_register_http_routes` itself. The official
modules each own their own model catalog, route(s), protocol conversion, and
stream/usage behavior; the core has no OpenAI or Anthropic branch.

Build against the exact Revlm build you intend to run:

```bash
cmake -S /path/to/revlm -B /tmp/revlm-build -DREVLM_BUILD_TESTS=OFF
cmake --build /tmp/revlm-build --target revlm revlm_worker
cmake --install /tmp/revlm-build --prefix /tmp/revlm-core
cmake -S . -B build -DCMAKE_PREFIX_PATH=/tmp/revlm-core
cmake --build build
```

Build one module on Linux `amd64` and one on Linux `arm64`, then package each
plugin with `packaging/build-package.py`. The upload artifact requires both
architectures. The multi-architecture Revlm image instead creates one
image-local package for its own architecture under `REVLM_SYSTEM_PLUGIN_DIR`.

See [the preload format](docs/preload-format.md) for the package, ABI, load
order, frontend, and lifecycle contract.

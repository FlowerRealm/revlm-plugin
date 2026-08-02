# revlm-plugin

The official V1 data-plane plugins for Revlm:

- `OpenAI`: `GET /v1/models`, `GET /v1/models/:model_id`,
  `POST /v1/chat/completions`, `POST /v1/responses`, and
  `POST /v1/responses/input_tokens`.
- `Anthropic`: `POST /v1/messages`.

Build against a matching Revlm SDK install:

```bash
cmake -S /path/to/revlm -B /tmp/revlm-build -DREVLM_BUILD_TESTS=OFF
cmake --build /tmp/revlm-build --target revlm
cmake --install /tmp/revlm-build --prefix /tmp/revlm-sdk
cmake -S . -B build -DCMAKE_PREFIX_PATH=/tmp/revlm-sdk
cmake --build build
```

Build one module on Linux `amd64` and one on Linux `arm64`, then package each
plugin with `packaging/build-package.py`. The script intentionally requires
both artifacts, because production packages target both supported Revlm Linux
architectures.

Revlm's multi-architecture container release also builds one image-local
system package per image. It uses `--system-target linux-amd64|linux-arm64`
with the native module; that package deliberately contains only the image's
own architecture and is never used as an upload artifact. The release Docker
build expands it with `packaging/install-system-package.py` into the host's
read-only `REVLM_SYSTEM_PLUGIN_DIR` layout.

See [the V1 format](docs/format-v1.md) for the ABI and lifecycle contract.

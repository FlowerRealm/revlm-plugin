# revlm-plugin

The official channel data-plane plugins for Revlm (plugins v3 architecture):

- `OpenAI`: `ChannelGroup.type == "openai_compatible"`, handles
  `GET /v1/models`, `GET /v1/models/:id`, `POST /v1/chat/completions`,
  `POST /v1/responses`, and `POST /v1/responses/input_tokens`.
- `Anthropic`: `ChannelGroup.type == "anthropic"`, handles `POST /v1/messages`.

Each plugin is an `LD_PRELOAD` module that provides `extern "C" revlm_handle_v1`
(the single /v1 data-plane hook), plus the interposable upstream-prepare and
model-catalog symbols. Non-matching `ChannelGroup.type` chains via
`dlsym(RTLD_NEXT)`.

Build the modules against the host core headers (a matching Revlm checkout or
an installed RevlmCore package), then package each plugin with
`packaging/build-package.py`. The script intentionally requires both artifacts,
because production packages must contain `backend/amd` and `backend/arm`.

Revlm's multi-architecture container release also builds one image-local
system package per image. It uses `--system-target linux-amd64|linux-arm64`
with the native module; that package deliberately contains only the image's
own architecture and is never used as an upload artifact. The release Docker
build expands it with `packaging/install-system-package.py` into the host's
read-only `REVLM_SYSTEM_PLUGIN_DIR` layout.

See [the package format](docs/format-v1.md) for the ABI and lifecycle contract.

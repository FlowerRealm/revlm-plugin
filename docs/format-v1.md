# Revlm 数据面插件格式

本仓库的 `OpenAI` 与 `Anthropic` 是 Revlm 官方 channel 数据面插件。它们按 Revlm
插件 v3 架构（即当前 `.revlm-plugin` 包契约 v1）实现：不是注册路由的窄接口，而是
`LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` 模块，通过覆盖核心公开的普通 C 符号进入
一次 `/v1` 数据面请求的完整协议生命周期。

宿主侧权威文档是 Revlm 仓库的 `docs/plugin-package-format.md` 与 `CONTEXT.md`；
本文件只说明本仓库插件按什么契约构建。

## 包内容

上传文件扩展名是 `.revlm-plugin`，内容是 ZIP。生产包必须同时包含 `backend/amd/`
与 `backend/arm/` 两个平台目录，每目录恰好一个可加载 `.so`（文件名任意）：

```text
plugin.json
backend/amd/libOpenAI.so
backend/arm/libOpenAI.so
frontend/entry.js        # 必须提供，可为空/no-op ESM
```

路径必须是相对 POSIX 路径，不含 `..`、绝对路径、符号链接、加密项或 ZIP64。
平台名固定为 `amd`/`arm`（不是 `amd64`/`arm64`）。安装包先写入 staging 再原子发布到
`REVLM_PLUGIN_DIR/packages/<id>/`（单目录覆盖，无版本目录层级）。

## manifest

`plugin.json` 只有五个字段，无 `format_version`、`core_abi`、`requires`、`targets`、
`load_order`、`migrations`：

```json
{
  "id": "OpenAI",
  "type": "channel",
  "name": "OpenAI",
  "description": "OpenAI 协议插件（v1/chat/completions、v1/responses、v1/models）",
  "version": "0.2.0"
}
```

- `id`、`version` 只能使用字母、数字、`-`、`_`、`.`。
- `type` 是插件类别，`channel` 表示参与渠道协议数据面；`plugin.type` 与
  `ChannelGroup.type` 是两个概念。
- 后端入口 `backend/<amd|arm>/` 与前端入口 `frontend/entry.js` 由目录约定声明，
  不进入清单；两者都必须提供。

## ABI

v1 不做 ABI 兼容性校验：清单不含 `core_abi`，插件与当前 Revlm/httplib ABI 一起
冷启动升级。`type` 只表示插件类别，不决定协议映射。

数据面入口是核心公开的可插桩符号，插件提供同名 `extern "C"` 定义并通过
`LD_PRELOAD` 覆盖：

```cpp
extern "C" void revlm_handle_v1(const httplib::Request &, httplib::Response &, ProxyRequest &);
```

插件检查 `ProxyRequest.channel_group.type` 是否属于自己：
- `OpenAI` 处理 `openai_compatible`，分支 `/v1/models`、`/v1/models/:id`、
  `POST /v1/chat/completions`、`POST /v1/responses`、`POST /v1/responses/input_tokens`。
- `Anthropic` 处理 `anthropic`，分支 `POST /v1/messages`。
- 其它类型沿 `dlsym(RTLD_NEXT, "revlm_handle_v1")` 链继续；未命中任何协议插件时
  核心默认实现返回 500，不扣费。

目标插件在一次 hook 调用内完成协议分支、上游请求构造、响应/SSE 解析、原始 usage
提取与协议计费结果。核心提供鉴权后的 ChannelGroup 快照、通用上游传输、候选 Channel
迭代与最终提交：

```cpp
extern "C" const Channel *revlm_next_candidate(ProxyRequest &); // 每次上游失败后取下一个
extern "C" bool revlm_commit_request(ProxyRequest &);           // 最终提交：应用倍率、扣款、写核心请求记录
```

插件还覆盖 `revlm_prepare_upstream`（核心的 `UpstreamExecutor::prepare` 依赖它，
否则抛异常）与模型目录符号 `revlm_models_for_channel_type` / `revlm_all_models`。
模型目录由插件按 `ChannelGroup.type` 提供；`Model` 只有 `id/name/pricing(json)`，
价格结构插件自定义。

## 计费

插件理解协议 usage 字段并得到运行时基础金额 `protocol_cost_usd`，把完整原始
usage JSON 写入 `ProxyRequest.token_details`，然后调用 `revlm_commit_request`；
核心应用 `ChannelGroup.price_multiplier`、扣款并持久化 `usd` 与倍率快照。核心不解析
协议 token JSON。流式响应在 SSE 泵内扫描最终 usage 事件（必要时合并），普通响应直接
解析响应体。一次客户端请求只提交一次；中间 Channel 尝试不落库也不独立计费。

## 前端

`frontend/entry.js` 是普通 ESM，可为空。核心前端只动态 `import` 该模块，入口依靠
模块副作用自行挂载。这两个插件是纯后端，entry.js 是 no-op。

## 启动、停用与卸载

已启用插件按 `id` 字典序进入 `LD_PRELOAD`。可选的 `revlm_plugin_migrate()` /
`revlm_plugin_cleanup()` 是 `extern "C" void` 生命周期符号，缺失按 no-op；本仓库
插件没有专属数据库表，不提供这两个符号。停用、卸载、失败恢复都按宿主包格式文档的
文件系统标记语义执行。

## 构建与打包

在 Linux `amd64` 和 Linux `arm64` 各构建一次模块，然后合并为生产包：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/RevlmCore
cmake --build build

python3 packaging/build-package.py OpenAI \
  --linux-amd64 /artifacts/amd64/libOpenAI.so \
  --linux-arm64 /artifacts/arm64/libOpenAI.so \
  --output dist/OpenAI.revlm-plugin
```

Revlm 的测试构建会把本子模块 `add_subdirectory` 进测试目标，直接链接宿主刚构建的
`revlm_core` 并 `dlopen` 这些模块，而不是维护核心内的协议回退实现。macOS 的
Mach-O 两级绑定不提供符号替换，本地 macOS 构建只能验证编译，不能证明 Linux 替换链路。

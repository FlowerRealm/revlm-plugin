# Revlm 数据面插件格式 V1

V1 是一个窄接口：预编译 C++ 模块注册精确的数据面路由和渠道类型。它不是二进制 hook，也不允许插件覆盖核心中未声明的函数。核心保留 listener、请求限制、请求 ID、token 鉴权、余额检查、上游调度与传输、SSE 工具、用量记录和计费。

安装插件即表示 root 无条件信任其本机代码。V1 没有签名、沙箱、权限隔离、热加载或热卸载；所有安装、启停和卸载都要重启 Revlm 才生效。

## 上传包

上传文件扩展名是 `.revlm-plugin`，内容是 ZIP。生产上传包必须同时包含 Linux `amd64` 和 `arm64` 模块：

```text
plugin.json
backend/linux-amd64/libOpenAI.so
backend/linux-arm64/libOpenAI.so
frontend/channel-types.json
migrations/0001_initial.sql       # 可选
```

路径必须是相对 POSIX 路径，不能包含 `..`、绝对路径、符号链接、加密项或 ZIP64。主机在解压前校验归档大小、CRC、manifest 和当前平台模块；上传目录先写到 staging，再原子移动到 `REVLM_PLUGIN_DIR/packages/<id>/<version>/`。

镜像内系统插件使用同一目录格式，但每个多架构镜像只放入自己的单一目标模块。它们由 Revlm Docker 发行流程生成，不是上传包。

## manifest

`plugin.json` 的 V1 必填字段：

```json
{
  "format_version": 1,
  "id": "OpenAI",
  "name": "OpenAI",
  "version": "0.1.0",
  "sdk_abi": "revlm-plugin-cpp-v1",
  "requires": [],
  "targets": [
    { "os": "linux", "arch": "amd64", "module": "backend/linux-amd64/libOpenAI.so" },
    { "os": "linux", "arch": "arm64", "module": "backend/linux-arm64/libOpenAI.so" }
  ],
  "frontend_schema": "frontend/channel-types.json",
  "migrations": []
}
```

- `id`、`version` 只能使用字母、数字、`-`、`_`、`.`，且每个目标平台只能有一个模块。
- `sdk_abi` 必须精确等于 `revlm-plugin-cpp-v1`；不匹配的包不会加载。
- `requires` 是插件 ID 列表。缺失依赖或循环依赖会将该插件标为 `failed`，不影响管理面或无关插件。
- 首批兼容插件的固定 ID/显示名是 `OpenAI`、`Anthropic`；渠道数据库类型仍分别是 `openai_compatible`、`anthropic`，所以现有渠道不需要数据迁移。
- `migrations` 是只进不退的 SQL 文件。模块可通过 `register_migrations` 声明同一列表；主机拒绝两者不一致的包。

## 原生 ABI

插件只通过两个 C 符号被发现，随后使用版本锁定的 C++ 对象：

```cpp
extern "C" revlm::plugin::v1::Plugin* revlm_plugin_create_v1();
extern "C" void revlm_plugin_destroy_v1(revlm::plugin::v1::Plugin*);
```

模块与 Revlm 必须链接同一个已发布 `RevlmPluginSDK` 主机库。不要包含未公开的核心头文件，也不要依赖主程序的私有符号。

`Plugin::register_with(PluginRegistrar&)` 只能调用：

```cpp
registrar.register_data_plane_route({"POST", "/v1/example", true, true}, handler);
registrar.register_channel_type(descriptor);
registrar.register_migrations(migrations);
```

路由键是标准化的 `HTTP method + path template`。V1 支持 `GET` 和 `POST`；重复路由、重复渠道类型、空 handler 或非法 method 都会使加载失败。主机以 `dlopen(..., RTLD_NOW | RTLD_LOCAL)` 加载模块，加载完成后冻结路由表。

处理器签名为：

```cpp
revlm::plugin::v1::DataPlaneResult handle(
    revlm::plugin::v1::AuthenticatedRequest& request,
    revlm::plugin::v1::ResponseWriter& response,
    revlm::plugin::v1::HostServices& host);
```

`AuthenticatedRequest` 已带 token/user/channel-group、请求 ID、标准化头/body 与 `ProxyRequest`。普通响应返回后，核心只提交一次用量；流式响应须返回 `handled_stream=true`，由流回调在终止时提交用量。插件可以继承 SDK 中公开的 `Gateway` 复用渠道选择、上游传输、SSE 泵送与失败切换，但 token 语义、协议头和错误格式属于插件。

## 渠道 schema

`frontend/channel-types.json` 是声明式文件。核心静态前端无需重新构建即可读取它并渲染渠道创建/编辑表单。V1 字段类型为 `text`、`secret`、`number`、`boolean`、`select`，支持 `label`、`description`、`default`、`required`、`order` 和基础格式校验。

字段可绑定已有渠道列：

```json
{
  "key": "api_key",
  "binding": "api_key",
  "type": "secret",
  "label": "API 密钥",
  "required": true
}
```

允许的绑定只有 `base_url`、`api_key`、`price_multiplier`。未绑定字段持久化到渠道的 `config_json`；V1 不执行插件 JavaScript 或 React 组件。

## 启动、停用与卸载

1. 核心初始化自身 schema 后发现镜像系统包和 `REVLM_PLUGIN_DIR` 中已安装包。
2. 以数据库 migration lock 串行执行未执行的插件 migration，记录 `plugin_id + migration_id`。
3. 加载当前 OS/arch 模块，调用 factory，并验证所有注册项。
4. 成功插件标记为 `active`；ABI、模块、依赖、migration 或路由冲突失败的插件标记为 `failed`，其路由不注册。

停用会设为 `pending_restart`；下一次启动时模块、路由和渠道类型消失。卸载只允许没有启用依赖者的上传包，下一次启动删除其包文件与安装记录，但不执行 down migration，也不删除历史数据。系统包只能停用。root 上传同 ID 包会覆盖系统包；卸载覆盖包并重启后会恢复镜像中的兼容包。

## 构建与打包

先安装与目标 Revlm 完全匹配的 SDK，再在 Linux `amd64` 和 Linux `arm64` 各构建一次：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/RevlmPluginSDK
cmake --build build

python3 packaging/build-package.py OpenAI \
  --linux-amd64 /artifacts/amd64/libOpenAI.so \
  --linux-arm64 /artifacts/arm64/libOpenAI.so \
  --output dist/OpenAI.revlm-plugin
```

GitHub Actions 的 release workflow 会对两个 Linux 架构分别构建模块、合并为上传包，并发布与输入 SDK 版本匹配的资产。Revlm 自己的测试构建也会 `dlopen` 这些相同插件模块，而不是维护核心内的协议回退实现。

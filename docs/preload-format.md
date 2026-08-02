# Revlm 预加载插件格式（format_version 2）

这是受信任代码的替换机制，不是 SDK 扩展点。

插件是与目标 Revlm **同一套完整 C++ ABI** 编译出的 Linux 共享库。启动 worker 前，bootstrap
把已启用模块放进 `LD_PRELOAD`，ELF 的符号解析会优先使用插件定义。插件可以定义它想覆盖的
任何已导出、可插桩的核心 C++ 符号；没有 `Plugin`、manager、registry、factory、路由表或
`HostServices` 白名单。

这意味着安装插件等价于把任意本机代码放进网关进程。没有沙箱、签名、权限隔离、热加载或
热卸载。

## 包内容

上传文件扩展名为 `.revlm-plugin`，内容是 ZIP。生产上传包必须同时包含 Linux `amd64` 与
`arm64` 产物：

```text
plugin.json
backend/linux-amd64/libExample.so
backend/linux-arm64/libExample.so
frontend/entry.js                 # 可选；任意 ESM 入口
frontend/assets/chunk.js          # 可选；entry 的任意相对资源
migrations/0001_initial.sql       # 可选；只进不退
```

归档路径必须是相对 POSIX 路径，不能包含 `..`、绝对路径、符号链接、加密项或 ZIP64。主机在
解压前校验大小、CRC、manifest 和本机平台模块；内容先写入 staging，再原子发布到
`REVLM_PLUGIN_DIR/packages/<id>/<version>/`。

镜像内系统插件使用相同布局，但每个 Linux 镜像只携带自己的一个平台产物；它们不能从镜像
物理删除，但可以停用或被同 ID 上传包覆盖。

## manifest

`plugin.json` 的必填字段：

```json
{
  "format_version": 2,
  "id": "Example",
  "name": "Example",
  "version": "0.1.0",
  "core_abi": "revlm-core-preload-v2",
  "requires": [],
  "load_order": 0,
  "targets": [
    { "os": "linux", "arch": "amd64", "module": "backend/linux-amd64/libExample.so" },
    { "os": "linux", "arch": "arm64", "module": "backend/linux-arm64/libExample.so" }
  ],
  "migrations": []
}
```

- `id`、`version` 只能包含字母、数字、`-`、`_`、`.`；一个平台只能有一个模块。
- `core_abi` 必须和当前 Revlm 的 `revlm::plugin::k_core_abi` 完全相同。它是粗粒度拒绝门；
  真正的 C++ ABI 仍要求使用同一版本的头文件、编译器、标准库和 `librevlm_core` 构建。
- `requires` 只解决包的安装/启用顺序，不声明模块能力。缺失或循环依赖会让对应包不进入
  preload 集合。
- `load_order` 越大越靠前；若模块定义同一符号，先出现的定义获胜。依赖包会排在依赖者后。
  多个模块故意覆盖同一函数时，作者必须自己设计顺序与 `RTLD_NEXT` 链式调用；核心不会替你
  合并或拒绝它们。
- `migrations` 是只进不退的 SQL 文件。卸载不运行 down migration，也不删除历史数据。

首批系统插件 ID/显示名固定为 `OpenAI`、`Anthropic`。既有渠道类型字符串仍分别是
`openai_compatible`、`anthropic`，所以已有渠道无需迁移。

## 写原生替换

安装完整 Revlm core 后，插件直接包含它需要的真实头文件并提供完全相同的普通 C++ 定义。首批
协议插件用 C linkage 只固定 `dlsym(RTLD_NEXT, ...)` 的符号名字；参数和实现仍是完整 C++ ABI，
并不是 SDK 回调：

```cpp
#include <models/catalog.hpp>

namespace revlm {

extern "C" void revlm_models_for_channel_type(std::string_view type,
                                                std::vector<Model>& models) {
    if (type == "my_provider") {
        models = { /* this provider's catalog */ };
        return;
    }
    // Optional: use RTLD_NEXT to continue the ordinary ELF symbol chain.
}

} // namespace revlm
```

官方 Anthropic 插件在同一个 `.so` 中同时定义模型、`/v1/messages`、上游头和 SSE/用量处理；它
不是给核心里的 `anthropic_*` 槽填值。也可以直接覆盖 `UpstreamExecutor::prepare`、`ChannelStore`
的公开方法、计费代码、`revlm_register_http_routes`，甚至自行启动另一套运行逻辑。核心不规定允许列表。相反，`static` 函数、
内联后不产生可抢占符号的函数、隐藏可见性符号和没有走动态链接的代码，本来就无法被
`LD_PRELOAD` 替换；这是 ELF/C++ 的物理限制，不是 Revlm 的权限限制。

不要指望运行中的 `dlopen` 完成这件事：核心一旦已加载，已绑定的 C++ 调用不能可靠地重新
指向新实现。因此变更只能在下一次启动生效。

## 启动与状态

`/revlm` 是短生命周期 bootstrap：

1. 初始化核心数据库 schema，处理包安装状态和插件 migrations。
2. 找出镜像系统包与 `REVLM_PLUGIN_DIR` 中的启用包，按依赖与 `load_order` 得到 preload 列表。
3. 设置 `LD_PRELOAD`，随后 `exec` 同目录的 `/revlm-worker`。
4. worker 由动态链接器带着插件启动；没有运行时 module manager。

包管理仍保留，因为文件、migrations 和多副本状态必须有一个简单的落点；它不读取模块导出，
更不维护“插件登记了哪些函数”。安装、启用、停用、卸载都只改变下一次启动的 preload 集合。

`REVLM_PLUGIN_DIR` 必须是持久化目录。Docker 使用卷；Kubernetes 的所有副本必须挂载同一个
RWX PVC，并在包状态变化后滚动重启所有 worker。若某个模块或迁移失败，该包标记 `failed`，
其模块不被预加载；管理面仍可启动。

## 前端

可选的 `frontend/entry.js` 是普通 ESM。核心前端启动后请求
`GET /api/plugins/frontend`，再动态导入每个条目；`frontend/` 下的相对 chunk、CSS、WASM 和资源会
通过同一路径前缀提供。模块可以自行挂载 React、替换 DOM、修改路由、拦截请求，或什么都不做。没有
schema、字段描述符、React component API 或前端 SDK。资产列表固定为 worker 启动时的 preload 包，
上传或停用不会绕过重启要求。

核心渠道页仅提供“type + 通用列 + 原始 `config_json`”的退化编辑器；插件要更好的 UI，就直接
在自己的 `entry.js` 里实现。

## 构建与打包

在 Linux `amd64` 与 `arm64` 上各自用匹配的 core 安装构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/RevlmCore
cmake --build build

python3 packaging/build-package.py OpenAI \
  --linux-amd64 /artifacts/amd64/libOpenAI.so \
  --linux-arm64 /artifacts/arm64/libOpenAI.so \
  --output dist/OpenAI.revlm-plugin
```

`build-package.py` 不限制插件 ID；它读取任意 `plugins/<目录>/plugin.json` 的 target 路径并把给定
模块放进去。CI 必须在 Linux 上以真实 `LD_PRELOAD` 启动 probe 和集成测试；macOS 的 Mach-O 两级符号绑定
不是该运行模型，不能把本地 macOS 成功编译误当成 Linux 替换验证。

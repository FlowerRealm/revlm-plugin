# Revlm data-plane plugin format V1

A `.revlm-plugin` file is a ZIP archive. It contains `plugin.json`, a Linux
shared object for each supported architecture, `frontend/channel-types.json`,
and optional forward-only SQL migrations.

The first two packages have fixed IDs and display names: `OpenAI` and
`Anthropic`. Their channel database types remain `openai_compatible` and
`anthropic` for compatibility with existing Revlm installations.

The dynamic library must export exactly:

```cpp
extern "C" revlm::plugin::v1::Plugin* revlm_plugin_create_v1();
extern "C" void revlm_plugin_destroy_v1(revlm::plugin::v1::Plugin*);
```

The host performs HTTP limits, request IDs, token authentication, routing,
upstream transport, usage persistence, and billing. A plugin registers exact
data-plane routes and channel type descriptors. It never owns the listener or
loads arbitrary frontend JavaScript.

Package lifecycle is restart-only. The root admin upload marks a package
`pending_restart`; Revlm discovers it from `REVLM_PLUGIN_DIR` on boot, applies
each migration once under the plugin migration lock, and then loads it with
`dlopen(..., RTLD_NOW | RTLD_LOCAL)`. Disabling and uninstalling are also
applied on the next restart. Uninstall never runs a down migration or deletes
plugin data.

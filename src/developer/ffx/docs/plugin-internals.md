# Advanced Plugin Internals

This page is intended for developers who want to understand FFX's plugin system better. FFX is built on top of the [argh](https://docs.rs/argh/0.1.3/argh/) crate. This crate imposes the Google standard for CLI parameters. The structure that argh expects to define the CLI parameters is required at compile time. Therefore, this rules out any runtime plugin systems as Rust structs cannot be edited after compile time.

It also means that any dynamic ability to add plugins must come before compile time. To accomplish this, FFX uses GN build rules to generate the final argh structure from the supplied plugin depenedencies.  You can see that code [here](https://fuchsia.googlesource.com/fuchsia/+/refs/heads/master/src/developer/development-bridge/build/ffx.gni#35).

Internal libraries to FFX require access to top level CLI flags and parameters. For example, the config library needs access to the "--config" flag:

```sh
$fx ffx --config "config-test=runtime" config get --name config-test
```

These internal libraries are available for use in the plugins.  For example, the config library supplies configuration management for plugins.  More about that can be read on the [config](config.md) page.

This means that the final argh structure generated needs to be supplied to these internal libraries. And in turn, the config libraries need to be supplied to the plugins.

This is why there are two libraries generated by the ffx_plugin template.

The Rust attributes that developers decorate their code with generates some code at compile time as well.

The "ffx_command" attribute just creates the following code in the "_args" library:

```rust
pub type FfxPluginCommand = <Your Command>;
```

This provides a known entry point into this library that can used by FFX while allowing the developer to name their command whatever they want.

The "ffx_plugin" attribute does something similiar.

```rust
pub async fn ffx_plugin_impl(<Your methods inputs>) -> Result<(), Error> {
    <Your method name>(<Your parameter names>).await
}
```

The ffx_plugin template requires enough dependencies to allow for the code generated via the Rust attributes to compile.  Therefore the ffx_plugin template gets the following [dependencies](https://fuchsia.googlesource.com/fuchsia/+/refs/hea%20ds/master/src/developer/development-bridge/build/ffx_plugin.gni#99) for free:

 + //sdk/fidl/fuchsia.developer.remotecontrol:fuchsia.developer.remotecontrol-rustc
 + //src/connectivity/overnet/lib/hoist
 + //src/developer/development-bridge/core:lib
 + //src/developer/development-bridge:command_lib
 + //src/developer/development-bridge/config:lib
 + //src/lib/fidl/rust/fidl
 + //third_party/rust_crates:anyhow
 + "//third_party/rust_crates:argh
 + "//third_party/rust_crates:futures
 + "//third_party/rust_crates:log

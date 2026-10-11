Upstream: `tauri-runtime-wry` 2.12.1 from crates.io, with its original Apache-2.0
and MIT license files and `.cargo_vcs_info.json` retained.

Both runtime changes are behind the new `embedded-host` Cargo feature:

- On Windows, call `EventLoopBuilderExtWindows::with_dpi_aware(false)` before
  building the event loop, preserving the containing process's DPI policy.
- Skip the blocking missing-WebView-runtime error dialog. Return the existing
  `WebviewRuntimeNotInstalled` error to the embedding owner instead, allowing
  startup to fail and all owned callbacks to drain without user interaction.

The standalone companion does not enable this feature and retains upstream
behavior. The host explicitly constructs its WebView before `run_return`, so
fallible startup returns a normal error outside Tauri's Ready/setup callback.

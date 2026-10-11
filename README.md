# Cs2-HVH

Open **Cs2 Internal.slnx** in Visual Studio, or work in the **Cs2 Internal**
project folder. Its [README](Cs2%20Internal/README.md) contains the folder map,
build commands, and image-finish instructions.

Build from this directory:

```powershell
powershell -File '.\Cs2 Internal\build.ps1'
```

The rebuilt DLL is **Cs2 Internal/DLL/DeltaEAC.dll**.

Building requires Visual Studio 2022 or newer with the C++ desktop tools and a
Windows SDK, Node.js/npm, and Rust/Cargo with the `x86_64-pc-windows-msvc` target.
The build scripts install the GUI's npm dependencies and build its embedded
Rust library. Keep the npm and Cargo lockfiles and the patched Tauri runtime
sources in the repository.

This is a source-only repository. DLLs, executables, build intermediates,
dependency caches, test captures, personal configuration, and `cs2_assets` stay
local and are excluded by `.gitignore`. The runtime catalogs, images, previews,
and custom game resources in `cs2_assets` are not needed to compile, but must be
provided separately to use the asset-dependent features or create a complete
release package.

Run Git commands from this directory, beside the solution file.

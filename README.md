# NvrMissingTextures

An EchoLoader plugin that lets the **Halloween 2017** event build (Echo Arena 1.76) reach its lobby.

The published Halloween 2017 package lacks about 160 textures that its first global level
(`0x3F9915D3001DC28E`) loads. The game logs each one as "File not found", keeps a null resource for it,
and crashes on that null a moment later. This plugin hooks `CreateFileW`/`CreateFileA`: when the game
fails to open one half of a texture (`_data\...\primary\e8017b774f2b6327\<version>\<id>` or
`...\GPU\e2f9e022d8519ca9\<version>\<id>`) because the file isn't there, it opens the same half of a
texture the package has instead, so both halves always match. Every other open is untouched.

On any other build (checked by the exe's PE timestamp, `0x59E8F804`) it unloads itself.

## Using it

The Echo VR launcher installs it with Halloween 2017: EchoLoader 2 in the game's `BugSplat64.dll` place
loads it from `bin/win7/plugins/`. It logs to `bin/win7/plugin_logs/NvrMissingTextures/`.

Release build: `https://release.echovr.de/launcher/event-builds/NvrMissingTextures-1.0.0.dll`
(sha256 `362f0406841489dbb50607522d4d0846c1325e9276ac2bf8e0ac09567be69909`).

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake
cmake --build build
```

It implements the nEVR plugin ABI v5 (`nevr_plugin_interface.h`) and pins MinHook 1.3.4.

# RatataR RTX Remix Compatibility Fork

This is an experimental fork of **RatataR**, built on top of the original Ratatouille PC launcher/modding tool by SabeMP/0x5abe.

The original RatataR project adds quality-of-life features such as custom resolution support, windowed/borderless modes, frame limiting, gameplay/display tweaks, and no-disc support. This fork keeps those features and adds work-in-progress RTX Remix compatibility code for Ratatouille's Direct3D 9 renderer.

The goal of this fork is to make Ratatouille more usable with RTX Remix by bridging parts of the game's shader-driven render path back through fixed-function-style Direct3D 9 state that RTX Remix can capture more reliably.

> [!WARNING]
> This is experimental compatibility work, not a finished RTX Remix mod. Expect missing geometry, incorrect transforms, UI/sky issues, lighting problems, or crashes depending on game version, level, and RTX Remix/DXVK setup.

## What changed in this fork

- Added an RTX Remix bridge module: `hook/src/RtxRemixBridge.cpp`
- Hooks Ratatouille render queue and draw paths to inspect and optionally resubmit draw calls
- Attempts to reconstruct fixed-function transform state from renderer matrices or shader constants
- Supports proxy/fixed-function submission modes for RTX Remix capture experiments
- Adds vertex declaration/FVF conversion and vertex repacking paths
- Adds filtering for world, UI, sky, transparent, skinned, screen-space, and patch-like geometry
- Adds optional debug logging and per-frame bridge counters
- Adds RTX/DXVK configuration helpers so RatataR can set `DXVK_CONFIG_FILE`
- Adds `rtx-remix/dxvk.conf` as a sample RTX Remix/DXVK config

## Original RatataR features

- Custom resolution
- Windowed/borderless mode
- Developer tools
- Graphical and gameplay improvements
- Frame limiter
- No disc requirement
- Discord rich presence
- Optional frame counter

## RTX Remix bridge

The bridge is enabled by default through `RatataRconfig.ini`.

At startup, RatataR writes a DXVK config to:

```text
%LOCALAPPDATA%\RatataR\dxvk-ratatouille.conf
```

It sets `DXVK_CONFIG_FILE` to that file unless you already set `DXVK_CONFIG_FILE` yourself. If you want full manual control, set `DXVK_CONFIG_FILE` before launching RatataR.

The bridge also writes logs when enabled. Typical log locations are:

```text
%LOCALAPPDATA%\RatataR\RatataR-rtx-bridge.log
%TEMP%\RatataR-rtx-bridge.log
```

## RTX configuration options

These options are stored in `RatataRconfig.ini` under the normal RatataR section.

```ini
rtxRemixBridge=1
rtxDebugLog=0
rtxFailOpen=1
rtxBridgeLogFile=1
rtxAllowUnsafeShaderWvp=0
rtxAllowUnsafeRuntimeReplace=0
rtxBridgeSubmissionMode=originalOnly
rtxTransformMode=rendererMatrixCache
rtxWvpRegister=auto
rtxWvpTranspose=auto
rtxAutoLockWvpRegister=1
rtxVertexBridgeMode=repackAuto
rtxRepackMaxVertices=65536
rtxLightBridgeMode=off
rtxFixedFunctionMaterialMode=captureNeutral
rtxShaderMaterialEmissiveScale=0
rtxCaptureAllowTexturelessProxy=0
rtxMatrixProbeLog=0
rtxProxyGeometryFilter=solidWorldOnly
rtxProxyDeduplicateCommandDraws=1
rtxTransformAssemblyMode=splitWorldViewProjection
rtxProxyAlphaTestMode=solidWorldMasked
rtxProxySkinnedMode=paletteSkinning
rtxProxyCameraLockMode=multiCandidate
rtxProxyUiSkyMode=classifiedProxy
```

Useful experimental values:

- `rtxBridgeSubmissionMode`: `originalOnly`, `proxyAfterOriginal`, `captureProxyAfterOriginal`, `visibleProxyAfterOriginal`, `runtimeMirrorAfterOriginal`, `runtimeFixedFunction`, `replaceVisible`
- `rtxTransformMode`: `rendererMatrixCache`, `deviceTransforms`, `shaderConstantsAuto`, `shaderConstantsRegister`, `shaderConstantsWvp`, `ratatouilleShaderConstants`, `rendererConstant5`, `rendererSlot5World`
- `rtxVertexBridgeMode`: `repackAuto`, `directFvf`
- `rtxLightBridgeMode`: `off`, `debugDirectional`, `shaderConstants`

The safest default is `originalOnly`, which leaves the original draw path active while the bridge initializes and logs. Proxy and replacement modes are for RTX Remix capture/debug work and may affect rendering.

## Usage

1. Install Ratatouille for PC.
2. Copy the built RatataR files into the game folder, or run RatataR from a folder that contains `hook.dll`.
3. Run `RatataR.exe`.
4. Edit `RatataRconfig.ini` to adjust normal RatataR options or RTX bridge options.
5. If using RTX Remix, place the RTX Remix/DXVK files according to the RTX Remix setup you are testing.

The launcher also tries to find the default installed game path:

```text
C:\Program Files (x86)\THQ\Disney-Pixar\Ratatouille\Rat
```

## Play without disc

If you want to play without needing the disc inserted, copy the required game files from the disc into the game directory.

1. If you have multiple discs, mount or insert the second game disc.
2. Open File Explorer.
3. Navigate to the `RAT2` disc.
4. In the View tab, enable hidden items.
5. Copy the `MUSIC` and `VIDEOS` folders into the root of the game folder.

## Current status

This fork is mainly a research/prototype branch for fixed-function pipeline compatibility. It contains a lot of instrumentation and heuristics for RTX Remix capture, including matrix probing, proxy draw classification, vertex repacking, material handling, and bridge counters.

It should be treated as unfinished. The original RatataR functionality is still present, but the RTX Remix bridge may need per-system and per-level tuning.

## Credits

This fork is based on **RatataR** by SabeMP/0x5abe.

RatataR makes use of third-party libraries:

- **MinHook** - A minimalistic API hooking library for x64/x86. Copyright (C) 2009-2017 Tsuda Kageyu.
- **Hacker Disassembler Engine 32 C** - Copyright (c) 2008-2009, Vyacheslav Patkov.
- **Discord rich-presence** by Jay.

See [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) for details.

## Disclaimer

This application is a fan-made project and is not affiliated with or approved by Disney Pixar, Asobo Studio, THQ, NVIDIA, or any other copyright holder.

No copyrighted game assets are distributed. A legally obtained copy of Ratatouille for PC is required.

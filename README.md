# RatataR RTX Remix Compatibility Fork

This is an experimental fork of **RatataR**, built on top of the original Ratatouille PC launcher/modding tool by SabeMP/0x5abe.
The goal of this fork is to make Ratatouille more usable with RTX Remix by bridging parts of the game's shader-driven render path back through fixed-function-style Direct3D 9 state that RTX Remix can capture more reliably.

> [!WARNING]
> This is experimental compatibility work, not a finished RTX Remix mod. Expect missing geometry, incorrect transforms, UI/sky issues, lighting problems, or crashes depending on game version, level, and RTX Remix/DXVK setup.

This fork is mainly a research/prototype for fixed-function compatibility.
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

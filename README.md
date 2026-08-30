<div align="center">

<img src=".github/logo.png" width="96" alt="SoulMeter">

# [SoulMeter](https://discord.gg/7ynGDqcnPJ)

**A real-time DPS meter and combat overlay for [SoulWorker](https://store.steampowered.com/app/1377580/Soulworker/).**

Live damage tables, per-player breakdowns, buff uptime, DPS graphs, a skill timeline and a combat log — drawn straight over the game.

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599C)
![UI](https://img.shields.io/badge/UI-ImGui%20%2B%20DirectX%2011-5C2D91)
[![Build](https://github.com/TyrantRey/SoulMeter-1/actions/workflows/build.yml/badge.svg)](https://github.com/TyrantRey/SoulMeter-1/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/TyrantRey/SoulMeter-1?display_name=tag&color=brightgreen)](https://github.com/TyrantRey/SoulMeter-1/releases/latest)

</div>

---

## What it does

SoulMeter reads the game's network traffic and turns it into a live picture of your run — who is doing what damage, which buffs are up, when the boss died and why your numbers look the way they do.

| Feature | What you get |
|---|---|
| **Damage table** | Live DPS, total damage, share %, crit rate and hit counts for every player in your party |
| **Per-player detail** | Click a name to break a player down by skill — damage, casts, crit rate, average hit |
| **Buff tracking** | Uptime for armour break, attack speed, boss damage and more |
| **DPS graphs** | Damage-over-time plots per player |
| **Skill timeline** | Every cast per player on the raid clock as a Gantt chart — each bar spans the time its skill kept hitting and shows the damage it dealt — with a filterable list and CSV export |
| **Combat log** | A recorded blow-by-blow of the encounter |
| **History** | The last 50 runs are kept and can be reopened and compared |
| **Ping** | Live latency, measured from the game's own heartbeat exchange |
| **Faster loading** | Cuts cold game startup from ~95s to ~38s — see [Load-time optimisations](#load-time-optimisations) |
| **Maze hotkeys** | Restart or leave a maze from a key of your choice |
| **Chat paste** | Paste text copied anywhere in Windows into the game's chat box, with the game's own `Ctrl` + `V` |
| **Localised** | English, 日本語, 한국어, 繁體中文 |

---

## Getting started

> [!IMPORTANT]
> **No external loader is needed.** Earlier releases required a separate injector — this one injects itself.

1. Grab a [release](https://github.com/TyrantRey/SoulMeter-1/releases/latest) (or [build it yourself](#building-from-source)).
2. Keep the folder layout intact:
   ```
   SoulMeter.exe
   SoulMeterHook.dll     <- capture DLL, injected into the game
   sqlite3.dll
   SWDB.db               <- skill / monster / map names
   Lang/                 <- en, jp, kr, zh_tw
   Font/                 <- optional, drop .ttf files here
   ```
3. **Run `SoulMeter.exe` as Administrator**, then launch the game.

The meter waits for the game process, injects automatically the moment it appears, and switches from *Waiting for SoulWorker* to your world name once packets start flowing.

### Hotkeys

| Action | Default |
|---|---|
| Reset the current run | `Ctrl` + `Del` |
| Restart maze | not set |
| Leave maze | not set |

Rebind them under **Options → Hotkey**: click a binding, press up to three keys, and it is saved when you release them. The maze actions send real commands to the game and are refused in town, like the in-game versions.

### Tips

- **Right-click the title bar** for the full feature menu.
- **Left-click a character's row** to open their detailed breakdown.
- In the skill timeline, `Ctrl` + wheel zooms the time axis, dragging pans it, and a double-click (or **Fit**) shows the whole run again.
- Non-Latin text not rendering? Drop a font covering your language into `Font/` and pick it in the Font Selector.
- **Pasting into chat** works with the game's own `Ctrl` + `V` — SoulMeter keeps the game's internal clipboard in step with the Windows one, which is the part the client itself never did. Line breaks become spaces and a paste is capped at 512 characters. It is on by default; untick it under **Options → Features** to keep your Windows clipboard out of the game.
- User settings live in `option.xml` and `imgui.ini`; saved history in `SoulMeter.dat`.

---

## How it works

Capture happens **inside the game process** rather than on the wire, which means no packets are missed and no traffic is intercepted at the network layer.

```
SoulMeter.exe                              SoulWorker (game process)
├── Injector ──── injects ───────────────► SoulMeterHook.dll
│                                          └── detours the game's own
│                                              packet (de)serialisers
└── PipeReceiver ◄── \\.\pipe\SoulMeterHook ──┘  (complete, plaintext frames)
         │
         └──► SWPacketMaker → damage / buff / combat state → ImGui overlay
```

The hook attaches to the client's own serialiser and deserialiser rather than to `ws2_32`. That matters: the client is an IOCP application issuing overlapped `WSARecv`, so socket-level hooks never observe a single byte. Hooking the game's own functions also means the client has already reassembled the TCP stream and decrypted the packet body by the time we see it — so frames arrive complete, in order, and exactly once, with no reassembly on our side to fall out of sync.

> [!NOTE]
> The hook finds those functions at runtime by signature-scanning `SoulWorker64.dll`, so a game patch that merely moves them needs no changes here. The scan fails closed — if the signature is missing or matches more than once, the hook never attaches rather than detouring the wrong code. If that happens, `kSerializeSig` in `SoulMeterHook/sockethooks.cpp` needs re-extracting.

**Built with** ImGui + ImPlot on DirectX 11 · SQLite for game data · FlatBuffers for history · MinHook for the detours · nlohmann/json for i18n · tinyxml2 for settings.

---

## Load-time optimisations

Since the hook is already inside the game process, it also removes the work that dominates a cold start — plus a delay paid on every zone change. Measured on a 511199 client, startup went from **~95s to ~38s**.

| What the client does | What the hook does | Saved |
|---|---|---|
| MD5-hashes `data01.v`–`data60.v` and `packinginfo.dat` — **9.2 GiB of file content** — on *every* launch | Memoises each hash against the file's size and last-write time | **~57s** |
| Reads archive file tables in ~194-byte chunks, seeking before nearly every one — 2.57M reads in ~5s, ~80% of it inside `ReadFile` | Serves those reads from 32 KB blocks in a 32 MB cache, collapsing ~2.5M syscalls to ~26k | **~3s** |
| Holds the zone loading screen for a hardcoded 0.5s *after* the map is already resident | Zeroes that one timer | 0.5s per warp |

The hash is **memoised, not skipped** — the value handed back is the one the client would have computed, so a local comparison or a server-side check sees no difference. Archives that change on patch day are rehashed automatically, so updates need no action.

Hashes are cached in `swmd5cache.txt` next to the game executable, or in `%LOCALAPPDATA%\SoulMeter\` when the install folder isn't writable (Steam, Program Files). Deleting it costs one slow launch, nothing more.

> [!NOTE]
> Each optimisation locates its target by signature and **fails closed** — if a game patch moves or changes the code, that optimisation quietly disables itself rather than acting on the wrong bytes, and the meter carries on as normal. All three were verified against GB 511199, GB 0820, KR and KR 2.4.27.0.

---

## Building from source

Requires **Visual Studio 2022** or newer with the **v143** toolset (C++20).

```bash
MSBuild "Soulworker Utility.sln" -m -p:Configuration=Release -p:Platform=x64
```

The solution builds `Soulworker Utility` (the meter) and `SoulMeterHook` (the injected DLL). A Release build leaves you with:

- `x64\Release\` — the binaries, with `Lang\` copied alongside.
- `x64\Release\SoulMeter-<version>.zip` — a ready-to-ship package (meter, hook DLL, `sqlite3.dll`, `SWDB.db`, `Lang\`, `Font\`), staged from `x64\Release\package\`. The version is read from `Soulworker Utility\SWConfig.h`.

Fonts are not tracked in git. Drop the `.ttf` you want to ship into `Soulworker Utility\Font\` before building; without one the build warns and packages without a font.

### Building on Linux

The Windows binaries can also be cross-compiled from Linux (or macOS) with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and CMake. Clang is required rather than GCC-mingw — the hook DLL uses `__try`/`__except`, which only clang implements for mingw targets.

```bash
# once: llvm-mingw on PATH (any recent release; CI pins 20260826)
curl -sSL https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64.tar.xz | tar xJ
export PATH="$PWD/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin:$PATH"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake
cmake --build build --target dist
```

`dist` leaves the same package as the MSBuild release in `build/package/` and `build/SoulMeter-<version>.zip`. The binaries are linked statically against libc++, so they need nothing beyond what the MSVC build needs. Two things differ from the MSVC build: the source lists live in `CMakeLists.txt` and must be kept in sync with the `.vcxproj` files when files are added, and the save file is opened without MSVC's `_SH_DENYRW` share lock (other standard libraries have no equivalent; the single-instance mutex already keeps a second meter out).

### Continuous builds and releases

[`build.yml`](.github/workflows/build.yml) runs the same build on GitHub Actions:

- **Every push and pull request** builds Release x64 and keeps the zip as a workflow artifact. A second job cross-compiles the same package on Ubuntu with llvm-mingw (artifact `SoulMeter-<version>-mingw`) as a build check; releases come from the MSVC build.
- **Pushing to `main`** publishes a GitHub Release `v<version>` — tag, generated notes and the zip — whenever `SWConfig.h` carries a version that has not been released yet. Bumping that version is the whole release process; an already-released version is left untouched. A hand-made `vX.Y.Z.W` tag is released the same way and must match `SWConfig.h`.
- To ship a font from CI, set the repository variable `SOULMETER_FONT_URL` to a direct download link, or commit a `.ttf` under `Soulworker Utility\Font\`.

---

## Credits

This project stands on the work of the people who built and maintained SoulMeter before it.

| Author | Contribution |
|---|---|
| **[FeAr](https://github.com/fearek/DPSMeter/)** | `fearek/DPSMeter` — the Global-server meter this repository continues from |
| **[AFNGP](https://github.com/AFNGP/SoulMeter)** | `AFNGP/SoulMeter` — long-running maintenance and feature work |
| **[0xarray](https://github.com/0xarray/SoulMeter)** | `0xarray/SoulMeter` — the in-process hook, load-time work and the base this fork builds on |

Original project by **Park3740**. Special thanks to **@Nyanchii** and **@ga0321**.

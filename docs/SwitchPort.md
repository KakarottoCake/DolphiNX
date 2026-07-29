# Nintendo Switch port

This branch is a Switch-focused downstream of current Dolphin. The native Horizon/libnx frontend
was imported from [NaGaa95/dolphin-nx](https://github.com/NaGaa95/dolphin-nx), while
[dolphin-emu/dolphin](https://github.com/dolphin-emu/dolphin) remains the upstream codebase.

The priorities are:

1. Preserve Dolphin accuracy and compatibility.
2. Reach full emulation speed at official clocks before judging an overclocked result.
3. Keep Switch-only code behind `DOLPHIN_SWITCH` or inside `Source/Core/DolphinSwitch`.
4. Carry narrowly scoped, documented per-game workarounds only when measurement demonstrates a
   real need.
5. Merge upstream frequently instead of accumulating a long-lived source snapshot.

The Switch toolchain retains Dolphin's upstream ARMv8-A feature selection and tunes instruction
scheduling for the console's Cortex-A57 CPU. Release builds use Dolphin's existing LTO path. The
port deliberately does not enable broad unsafe-math flags: those can trade away emulation accuracy
and make compatibility failures difficult to diagnose.

## Host performance profiles

Host clock profiles use the API v4 IPC service supplied by
[sys-clk](https://github.com/retronx-team/sys-clk). Dolphin does not write voltage, memory-timing,
kernel, or Atmosphere configuration. At session start it records the current sys-clk enabled state
and three existing overrides; at session end it restores all four values.

| Profile | Handheld CPU/GPU/MEM | Charging CPU/GPU/MEM | Docked CPU/GPU/MEM |
| --- | --- | --- | --- |
| Stock baseline | 1020/384/1331 | 1020/384/1331 | 1020/768/1600 |
| Balanced | 1224/460 or 614/1600 | 1428/460 or 614/1600 | 1428/691/1600 |
| Performance | 1428/460 or 614/1600 | 1581/460, 614, or 768/1600 | 1581/768/1600 |
| Maximum (opt-in) | 1785/460 or 614/1600 | 1785/460, 614, or 768/1600 | 1785/768/1600 |

GPU selection depends on Erista/Mariko and charger mode. Unknown hardware receives the conservative
Erista limit. sys-clk applies its own platform caps after Dolphin requests a target, so the in-game
Host Performance page displays both requested and measured clocks.

At 75 C, Dolphin requests the official-clock profile. The selected profile resumes only after every
reported sensor has fallen to 70 C or below. This is an additional guard, not a guarantee for every
aged, repaired, or physically modified console. Maximum is deliberately excluded from the default
path.

Settings are global or per game:

- **One-button benchmark** temporarily forces the clean baseline described below for every launched
  game. It does not overwrite the user's normal Dolphin or per-game settings.
- **Host clock profile** chooses the session target.
- **Performance metrics log** writes a 500 ms CSV containing emulator FPS/VPS, emulation speed,
  clocks, temperatures, power, mode, and thermal-guard state.
- **Detailed frame-time log** enables Dolphin's native per-frame `render_times.txt` and
  `vblank_times.txt` logs.

Metrics CSVs are stored under `sdmc:/switch/dolphin/Logs/Performance`.

## Super Mario Sunshine baseline

Use a legally dumped copy of the game. Test the same revision, save, route, camera direction, and
duration for every build.

Arm **Settings > Host Performance > One-button benchmark**. For the active session this forces
stock host clocks, ARM64 JIT, dual core, fastmem, DSP HLE, 100% emulation speed, no emulated
CPU/VBI overclock, 1x internal resolution, no AA/high-resolution textures/post-processing/frame
generation, VSync off, and both logs on. Dolphin's normal and per-game settings return after the
session.

Recommended first acceptance pass:

1. Arm **One-button benchmark** and launch the game.
2. Run the route once to populate the shader and pipeline caches. Do not use that run for scoring.
3. Restart Dolphin, begin from the same save/state, wait 30 seconds, and record at least ten minutes.
4. Repeat in handheld and docked modes. To measure clock sensitivity afterward, disarm benchmark
   mode, enable both logs, and repeat with the Balanced and Performance profiles.

The initial target for a stable 30 FPS game is:

- average emulation speed at least 99.5%;
- 1% low emulation speed at least 99.0%;
- no thermal-guard activation;
- no sustained audio underrun;
- frame-time outliers attributable to gameplay, not first-run shader compilation.

Analyze copied logs on a PC:

```sh
python Tools/Switch/analyze_performance.py \
  --render render_times.txt \
  --vblank vblank_times.txt \
  --metrics GMSE01-YYYYMMDD-HHMMSS.csv \
  --render-target 30 \
  --vblank-target 60 \
  --warmup 30
```

The analyzer prints a baseline PASS/FAIL verdict using the thresholds above. It also compares
reported speed with Dolphin's unthrottled speed estimate: low unthrottled headroom points toward a
host compute limit, while normal headroom with low observed speed points toward pacing,
synchronization, or I/O stalls.

Do not treat frame generation as a pass. It may improve display fluidity, but the emulator-speed
column must independently remain at full speed.

## Building

Requirements:

- devkitPro with devkitA64, libnx, SDL2, SDL2_ttf, SDL2_image, curl, and the other Switch portlibs;
- all Dolphin Git submodules;
- an NVK Switch SDK archive compatible with the port.

Initialize dependencies:

```sh
git submodule update --init --recursive
```

Build from a devkitPro/MSYS2 shell:

```sh
export DOLPHIN_SWITCH_NVK_SHA256="<trusted archive SHA-256>"
Tools/Switch/build.sh "/path/to/mesa-switch-vulkan-sdk.zip"
```

The checksum is optional for local experimentation and strongly recommended for repeatable builds.
The output is `build-switch/Binaries/dolphin.nro`. Override the package version with
`DOLPHIN_SWITCH_VERSION`.

## Keeping up with Dolphin

The intended remotes are:

```text
upstream    https://github.com/dolphin-emu/dolphin.git
dolphin-nx  https://github.com/NaGaa95/dolphin-nx.git
```

For a private downstream, merge upstream rather than repeatedly rebasing the large donor import:

```sh
git fetch upstream
git switch switch/main
git merge --no-ff upstream/master
```

Resolve conflicts in small Switch-owned commits, run the Switch compile checks, and hardware-test the
stock profile before changing clock targets or adding a workaround. Keep `git rerere` enabled locally
if the same integration conflict recurs.

When a Switch change must touch shared Dolphin code, isolate it in its own commit and explain the
measured reason. This keeps regressions bisectable and makes future upstream merges substantially
easier than a single rolling port commit.

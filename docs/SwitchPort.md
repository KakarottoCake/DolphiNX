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

## Running and startup diagnostics

Dolphin requires full application memory. Launch the Homebrew Menu with title takeover (normally by
holding **R** while starting an installed game), then start Dolphin. Do not launch it from the Album
applet. An installed full-application forwarder is also suitable.

The diagnostic startup path writes `sdmc:/dolphinx-startup.log` and flushes every completed stage.
Mesa/NVK messages go to `sdmc:/dolphinx-mesa.log`. If Dolphin exits:

- no startup log means execution failed before `main`, commonly because it was launched as an
  applet rather than through title takeover;
- a log ending before `launcher: entering UI` identifies the last completed service stage;
- a log ending at `game: starting Dolphin core and Vulkan probe` isolates the failure to graphics
  initialization.

Normal menu launches currently leave networking off because libnx socket initialization was one of
the original pre-UI crash candidates. An nxlink launch initializes networking for live diagnostics.

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

The first Erista hardware capture (`GMSE04`, handheld, stock clocks, commit `9b8a794708`) averaged
27.8% emulation speed and 7.7 rendered FPS after warm-up. Temperatures remained below 42 C and no
thermal guard activated. Raw render times contained sustained 70-150 ms frames and repeated
200-380 ms stalls. This is far beyond what safe clocks can recover.

That capture identified an unconditional `nvFenceWait` after every successful `EXEC` in the pinned
switch-nvk winsys. It was useful while bringing up the smoke test, but it fully serialized CPU and
GPU work. DolphiNX carries `Tools/Switch/patches/switch-nvk-async-submit.patch` to return after
queueing work while retaining the completion fence on Vulkan signal sync objects. Explicit
dependencies and queue/device-idle operations still wait through `drmSyncobjWait`.

## Building

Requirements:

- devkitPro with devkitA64, libnx, SDL2, SDL2_ttf, SDL2_image, curl, and the other Switch portlibs;
- all Dolphin Git submodules;
- an NVK Switch SDK archive compatible with the port. The preferred package is built from
  [HayatoG/switch-nvk](https://github.com/HayatoG/switch-nvk); Dolphin also retains compatibility
  with the older multi-archive NVK bundles used by the donor port.

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

The initial pre-release used switch-nvk commit
`6eec707da3ad5f86c64f748226583202801bfd03` with Mesa 25.0.7. HayatoG's package includes the
Nouveau DRM, NWindow WSI, and loaderless Vulkan shims in one static archive; the build detects that
layout and avoids linking the donor port's older standalone DRM archive.

Performance builds retain that pinned source and Mesa version, then apply the asynchronous-submit
patch above before running switch-nvk's `package-nvk.sh`. The resulting archive's build information
is recorded in `Tools/Switch/switch-nvk-async-build-info.txt`.

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

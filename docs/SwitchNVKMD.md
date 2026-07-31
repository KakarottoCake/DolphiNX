# Switch NVK: native `nvkmd` backend

Goal: **full-speed Super Mario Sunshine with headroom.** Everything here is judged by that, not by
architectural tidiness.

Baseline to beat: `pre.3` measured 27.8% average speed / 7.7 FPS on Sunshine, with 70–150 ms render
times and repeated 200–380 ms stalls. That is a ~3.6x deficit.

## What Tico does differently (recovered from `tico-dolphin.nro`)

Tico Dolphin reaches full speed on the same hardware, and the binary shows why. It is **not** a
different graphics API — it is the same Vulkan/NVK stack with a different bottom layer.

Confirmed present in their NRO: `vkCreateInstance`, `VK_NN_vi_surface`, `Mesa 26.1.1`, `nouveau`,
and libnx `nv` calls (`nvGpuChannelKickoff`, `nvMapCreate`, …). No deko3d.

The decisive find is their leaked source paths and log strings:

```
../src/nouveau/vulkan/nvkmd/switch/nvkmd_switch_dev.c
../src/nouveau/vulkan/nvkmd/switch/nvkmd_switch_pdev.c
nvkmd-switch: nvGpuChannelKickoff failed: 0x%x (entries=%u, error type=%u, info=[%u,%u,%u,%u])
```

**They wrote a native `nvkmd` backend.** `nvkmd` is NVK's kernel-mode-driver abstraction;
`nvkmd/nouveau/` is merely one implementation of it. Tico added `nvkmd/switch/`, calling libnx `nv`
directly.

We instead inherited HayatoG's `drm_shim`: a userspace reimplementation of the Linux DRM ioctl ABI
(GEM, VM_BIND, syncobj, EXEC) that NVK's nouveau backend then talks to. Every GPU operation is
marshalled into a fake ioctl, decoded, and re-emitted as libnx calls. That layer is where every
failure since the Mesa 26 rebase has lived, and it is pure overhead on the hot path.

### Design details their error strings give away

| String | What it implies |
|---|---|
| `GPFIFO skid exhausted while %s (entries=%u, need=%u, skid=%u, queue=%u)` | A skid buffer over the GPFIFO ring, with **intermediate kickoffs** when it fills — i.e. real async submission |
| `intermediate kickoff failed while %s` | Submission continues without draining |
| `GetVARegions failed` / `no VA region for page size 0x%x` | VA space taken from the **real** `nvAddressSpaceGetVARegions`, chosen per page size — not a hand-picked fixed arena |
| `nvAddressSpaceAllocFixed` / `MapFixed` | Proper fixed-VA allocation |
| `failed to make coherent BO uncached` | Explicit CPU/GPU coherency handling |
| `nvMultiFenceWait` | Multi-fence waits |
| `mtx_init` / `cnd_timedwait` | C11 mutex + condvar: completion tracked asynchronously, not by blocking after each submit |
| `context bind not implemented yet`, `dma-buf import not implemented yet`, `dma-buf export is unsupported` | What they deliberately skipped — useful scope guidance |

The contrast that matters for performance: our `drm_shim` calls `nvFenceWait` after **every**
submit, fully serializing CPU and GPU. Tico's strings describe a pipeline that never does that.

## Why this is tractable

The interface is small. In Mesa 26.1.1:

- `src/nouveau/vulkan/nvkmd/nvkmd.h` — 627 lines, five ops tables
  (`nvkmd_pdev_ops`, `nvkmd_dev_ops`, `nvkmd_mem_ops`, `nvkmd_va_ops`, `nvkmd_ctx_ops`)
- `src/nouveau/vulkan/nvkmd/nouveau/` — ~1,500 lines total, the reference implementation to mirror

A Switch backend is comparable in size, and it **deletes** ~1,700 lines of `drm_shim` plus the DRM
emulation it feeds.

## Plan

1. Fresh Mesa 26.1.1 tree, Switch patches **without** `drm_shim` or the libdrm detour.
2. Implement `nvkmd/switch/` against libnx: pdev → dev → mem → va → ctx, in that order.
3. Bring up in stages, measuring at each: instance → physical device → device → submit → present.
4. Only then tune. Async submission and the skid buffer are the first performance levers, because
   the current serializing drain is a hard ceiling regardless of anything else.

Hacks are acceptable where they buy speed and are written down.

## Ground rules for this effort

Adopted after four wrong published diagnoses in the previous attempt (`pre.8`, `pre.9`, `pre.10`,
`pre.13`), all of which were plausible mechanisms reasoned toward rather than measured:

- No fix ships on a mechanism that has not been observed in a log or a disassembly.
- Instrumentation goes in **before** the guess, not after the third failed attempt.
- Absence of a debug string proves nothing — our own release builds strip the same strings.
- Comments in inherited code are claims, not evidence. The `drm_shim` v22/v32 comments contradicted
  each other and one of them was wrong.

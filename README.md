# HDMIB — HDMI on DDI B for Intel Ice Lake

A [WhateverGreen](https://github.com/acidanthera/WhateverGreen) module that makes the HDMI
port work on Ice Lake LP laptops, where Apple's framebuffer driver only knows how to talk
DisplayPort on that DDI.

> **Status: 1.0-rc3.** Working daily on one machine (ASUS X415JA, i5-1035G1, Iris Plus G1).
> It has **not** been tested anywhere else. It patches undocumented structures of Apple's
> kext, so a wrong offset is a kernel panic — which is why it stays idle on any macOS
> version whose layout has not been verified.

---

## The problem

`AppleIntelICLLPGraphicsFramebuffer` treats DDI B as DisplayPort. It probes the sink with
DPCD reads over the AUX channel, and an HDMI sink has no DPCD, so the port never comes up:

```
Hotplug detected on unsupported ddi 1
```

The module makes that port work as HDMI/TMDS: HPD-only detection, its own EDID reader
(GMBUS with a GPIO bit-bang fallback), the display-path plumbing Apple's client never does
for an injected framebuffer, DBUF slice 2 power-up for pipe B, a real hardware cursor on
the HDMI framebuffer, and work-arounds for the panics that come with HDMI hot-plugging
(DC6 double-enable, endless AUX timeouts, VRR errors).

---

## Status

**Legend** — ✅ verified working · ⏳ works within the stated limits · ❌ not implemented

| | Component | Status | Notes |
|---|---|:--:|---|
| 🖥️ | HDMI video | ⏳ | Confirmed at **1360×768**, the only mode tested. Higher resolutions and 4K are **pending testing**, not known to be broken |
| 🔊 | HDMI audio | ❌ | Not implemented. Audio over the 3.5 mm jack is unaffected |
| 💻 | Internal panel | ✅ | Unchanged by the module; brightness works |
| 🖱️ | Hardware cursor on HDMI | ✅ | Falls back to a software cursor if the cursor memory looks wrong |
| 🔌 | Hot-plug / unplug | ✅ | 5 cycles in a row without a panic; long soak tests pending |
| 😴 | Sleep / wake with HDMI | ✅ | Both with the cable plugged and unplugged during sleep |
| 🎬 | Acceleration (Metal, video decode) | ✅ | Unaffected; the module only touches the framebuffer's port path |

Verified on the reference machine:

```
HDMIB-version = 1.0-rc3
HDMIB-layout  = macOS 15 (Sequoia)
HDMIB-hooks   = installed
HDMIB-port    = ddi=1 gmbus=2 class=2 type=0x800 flags=0x2c1 OK (HDMI)
HDMIB-status  = hotplugs=1 edid-ok=1 edid-fail=0 dbuf-fixed=1 ...
```

### Supported versions

| macOS | Darwin | Layout | Notes |
|---|---|:--:|---|
| 26 Tahoe | 25 | ✅ verified | The version the module was developed on |
| 15 Sequoia | 24 | ✅ verified | Static layout check (`tools/reference_darwin24.json`) plus hardware run on Sequoia 15.8.2 (24H232) |
| anything else | — | ⚠️ idle | The module does nothing instead of risking a panic (`igfxhdmib-force=1` overrides) |

Run [`verify_layout.sh`](verify_layout.sh) **before** updating macOS: it reads the kernel
collection on the running system and tells you whether the layout table still matches. If
it does not, the module stays idle and HDMI stops working until the table is extended.

### Scope

Only **Ice Lake LP** (`AppleIntelICLLPGraphicsFramebuffer`, i3/i5/i7-10x5G*, Gen11). Not
Tiger Lake, not Comet Lake, not Ice Lake H. Only **platform ID `01005c8a`** has been
verified; other Ice Lake IDs need the porting steps below.

---

## Install

### 1. Get the kext

Either download `WhateverGreen-HDMIB.kext.zip` from
[Releases](../../releases) and unzip it, or build it yourself:

```bash
bash build_weg_hdmib.sh          # needs full Xcode, not just Command Line Tools
```

The build clones Lilu, MacKernelSDK and WhateverGreen, adds this module with two small
edits to `kern_igfx.cpp`, and leaves `WhateverGreen-HDMIB.kext` next to the script. Pin
upstream with `WEG_REF=<commit> LILU_REF=<commit>`.

Put the kext in `EFI/OC/Kexts/` and add it to `Kernel → Add`. Use it **instead of** the
stock `WhateverGreen.kext` — never both.

### 2. Enable it with a boot argument

```
-igfxhdmib
```

Without that argument the module does nothing, so the kext is safe to keep installed.
Boot arguments must be edited in `config.plist` (`NVRAM → Add → 7C436110-…`). If your
config also lists `boot-args` under `NVRAM → Delete`, it is rewritten on every boot and
`nvram boot-args=…` will not survive a reboot.

### 3. Patch the connector

WhateverGreen's `framebuffer-patchN-*` rewrites an unused connector entry of the platform
table. An entry is six `u32`: `{ddi, gmbus pin, class, pad, type, flags}`. For
`01005c8a`, this turns the table's 4th entry (DDI D, DisplayPort) into HDMI on DDI B,
GMBUS pin 2:

```xml
framebuffer-patch-enable   01000000
framebuffer-patch0-enable  01000000
framebuffer-patch0-find    03000000 0a000000 01000000 01000000 00040000 c1020000
                           {ddi 3, pin 10, class 1, pad 1, type 0x400 DP,   flags 0x2c1}
framebuffer-patch0-replace 01000000 02000000 02000000 00000000 00080000 c1020000
                           {ddi 1, pin 2,  class 2, pad 0, type 0x800 HDMI, flags 0x2c1}
```

The `find` bytes must match an entry of **your** platform table. Inspect it first with
Hackintool (*Patch → Connectors*), or with a DEBUG WhateverGreen and `-igfxfbdump`, which
writes the table to `IOService:/IOResources/WhateverGreen` in ioreg.

`hdmib_setup.sh patch <config.plist>` applies exactly this preset, with a timestamped
backup. It refuses silently to pretend it works on another platform ID.

### 4. Reboot and plug the cable

The module reports what it found:

```bash
ioreg -l | grep HDMIB
```

`HDMIB-port` says `OK (HDMI)` when the connector patch took effect, and `NOT HDMI` when it
did not — in that case HDMI will not light up no matter what.

---

## Reference machine

ASUS VivoBook 14 X415JA, i5-1035G1, Iris Plus G1 (device-id `0x8A56`, spoofed `0x8A5C`),
BIOS DVMT pre-allocated 64M, OpenCore 1.0.8, internal panel 3456×1944, HDMI at 1360×768.
The iGPU entry of its `config.plist` is in
[`example_iGPU_DeviceProperties.plist`](example_iGPU_DeviceProperties.plist).

| Key | Value | Needed by | Notes |
|---|---|---|---|
| `framebuffer-patch-enable`, `framebuffer-patch0-enable` | `01000000` | **the module** | turn the connector patch on |
| `framebuffer-patch0-find` / `-replace` | see step 3 | **the module** | creates the HDMI connector. The `find` bytes must match an entry of *your* platform table |
| `AAPL,ig-platform-id` | `01005c8a` | platform | the only ID verified with this module |
| `device-id` | `5c8a0000` | platform | this ASUS reports `0x8A56` and is spoofed to `0x8A5C` |
| `framebuffer-stolenmem` / `-fbmem` / `-unifiedmem` | `00003001` / `00009000` / `000000c0` | platform | depend on the BIOS DVMT pre-allocation |
| `enable-dbuf-early-optimizer` | `01000000` | platform | fixed a black screen at the login window here; independent of the module |
| `enable-cdclk-frequency-fix`, `enable-dvmt-calc-fix`, `enable-cfl-backlight-fix`, `AAPL,GfxYTile`, `igfxfw` | `01000000` / `02000000` | platform | WhateverGreen / driver options; unrelated to HDMI |
| `AAPL00,override-no-connect` | internal panel EDID | machine-specific | not included in the example |

---

## Boot arguments

| Argument | Default | Meaning |
|---|---|---|
| `-igfxhdmib` | off | enable the module |
| `igfxhdmib-ddi=N` | 1 | DDI carrying the HDMI port (1 = DDI B, the only one tested) |
| `igfxhdmib-fb=N` | auto (2, then read from the driver) | framebuffer index of that port |
| `igfxhdmib-bootdetect=S` | 3 | cable already plugged at boot: run the hotplug S seconds after the first mode set (0 = off). Apple raises no hotplug for a cable plugged before boot |
| `igfxhdmib-hwcursor=0\|1` | 1 | real hardware cursor on the HDMI framebuffer; 0 = software cursor there |
| `igfxhdmib-swcursor=1` | 0 | software cursor on every framebuffer |
| `igfxhdmib-aux=0` | 1 | do not short-circuit the AUX reads the accelerator keeps issuing for the HDMI stream |
| `igfxhdmib-dc6fix=0` | 1 | do not skip the second `enableHWDC6()` (panic *DC6 is already enabled*) |
| `igfxhdmib-edid-fallback=1` | 0 | if the monitor's EDID cannot be read, present a generic 1024×768@60 EDID |
| `igfxhdmib-force=1` | 0 | run on a macOS version whose layout is not verified. **At your own risk** |

Arguments from the development builds (`igfxhdmib-mode`, `-rc6`, `-cpg`, `-sample*`,
`-ring`, `-trace`) are ignored and can be removed.

---

## Diagnostics

```bash
ioreg -l | grep HDMIB              # the reliable one
bash hdmib_setup.sh check          # read-only summary: version, layout, port, displays, boot args
bash hdmib_diag.sh                 # collects everything into ~/Desktop/hdmib_diag.txt
```

`log show` does not reliably show the module's `IOLog` lines; if you need them, add
`--info --debug`. The `hwEnableUnderrunInterrupts` / `hwDisableUnderrunInterrupts` messages
are **not** underruns — filter them out.

| What you see | Meaning |
|---|---|
| `HDMIB-layout = "unverified: idle"` / `"unknown-darwin: idle"` | this macOS version is not in the layout table; the module did nothing |
| `HDMIB-hooks = "symbols-missing: idle"` | the kext lacks a function the module needs; the module did nothing |
| `HDMIB-hooks = "core-route-failed"` | Lilu could not patch the core hooks; HDMI will not work |
| `HDMIB-hooks = "installed"` | hooks are in place |
| `HDMIB-port = … NOT HDMI` | the connector patch is missing or wrong; see step 3 |
| `HDMIB-status = "hotplugs=1 edid-ok=1 …"` | a hotplug ran and the EDID was read. `edid-fail>0` → check cable/adapter, or try `igfxhdmib-edid-fallback=1` |

**A stale kext is the most common failure.** An old build shows ~100 `HDMIB-*` keys and
`HDMIB-mode=0`; current builds show only `HDMIB-version/layout/hooks/status/port`. Compare
hashes before blaming the module:

```bash
shasum -a 1 WhateverGreen-HDMIB.kext/Contents/MacOS/WhateverGreen \
            "/Volumes/NO NAME/EFI/OC/Kexts/WhateverGreen-HDMIB.kext/Contents/MacOS/WhateverGreen"
```

When reporting a problem, include the macOS build, the `ioreg` output, your iGPU
`DeviceProperties`, and the log from boot to the plug.

---

## Known issues

- **HDMI audio does not work.** Not implemented; nothing to configure.
- **Only 1360×768 has been tested.** Higher pixel clocks and 4K are untested, not
  confirmed broken.
- If the internal panel is **not** at its native resolution when the cable goes in, the
  HDMI output can stay black. Set the panel back to native and re-plug.
- Plugging the cable **after login** is the best-tested path. For a cable plugged at boot,
  rely on `igfxhdmib-bootdetect`.
- `VRR_ERR … Forcing remLineForOnGlass` appears on both framebuffers, also on the internal
  panel without the module, and one `kHDCP1_DPCD_AN write failed` follows a mode set on
  the HDMI (Apple tries HDCP over DPCD on a TMDS port). No visible effect observed.
- Long soak tests are still pending.

---

## Porting to a new macOS

All offsets into Apple's structures live in `struct Layout` in
[`kern_igfx_hdmib.hpp`](kern_igfx_hdmib.hpp), keyed by Darwin major version.

**Shortcut:** `bash verify_layout.sh` does steps 1–3 on the Mac you run it on, and writes
`~/Desktop/hdmib_verify.txt` plus a zip.

1. Extract the framebuffer kext from the kernel collection:
   ```bash
   perl tools/kcx.pl /System/Library/KernelCollections/SystemKernelExtensions.kc \
        ~/kcx AppleIntelICLLPGraphicsFramebuffer
   ```
2. `pip install capstone`, then
   ```bash
   python3 tools/layout_fingerprint.py dump ~/kcx/com.apple.driver.AppleIntelICLLPGraphicsFramebuffer \
       new.json kern_igfx_hdmib.hpp
   ```
3. ```bash
   python3 tools/layout_fingerprint.py diff tools/reference_darwin25.json new.json kern_igfx_hdmib.hpp
   ```
   The tool disassembles every function and compares the memory displacements it uses
   against the reference. `RESULT: layout offsets consistent …` means the table is valid.
4. If something changed, add a `Layout{...}` entry for that Darwin major, run the test
   checklist, and only then mark it verified.

The comparison proves the *offsets and symbols* are unchanged; it does not replace running
the checklist on real hardware.

## Test checklist (new machine or macOS version)

1. Boot with `-igfxhdmib`, no cable: `HDMIB-hooks = installed`, no panic, internal panel normal.
2. Plug the cable after login: monitor lights within a few seconds, `HDMIB-status` shows `hotplugs>=1 edid-ok>=1`.
3. Unplug and plug 5 times in a row: no panic, no frozen internal panel.
4. Boot with the cable plugged: monitor lights ≤ ~10 s after the desktop (`igfxhdmib-bootdetect`).
5. Sleep/wake with the cable plugged, and with it unplugged during sleep.
6. Move the pointer across both screens, over translucent areas, with a screensaver running: no double/giant pointer, no panic.
7. Leave it idle 30 min with the monitor on.

---

## How it hooks into WhateverGreen

`build_weg_hdmib.sh` makes two edits to `WhateverGreen/kern_igfx.cpp`:

1. `#include "kern_igfx_hdmib.hpp"` after `#include "kern_igfx.hpp"`.
2. `HDMIB::install(patcher, index, address, size);` inside `IGFX::processKext`, in the
   framebuffer-kext branch, right before the
   `if (applyFramebufferPatch || dumpFramebufferToDisk || …)` block.

`install()` returns immediately unless the kext is the Ice Lake LP framebuffer and
`-igfxhdmib` is set.

### Build details

| | |
|---|---|
| Module | header-only, `kern_igfx_hdmib.hpp` |
| Compile-checked against | WhateverGreen `093e76f`, Lilu `0249c3a`, MacKernelSDK `3f75008` |
| Kext version | `1.7.2` (WhateverGreen's own version) |
| SHA-1 of the released binary | `d5ff7571a4ba606da041f3e62f2d8ff62801ce80` |
| Requirement | full Xcode, Intel or Apple Silicon (the kext is always x86_64) |

---

## Repository layout

```
kern_igfx_hdmib.hpp                  the module (header-only)
build_weg_hdmib.sh                   builds WhateverGreen + module
verify_layout.sh                     one-step layout check for the running macOS
hdmib_diag.sh                        collects diagnostics into a single file
hdmib_setup.sh                       check / patch config.plist / verify kext
example_iGPU_DeviceProperties.plist  iGPU properties of the reference machine
tools/kcx.pl                         kernel-collection extractor
tools/layout_fingerprint.py          offset checker
tools/reference_darwin25.json        reference dump, macOS 26
tools/reference_darwin24.json        reference dump, macOS 15
LICENSE                              BSD-3-Clause
CHANGELOG.md                         version history
```

`hdmib_diag.sh`, `hdmib_setup.sh` and `verify_layout.sh` print their output in Spanish;
the code and the rest of the repository are in English.

---

## Credits and license

This module extends [WhateverGreen](https://github.com/acidanthera/WhateverGreen) and
depends on [Lilu](https://github.com/acidanthera/Lilu) and
[MacKernelSDK](https://github.com/acidanthera/MacKernelSDK), all by Acidanthera. The
approach follows the HDMI mapping used by the Ice Lake kext from the Catalina era.

`kern_igfx_hdmib.hpp` is BSD-3-Clause — see [LICENSE](LICENSE). WhateverGreen, Lilu and
MacKernelSDK are BSD-3-Clause as well, so the released binary keeps the same terms:
redistribute it with the copyright notice and disclaimer.

Not affiliated with Apple, Intel or Acidanthera. Patching Apple's kernel extensions can
panic or brick a boot; you own the consequences.

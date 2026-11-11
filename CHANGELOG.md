# Changelog

## 1.0-rc3 ("v43") — 2026-11 (hardening; kernel-side changes NOT yet tested on hardware)
* `isTarget` now also requires the HDMI connector-type bit (0x800): with `-igfxhdmib` but without the connector patch a real DP port on that DDI is no longer touched.
* `igfxhdmib-ddi` clamped to 0..5, `igfxhdmib-bootdetect` to 0..60 s; GMBUS pin from the platform table accepted for 1..12 (ICL type-C pins).
* New registry keys: `HDMIB-version`, `HDMIB-port` (what the driver really has on the target DDI, with an explicit "NOT HDMI" diagnosis).
* New `hdmib_setup.sh` (macOS only): `check`, `patch <config.plist>` (backup + DeviceProperties + boot-arg), `verify <kext>`. No Linux needed for the 01005c8a preset.

## 1.0-rc2 ("v42") — 2026-11
* macOS 15 Sequoia (Darwin 24) layout marked **verified** (static check + HDMI run on the development machine, 15.8.2 / 24H232).
* `tools/layout_fingerprint.py diff` now judges each `Layout` offset by whether the same functions still use it (a moved field changes that set) and reports
  changed code in hooked functions as information, instead of failing on any difference. Added `reference_darwin24.json`, `verify_layout.sh`.

## 1.0-rc1 ("v41") — 2026-11
Clean-up and generalisation of the development builds v1–v40. Behaviour on the development machine is intended to be identical to v40's
`igfxhdmib-mode=16` configuration (same hooks, same order of operations); everything that only existed to investigate the driver was removed.

* Removed: ~700 lines of research instrumentation (register samplers, ring/PS-counter tracing, AGDC/mode-enumeration observers, DDC self-test,
  RC6/CPG experiment) and the per-mode (`igfxhdmib-mode`) branching — mode 16 behaviour is now the only behaviour.
* Removed: the hard-coded EDID of the development monitor. If the monitor's EDID cannot be read the stack now gets Apple's own answer; an optional generic
  1024x768@60 EDID is available with `igfxhdmib-edid-fallback=1`.
* New: one `Layout` table with every private-structure offset, keyed by Darwin major version. Unknown or unverified versions leave the module idle instead
  of risking a kernel panic (`igfxhdmib-force=1` overrides).
* New: target DDI / framebuffer index / GMBUS pin are no longer fixed to "DDI B, FB2, pin 2": `igfxhdmib-ddi`, `igfxhdmib-fb`, and the pin is read from the platform table.
* New: only the Ice Lake LP framebuffer kext is touched; other GPUs are unaffected even with `-igfxhdmib` set.
* New: required symbols are checked before any hook is installed; essential and optional hooks are routed separately.
* New: `HDMIB-layout`, `HDMIB-hooks`, `HDMIB-status` in the registry for bug reports.
* New: `tools/layout_fingerprint.py` to validate the layout table against another macOS build.
* Defaults now equal the development machine's tested configuration: `bootdetect=3`, `hwcursor=1`.
* Fix: a second EDID block is only returned when it was actually read (v40 could return an all-zero block).

## 0.x ("v1–v40")
Research builds: HPD-only detection, AGDC plumbing, EDID reader (GMBUS/GPIO), DBUF slice 2 power-up, hardware cursor for the HDMI framebuffer, boot-time
detection, AUX short-circuit, DC6 panic fix, VRR off, plus instrumentation. Not published.

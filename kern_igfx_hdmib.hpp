//
//  kern_igfx_hdmib.hpp
//  HDMI on DDI B for AppleIntelICLLPGraphicsFramebuffer (Intel Ice Lake, macOS 15 / 26)
//
//  Header-only module for WhateverGreen. Version 1.0-rc3 (internally "v43").
//
//  Why it exists
//  -------------
//  Apple's Ice Lake framebuffer driver only knows DisplayPort sinks on DDI B: it detects a monitor with DPCD reads over the
//  AUX channel, which an HDMI sink does not have. Plugging an HDMI cable therefore logs "Hotplug detected on unsupported ddi 1"
//  and nothing lights up. This module teaches the driver to treat that port as an HDMI/TMDS sink:
//    * HPD-only sink detection (no AUX / DPCD) for the HDMI port;
//    * its own EDID reader (GMBUS, GPIO bit-bang as a fallback), because Apple's DDC path is DisplayPort-oriented;
//    * the plumbing Apple's AGDC client never does for an injected framebuffer (display path, resource configuration,
//      connect event, hardware cursor memory), and the display-buffer (DBUF) slice 2 power-up that pipe B needs;
//    * work-arounds for panics and stalls that appear with HDMI hot-plugging (DC6 double-enable, AUX timeouts, VRR errors).
//
//  Requirements
//  ------------
//    * The HDMI connector has to exist in the platform table. Add it with DeviceProperties on the iGPU
//      (framebuffer-patchN-*), see README.md. Its {ddi, gmbus pin, class, pad, type, flags} are read back from the driver.
//    * Boot-arg  -igfxhdmib   enables the module. Without it the module does nothing at all.
//
//  Boot-args (all optional)
//    igfxhdmib-ddi=N          DDI that carries the HDMI port (default 1 = DDI B)
//    igfxhdmib-fb=N           framebuffer index of that port (default: 2, then taken from the driver at the first hotplug)
//    igfxhdmib-bootdetect=S   if the cable is already plugged at boot, run the hotplug S seconds after the first mode set
//                             (default 3, 0 = off). Apple never raises a hotplug for a cable that was plugged before boot.
//    igfxhdmib-hwcursor=0|1   1 (default) = real hardware cursor on the HDMI framebuffer, 0 = software cursor there
//    igfxhdmib-swcursor=1     software cursor on every framebuffer
//    igfxhdmib-aux=0          do not short-circuit the AUX reads the accelerator keeps issuing for the HDMI stream
//    igfxhdmib-dc6fix=0       do not skip the second enableHWDC6() (panic "DC6 is already enabled")
//    igfxhdmib-edid-fallback=1  if the monitor's EDID cannot be read, present a generic 1024x768@60 EDID instead of nothing
//    igfxhdmib-force=1        run on a macOS version whose private structure layout has not been verified (see Layout)
//
//  Diagnostics:  ioreg -l | grep HDMIB      (HDMIB-status, HDMIB-layout, HDMIB-hooks)
//                dmesg | grep HDMIB
//
//  The module touches private, undocumented structures of Apple's kext. The offsets live in ONE table (Layout) keyed by the
//  Darwin major version; on a version that is not in the table the module stays idle instead of risking a kernel panic.
//

#ifndef kern_igfx_hdmib_hpp
#define kern_igfx_hdmib_hpp

#include "kern_igfx_kexts.hpp"
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>

namespace HDMIB {

	static constexpr const char *Version = "1.0-rc3";

	// IOLog directly: Lilu's SYSLOG is silent when interrupts are disabled (hotplug paths run that way).
	#define HLOG(fmt, ...) do { if (ml_get_interrupts_enabled()) IOLog("HDMIB: " fmt "\n", ## __VA_ARGS__); } while (0)
	#define HLOG_ALWAYS(fmt, ...) IOLog("HDMIB: " fmt "\n", ## __VA_ARGS__)

	// ---------------------------------------------------------------------------------------------------------------------
	// Private structure layout of Apple's kext, per Darwin major version.
	// The defaults are the offsets verified against the macOS 26 (Darwin 25) kext. A version is only used if it is listed in
	// `layouts` AND marked verified (or the user passes igfxhdmib-force=1).
	// ---------------------------------------------------------------------------------------------------------------------
	struct Layout {
		// AppleIntelPort
		uint32_t portConfig     = 0x440;   // PortConfig *
		uint32_t portState      = 0x44c;   // last probe result: 0 = nothing, 1 = sink, 2 = error
		uint32_t portIndex      = 0x1c;    // index of the framebuffer that owns the port
		// AppleIntelFramebuffer
		uint32_t fbIndex        = 0x1dc;   // framebuffer number (0 = internal panel)
		uint32_t fbFlags        = 0x1e0;   // bit 0 = "online/active"
		uint32_t fbCursorMem    = 0x1f8;   // two u32 cursor buffer addresses
		uint32_t fbCfgTable     = 0x20d;   // resource-configuration table (count + entries)
		uint32_t fbCfgTableLen  = 0x3e0;
		uint32_t fbPathSel      = 0x7d9c;  // u32: 0xffffffff selects fbPath0, anything else fbPathAlt
		uint32_t fbPathCount    = 0x8f28;  // number of display paths
		uint32_t fbPath0        = 0x8f30;  // AppleIntelDisplayPath *
		uint32_t fbPathAlt      = 0x8f48;
		uint32_t fbVrr          = 0x8f75;  // five VRR capability bytes
		uint32_t pathPort       = 0x4d20;  // AppleIntelDisplayPath::port
		// AppleIntelFramebufferController
		uint32_t ctlFbCount     = 0xd14;   // u32
		uint32_t ctlFbArray     = 0xd18;   // AppleIntelFramebuffer *[3]
		uint32_t ctlCursorPhys  = 0xd78;   // u64 base of the cursor region (physical)
		uint32_t ctlCursorSize  = 0xd80;   // u32
		uint32_t ctlCursorCpu   = 0xc58;   // u64 CPU mapping of the cursor region
		uint32_t ctlDC6Count    = 0xbf4;   // s32 enableHWDC6/disableHWDC6 reference count
		// AGDC client structure passed to CallBackAGDC
		uint32_t agdcCfgIndex   = 0xac;    // s32 resource-configuration entry chosen for a link-modeset callback
	};
	struct LayoutEntry { uint32_t darwin; bool verified; const char *name; Layout l; };
	static const LayoutEntry layouts[] = {
		{25, true,  "macOS 26 (Tahoe)",   Layout{}},
		// Sequoia: verified 2026-11 on macOS 15.8.2 (24H232). Static: tools/layout_fingerprint.py shows every Layout offset is used by the
		// same functions as on macOS 26 (reference_darwin25.json vs reference_darwin24.json); the 5 hooked functions whose code differs
		// only touch fields that are not in Layout. Dynamic: HDMI hot-plug, EDID, mode set and cursor ran on an i5-1035G1 (X415JA).
		{24, true,  "macOS 15 (Sequoia)", Layout{}},
	};
	static const Layout *L = nullptr;

	// Hardware registers (Ice Lake).
	static constexpr unsigned long REG_PWR_WELL_CTL2 = 0x45404;
	static constexpr unsigned long REG_DBUF_CTL_S1   = 0x45008;
	static constexpr unsigned long REG_DBUF_CTL_S2   = 0x44fe8;
	static constexpr unsigned long REG_GMBUS0 = 0xC5100, REG_GMBUS1 = 0xC5104, REG_GMBUS2 = 0xC5108, REG_GMBUS3 = 0xC510C,
	                               REG_GMBUS4 = 0xC5110, REG_GMBUS5 = 0xC5120, REG_GPIO_BASE = 0xC5010;

	// PortConfig as stored by the platform table: six u32 {ddi, gmbus pin, class, pad, type, flags}
	struct PortConfig { uint32_t ddi, gmbus, cls, pad, type, flags; };

	// ---------------------------------------------------------------------------------------------------------------------
	// State
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t tddi = 1;                 // target DDI
	static int      fbIdx = 2;                // target framebuffer index
	static bool     fbIdxFixed = false;       // set by igfxhdmib-fb
	static int      hwCursorFb = 1, swCursorAll = 0, auxBlockEnabled = 1, dc6FixEnabled = 1, edidFallback = 0, bootDetectSec = 3;
	static void    *gCtl = nullptr;           // AppleIntelFramebufferController, captured at the first hotplug

	static void *(*getPortByDDI)(uint32_t) = nullptr;
	static bool (*isHPDLow)(void *) = nullptr;
	static uint32_t (*readReg)(void *, unsigned long) = nullptr;
	static void (*writeReg)(void *, unsigned long, uint32_t) = nullptr;
	static uint32_t (*connChanged)(void *) = nullptr;
	static void *(*addDispPath)(void *, uint32_t) = nullptr;
	static uint8_t (*allocPathRes)(void *, uint32_t, uint32_t) = nullptr;
	static void (*injectHPD)(void *, void *) = nullptr;
	static void (*orgUpdateCursorMem)(void *, void *) = nullptr;

	static uint32_t (*orgProbePortState)(void *) = nullptr;
	static IOReturn (*orgGetOnlineInfo)(void *, void *, uint8_t *, uint8_t *) = nullptr;
	static void (*orgHotplug)(void *, uint32_t, uint32_t) = nullptr;
	static int (*orgSetDPPower)(void *, uint8_t) = nullptr;
	static int (*orgGetSinkCount)(void *, uint8_t *) = nullptr;
	static int (*orgGetDPCDInfo)(void *) = nullptr;
	static int (*orgGetDDCBlock)(void *, uint32_t, uint8_t *, bool) = nullptr;
	static IOReturn (*orgStreamAux)(void *, void *, void *) = nullptr;
	static bool (*orgCallbackAGDC)(void *, uint32_t, unsigned long, uint32_t) = nullptr;
	static uint32_t (*orgLightUpTMDS)(void *, void *, void *, void *) = nullptr;
	static uint32_t (*orgHwSetMode)(void *, void *, void *, int, void *) = nullptr;
	static uint32_t (*orgHwSetCursorImage)(void *, void *, void *) = nullptr;
	static uint32_t (*orgHwSetCursorState)(void *, void *, int, int, bool, bool) = nullptr;
	static void (*orgInitVRRCaps)(void *) = nullptr;
	static void (*orgEnableHWDC6)(void *) = nullptr;

	static uint32_t hotplugs = 0, lastPortMode = 0, edidReads = 0, edidFails = 0, dbufFixed = 0, dc6Skipped = 0, auxBlocked = 0,
	                cursorBlocked = 0, vrrOff = 0;

	// ---------------------------------------------------------------------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------------------------------------------------------------------
	static inline bool ptrOk(const void *p) {
		auto v = reinterpret_cast<uintptr_t>(p);
		return v >= 0xffff800000000000ULL && (v & 0x3) == 0;
	}
	template <typename T> static inline T rd(const void *base, uint32_t off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(base) + off); }
	template <typename T> static inline void wr(void *base, uint32_t off, T v) { *reinterpret_cast<T *>(static_cast<uint8_t *>(base) + off) = v; }

	static inline const PortConfig *configOf(void *port) {
		if (!L || !ptrOk(port)) return nullptr;
		auto cfg = rd<PortConfig *>(port, L->portConfig);
		return ptrOk(cfg) ? cfg : nullptr;
	}

	// Registry channel (`ioreg -l | grep HDMIB`); only touched when interrupts are enabled (never from locked hotplug leaves).
	static inline void setRoot(const char *key, const char *value) {
		auto root = IORegistryEntry::getRegistryRoot();
		if (root && ml_get_interrupts_enabled()) root->setProperty(key, value);
	}
	static void publishStatus() {
		if (!ml_get_interrupts_enabled()) return;
		char s[192];
		snprintf(s, sizeof(s), "hotplugs=%u edid-ok=%u edid-fail=%u dbuf-fixed=%u dc6-skipped=%u aux-blocked=%u cursor-blocked=%u fb=%d ddi=%u",
		         hotplugs, edidReads, edidFails, dbufFixed, dc6Skipped, auxBlocked, cursorBlocked, fbIdx, tddi);
		setRoot("HDMIB-status", s);
		// Self-diagnosis of the connector table: shows what the driver really has on the target DDI.
		auto cfg = getPortByDDI ? configOf(getPortByDDI(tddi)) : nullptr;
		if (cfg) {
			char c[160];
			snprintf(c, sizeof(c), "ddi=%u gmbus=%u class=%u type=0x%x flags=0x%x %s", cfg->ddi, cfg->gmbus, cfg->cls, cfg->type, cfg->flags,
			         (cfg->type & 0x800) ? "OK (HDMI)" : "NOT HDMI: framebuffer-patch0 missing or does not match - module idle");
			setRoot("HDMIB-port", c);
		}
	}

	static inline bool isTarget(void *port) { auto c = configOf(port); return c && c->ddi == tddi && (c->type & 0x800); }  // 0x800 = HDMI connector type: a real DP port on this DDI is never touched
	static inline bool isTargetFb(void *fb) { return L && ptrOk(fb) && rd<uint32_t>(fb, L->fbIndex) == (uint32_t)fbIdx; }

	// Apple keeps power wells 2 and 3 down while only the internal panel is lit; GMBUS and DDI B need them.
	static void ensurePW() {
		if (!gCtl || !readReg || !writeReg) return;
		uint32_t v = readReg(gCtl, REG_PWR_WELL_CTL2);
		// HSW-style layout: well n has STATE at bit 2n and REQUEST at bit 2n+1 (PW1..PW3 -> 0x3f)
		if ((v & 0x3f) != 0x3f) {
			writeReg(gCtl, REG_PWR_WELL_CTL2, v | 0x2a);
			for (int i = 0; i < 100; i++) {
				if ((readReg(gCtl, REG_PWR_WELL_CTL2) & 0x15) == 0x15) break;
				IOSleep(1);
			}
		}
	}

	// Apple allocates pipe B / part of pipe A display buffer (DDB) in DBUF slice 2 (blocks >= 1024) but never powers
	// slice 2 (DBUF_CTL_S2 stays 0): both planes then fetch garbage and underrun.
	static void ensureDbuf2(void *ctl) {
		if (!readReg || !writeReg || !ctl) return;
		uint32_t s1 = readReg(ctl, REG_DBUF_CTL_S1);
		uint32_t s2 = readReg(ctl, REG_DBUF_CTL_S2);
		if ((s2 & 0xc0000000u) != 0xc0000000u) {
			writeReg(ctl, REG_DBUF_CTL_S2, (s1 & 0x3fffffffu) | 0x80000000u);
			for (int i = 0; i < 500; i++) {
				if (readReg(ctl, REG_DBUF_CTL_S2) & 0x40000000u) break;
				IODelay(10);
			}
			dbufFixed++;
		}
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// EDID: GMBUS (with a Linux-style controller reset after a NAK) and a GPIO bit-bang fallback.
	// ---------------------------------------------------------------------------------------------------------------------
	static uint8_t  liveEdid[256];
	static bool     liveEdidOk = false;
	static uint32_t gmPin = 0xff, gmMode = 0, gmBlocks = 0;
	extern "C" uint64_t mach_absolute_time(void);

	static void gmbusReset(void *ctl) {
		writeReg(ctl, REG_GMBUS1, 1u << 31);    // GMBUS_SW_CLR_INT
		writeReg(ctl, REG_GMBUS1, 0);
		writeReg(ctl, REG_GMBUS0, 0);
		writeReg(ctl, REG_GMBUS2, 0x8000);      // drop INUSE
	}
	static bool gmbusWaitFlag(void *ctl, uint32_t mask, bool wantSet) {
		for (int i = 0; i < 400; i++) {         // 10 ms max per wait
			uint32_t v = readReg(ctl, REG_GMBUS2);
			if (v & 0x400) return false;        // NAK
			if (wantSet ? (v & mask) : !(v & mask)) return true;
			IODelay(25);
		}
		return false;                           // timeout
	}
	static void gmbusStop(void *ctl) {
		writeReg(ctl, REG_GMBUS1, (4u << 25) | (1u << 30));   // CYCLE_STOP | SW_RDY
		gmbusWaitFlag(ctl, 0x200, false);                     // wait !ACTIVE
		writeReg(ctl, REG_GMBUS0, 0);
		writeReg(ctl, REG_GMBUS2, 0x8000);
	}
	// read `len` bytes from slave 0x50 starting at `off`; method 0 = index read, 1 = write offset + read
	static bool gmbusReadChunk(void *ctl, uint32_t pin, uint32_t off, uint8_t *out, uint32_t len, int method) {
		gmbusReset(ctl);
		writeReg(ctl, REG_GMBUS4, 0);                       // no irq
		writeReg(ctl, REG_GMBUS5, 0);                       // no 2-byte index
		writeReg(ctl, REG_GMBUS0, pin);                     // pin pair, 100 kHz
		if (method == 1) {
			writeReg(ctl, REG_GMBUS3, off & 0xff);          // offset byte
			writeReg(ctl, REG_GMBUS1, (1u << 25) | (1u << 16) | (0x50u << 1) | (1u << 30)); // WAIT, 1 byte, write, SW_RDY
			if (!gmbusWaitFlag(ctl, 0x800, true)) { gmbusReset(ctl); return false; }
			writeReg(ctl, REG_GMBUS1, (1u << 25) | (len << 16) | (0x50u << 1) | 1u | (1u << 30));
		} else {
			writeReg(ctl, REG_GMBUS1, (2u << 25) | (1u << 25) | ((off & 0xff) << 8) | (len << 16) | (0x50u << 1) | 1u | (1u << 30));
		}
		for (uint32_t got = 0; got < len; ) {
			if (!gmbusWaitFlag(ctl, 0x800, true)) { gmbusReset(ctl); return false; }
			uint32_t v = readReg(ctl, REG_GMBUS3);
			for (int k = 0; k < 4 && got < len; k++) { out[got++] = v & 0xff; v >>= 8; }
		}
		gmbusStop(ctl);
		return true;
	}
	// GPIO bit-bang I2C (i915 intel_gpio_algo style). pin 1..3 -> GPIOB..GPIOD
	static unsigned long gR = 0;
	static unsigned long gpioRegFor(uint32_t pin) { return REG_GPIO_BASE + 4 * pin; }
	static void gSCL(void *ctl, int hi) { uint32_t keep = readReg(ctl, gR) & ((1u << 13) | (1u << 5)); writeReg(ctl, gR, keep | (hi ? 0x1u : 0x7u)); }
	static void gSDA(void *ctl, int hi) { uint32_t keep = readReg(ctl, gR) & ((1u << 13) | (1u << 5)); writeReg(ctl, gR, keep | (hi ? 0x100u : 0x700u)); }
	static int gGetSDA(void *ctl) { return (readReg(ctl, gR) & (1u << 12)) != 0; }
	static int gGetSCL(void *ctl) { return (readReg(ctl, gR) & (1u << 4)) != 0; }
	static void gSclHigh(void *ctl) {
		gSCL(ctl, 1);
		for (int i = 0; i < 500 && !gGetSCL(ctl); i++) IODelay(10);   // clock stretching
		IODelay(10);
	}
	static void gStart(void *ctl) { gSDA(ctl, 1); gSclHigh(ctl); gSDA(ctl, 0); IODelay(10); gSCL(ctl, 0); IODelay(10); }
	static void gStop(void *ctl) { gSDA(ctl, 0); IODelay(10); gSclHigh(ctl); gSDA(ctl, 1); IODelay(20); }
	static bool gWrite(void *ctl, uint8_t b) {
		for (int i = 7; i >= 0; i--) { gSDA(ctl, (b >> i) & 1); IODelay(5); gSclHigh(ctl); gSCL(ctl, 0); IODelay(5); }
		gSDA(ctl, 1); IODelay(5); gSclHigh(ctl);
		bool ack = !gGetSDA(ctl);
		gSCL(ctl, 0); IODelay(5);
		return ack;
	}
	static uint8_t gRead(void *ctl, bool ack) {
		uint8_t v = 0;
		gSDA(ctl, 1);
		for (int i = 0; i < 8; i++) { gSclHigh(ctl); v = (uint8_t)((v << 1) | (gGetSDA(ctl) ? 1 : 0)); gSCL(ctl, 0); IODelay(5); }
		gSDA(ctl, ack ? 0 : 1); IODelay(5); gSclHigh(ctl); gSCL(ctl, 0); IODelay(5); gSDA(ctl, 1);
		return v;
	}
	static bool gpioReadBlock(void *ctl, uint32_t pin, uint32_t off, uint8_t *out, uint32_t len) {
		gR = gpioRegFor(pin);
		gmbusReset(ctl);
		writeReg(ctl, REG_GMBUS4, 0);
		gSDA(ctl, 1); gSCL(ctl, 1); IODelay(50);
		gStart(ctl);
		if (!gWrite(ctl, 0x50 << 1)) { gStop(ctl); return false; }
		if (!gWrite(ctl, (uint8_t)off)) { gStop(ctl); return false; }
		gStart(ctl);
		if (!gWrite(ctl, (0x50 << 1) | 1)) { gStop(ctl); return false; }
		for (uint32_t i = 0; i < len; i++) out[i] = gRead(ctl, i + 1 < len);
		gStop(ctl);
		return true;
	}
	static bool edidBlockValid(const uint8_t *b) {
		uint8_t sum = 0;
		for (int i = 0; i < 128; i++) sum += b[i];
		return sum == 0;
	}
	static bool edidHeaderOk(const uint8_t *b) { return b[0] == 0 && b[1] == 0xff && b[7] == 0; }
	static bool tryBlock(void *ctl, uint32_t pin, int method, uint32_t off, uint8_t *dst) {
		bool ok = (method <= 1) ? gmbusReadChunk(ctl, pin, off, dst, 128, method) : gpioReadBlock(ctl, pin, off, dst, 128);
		return ok && edidBlockValid(dst);
	}
	// GMBUS pin pair for the target port: the platform table carries it (cfg->gmbus); otherwise DDI A/B/C -> pins 1/2/3.
	static uint32_t primaryPin() {
		const PortConfig *cfg = getPortByDDI ? configOf(getPortByDDI(tddi)) : nullptr;
		uint32_t p = cfg ? cfg->gmbus : 0;
		if (p >= 1 && p <= 12) return p;   // GMBUS pins 1..12 (ICL: 1-3 combo, 9-12 type-C)
		p = tddi + 1;
		return (p >= 1 && p <= 3) ? p : 2;
	}
	static bool readLiveEdid(void *ctl) {
		if (!readReg || !writeReg || !ctl) return false;
		ensurePW();
		uint8_t tmp[128];
		if (liveEdidOk) {
			// Already known (wake from sleep, screen saver...): a re-hotplug must not block the framebuffer thread. One quick
			// revalidation; on failure keep the cached EDID; a different monitor falls through to the full read.
			uint32_t pin = gmPin <= 3 ? gmPin : primaryPin();
			bool ok = tryBlock(ctl, pin, 0, 0, tmp);
			if (!ok && gmMode >= 1) ok = tryBlock(ctl, pin, (int)gmMode, 0, tmp);
			if (ok && edidHeaderOk(tmp)) {
				for (int i = 0; i < 128; i++) if (tmp[i] != liveEdid[i]) { liveEdidOk = false; break; }
			}
			if (liveEdidOk) return true;
		}
		liveEdidOk = false;
		uint32_t first = primaryPin(), pins[3], n = 0;
		pins[n++] = first;
		for (uint32_t p = 1; p <= 3; p++) if (p != first) pins[n++] = p;
		for (int method = 0; method <= 2 && !liveEdidOk; method++) {          // 0,1 = GMBUS, 2 = GPIO bit-bang
			unsigned npins = (method < 2) ? 1 : 3;                            // GMBUS only on the platform pin, GPIO tries all
			for (unsigned pi = 0; pi < npins && !liveEdidOk; pi++) {
				for (int attempt = 0; attempt < 2 && !liveEdidOk; attempt++) {
					if (tryBlock(ctl, pins[pi], method, 0, tmp)) {
						if (!edidHeaderOk(tmp)) continue;
						gmPin = pins[pi]; gmMode = method; gmBlocks = 1;
						for (int i = 0; i < 128; i++) liveEdid[i] = tmp[i];
						if (tmp[126] >= 1) {
							uint8_t t2[128];
							if (tryBlock(ctl, pins[pi], method, 128, t2)) {
								for (int i = 0; i < 128; i++) liveEdid[128 + i] = t2[i];
								gmBlocks = 2;
							}
						}
						liveEdidOk = true;
					} else {
						IODelay(2000);
					}
				}
			}
		}
		if (liveEdidOk) edidReads++; else edidFails++;
		return liveEdidOk;
	}

	// Generic 1024x768@60 EDID (VESA DMT 0x10, 65 MHz), only used with igfxhdmib-edid-fallback=1 when the monitor does not answer.
	static uint8_t fallbackEdid[128];
	static bool fallbackBuilt = false;
	static void buildFallbackEdid() {
		if (fallbackBuilt) return;
		static const uint8_t base[128] = {
			0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,                       // header
			0x20, 0x8d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x24,          // manufacturer "HDM", product 1, week 1, year 2026
			0x01, 0x03,                                                           // EDID 1.3
			0x80, 0x1e, 0x17, 0x78, 0x02,                                         // digital, 30x23 cm, gamma 2.2, preferred timing
			0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54,          // chromaticity (sRGB)
			0x21, 0x08, 0x00,                                                     // 640x480@60, 800x600@60, 1024x768@60
			0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // standard timings: unused
			0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30, 0x18, 0x88, 0x36, 0x00, 0x30, 0xe4, 0x10, 0x00, 0x00, 0x18, // DTD 1024x768@60
			0x00, 0x00, 0x00, 0xfc, 0x00, 'H', 'D', 'M', 'I', ' ', 'F', 'a', 'l', 'l', 'b', 'k', 0x0a, 0x20,           // name "HDMI Fallbk"
			0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e, 0x3c, 0x08, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // range limits
			0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // dummy
			0x00, 0x00};                                                          // extension count, checksum
		uint8_t sum = 0;
		for (int i = 0; i < 127; i++) { fallbackEdid[i] = base[i]; sum += base[i]; }
		fallbackEdid[127] = (uint8_t)(0x100 - sum);
		fallbackBuilt = true;
	}
	// The 128-byte EDID block we answer with, or nullptr if we have none (then Apple's own answer stands).
	static const uint8_t *edidFor(uint32_t block) {
		if (liveEdidOk) {
			if (block == 1) return liveEdid;
			if (block == 2 && gmBlocks >= 2) return liveEdid + 128;
			return nullptr;
		}
		if (edidFallback && block == 1) { buildFallbackEdid(); return fallbackEdid; }
		return nullptr;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Sink detection (HDMI has no AUX channel: presence is HPD)
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t wrapProbePortState(void *that) {
		if (!isTarget(that)) return orgProbePortState(that);
		bool hpdLow = isHPDLow ? isHPDLow(that) : true;
		uint32_t state = hpdLow ? 0 : 1;
		wr<uint32_t>(that, L->portState, state);
		return state;
	}
	static int wrapSetDPPower(void *that, uint8_t state) {
		if (!isTarget(that)) return orgSetDPPower(that, state);
		return 0;                                  // HDMI sink has no AUX/DPCD: pretend the wake-up worked
	}
	static int wrapGetSinkCount(void *that, uint8_t *count) {
		if (!isTarget(that)) return orgGetSinkCount(that, count);
		if (count) *count = 1;
		return 0;
	}
	static int wrapGetDPCDInfo(void *that) {
		if (!isTarget(that)) return orgGetDPCDInfo(that);
		return 0;
	}
	static int wrapGetDDCBlock(void *that, uint32_t block, uint8_t *buf, bool flag) {
		if (!isTarget(that)) return orgGetDDCBlock(that, block, buf, flag);
		ensurePW();
		int ret = orgGetDDCBlock(that, block, buf, flag);
		if (ret != 0 && (block == 1 || block == 2) && buf) {
			// The sink did not answer through Apple's (DisplayPort-oriented) DDC path: hand over the EDID we read ourselves
			if (auto src = edidFor(block)) { for (int i = 0; i < 128; i++) buf[i] = src[i]; ret = 0; }
		}
		return ret;
	}
	static IOReturn wrapGetOnlineInfo(void *that, void *path, uint8_t *a, uint8_t *b) {
		auto ret = orgGetOnlineInfo(that, path, a, b);
		if (a && b && isTargetFb(that) && getPortByDDI && isHPDLow) {
			// The caller sets fb->flags = *online: report the HDMI sink as online together with its EDID
			auto prt = getPortByDDI(tddi);
			if (ptrOk(prt) && !isHPDLow(prt) && rd<uint32_t>(prt, L->portState) != 0) {
				if (auto src = edidFor(1)) {
					*a = 1;
					for (int i = 0; i < 128; i++) b[i] = src[i];
					ret = 0;
				}
			}
		}
		return ret;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Hotplug
	// ---------------------------------------------------------------------------------------------------------------------
	static void wrapHotplug(void *ctl, uint32_t ddi, uint32_t portMode);

	// If the cable is already plugged at boot Apple never calls invokeHotplugHandler for the port (no HPD edge). After a delay run
	// the same hotplug once, from a timer on the controller's own workloop (same context as Apple's interrupt handler).
	static bool bootArmed = false;
	static void bootDetectFire(OSObject *owner, IOTimerEventSource *) {
		void *ctl = owner;
		if (hotplugs != 0 || !getPortByDDI || !isHPDLow || !L) return;
		auto port = getPortByDDI(tddi);
		if (ptrOk(port) && !isHPDLow(port)) wrapHotplug(ctl, tddi, lastPortMode ? lastPortMode : 1);
	}
	static void armBootDetect(void *ctl) {
		if (bootArmed || bootDetectSec <= 0 || !ptrOk(ctl)) return;
		bootArmed = true;
		auto svc = reinterpret_cast<IOService *>(ctl);
		IOWorkLoop *wl = svc->getWorkLoop();
		if (!wl) { HLOG("boot detect: no workloop"); return; }
		IOTimerEventSource *t = IOTimerEventSource::timerEventSource(reinterpret_cast<OSObject *>(ctl), bootDetectFire);
		if (!t) { HLOG("boot detect: no timer"); return; }
		if (wl->addEventSource(t) != 0) { HLOG("boot detect: addEventSource failed"); return; }
		t->setTimeoutMS((UInt32)bootDetectSec * 1000u);
	}

	static void wrapHotplug(void *ctl, uint32_t ddi, uint32_t portMode) {
		bool tgt = (ddi == tddi);
		if (tgt) {
			hotplugs++;
			lastPortMode = portMode;
			gCtl = ctl;
			ensureDbuf2(ctl);
			readLiveEdid(ctl);
		}
		orgHotplug(ctl, ddi, portMode);
		if (!tgt) return;

		// Nobody tells IOFramebuffer about the connect on this port. Build what the AGDC client never builds for an injected
		// framebuffer and then do what Apple's own InjectHPD() does: deliver the connect interrupt.
		if (injectHPD && getPortByDDI && isHPDLow) {
			auto port = reinterpret_cast<uint8_t *>(getPortByDDI(tddi));
			if (ptrOk(port) && !isHPDLow(port) && (rd<uint32_t>(port, L->portState) == 2 || rd<uint32_t>(port, L->portState) == 1)) {
				uint32_t idx = rd<uint32_t>(port, L->portIndex);
				uint32_t nfb = rd<uint32_t>(ctl, L->ctlFbCount);
				if (idx < nfb && idx < 3) {
					auto fb = rd<uint8_t *>(ctl, L->ctlFbArray + 8 * idx);
					if (ptrOk(fb) && rd<uint32_t>(fb, L->fbIndex) == idx) {
						if (!fbIdxFixed && fbIdx != (int)idx) { HLOG("target framebuffer is %u (expected %d)", idx, fbIdx); fbIdx = (int)idx; }
						ensurePW();
						if (connChanged) connChanged(fb);                   // make the framebuffer re-probe the sink (EDID)
						if (orgCallbackAGDC) orgCallbackAGDC(ctl, 0, idx, 0);   // class-2 connects only emit event 1; the client expects event 0
						if (addDispPath) {
							// give the framebuffer a display path and an active flag ourselves (the AGDC client never commits it)
							auto cnt = reinterpret_cast<uint32_t *>(fb + L->fbPathCount);
							if (*cnt == 0) {
								ensurePW();
								if (allocPathRes) allocPathRes(fb, idx, 0);     // Apple's per-port allocator: path, port, scaler, plane, pipe
								else addDispPath(fb, 0xffff);
							}
							if (*cnt) {
								// addDisplayPath leaves path->port null; Apple's allocateDisplayPathResources sets it
								auto dpx = rd<uint8_t *>(fb, L->fbPath0);
								auto prt = getPortByDDI(tddi);
								if (ptrOk(dpx) && ptrOk(prt) && rd<void *>(dpx, L->pathPort) == nullptr) wr<void *>(dpx, L->pathPort, prt);
							}
							wr<uint8_t>(fb, L->fbFlags, rd<uint8_t>(fb, L->fbFlags) | 1);
							// resource-configuration table (count + entry 0), cloned from the internal framebuffer's committed one,
							// so setupDisplayTiming() can resolve the entry index it gets back from the client
							auto fb0 = rd<uint8_t *>(ctl, L->ctlFbArray);
							if (ptrOk(fb0) && fb0 != fb && rd<uint32_t>(fb, L->fbCfgTable) == 0)
								for (uint32_t i = 0; i < L->fbCfgTableLen; i++) fb[L->fbCfgTable + i] = fb0[L->fbCfgTable + i];
						}
						injectHPD(ctl, fb);
					}
				}
			}
		}
		publishStatus();
	}

	static uint32_t wrapLightUpTMDS(void *ctl, void *fb, void *path, void *timing) {
		if (isTargetFb(fb)) ensureDbuf2(ctl);
		return orgLightUpTMDS(ctl, fb, path, timing);
	}
	static uint32_t wrapHwSetMode(void *ctl, void *fb, void *path, int m, void *timing) {
		armBootDetect(ctl);
		if (isTargetFb(fb)) ensureDbuf2(ctl);
		return orgHwSetMode(ctl, fb, path, m, timing);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// AGDC (accelerator <-> display driver) callbacks and AUX
	// ---------------------------------------------------------------------------------------------------------------------
	static bool wrapCallbackAGDC(void *c, uint32_t ev, unsigned long port, uint32_t arg) {
		if ((ev == 10 || ev == 12) && port > 0x1000 && ptrOk(reinterpret_cast<void *>(port)) &&
			*reinterpret_cast<uint32_t *>(port) == (uint32_t)fbIdx) {
			if (ev == 10) return false;            // validateDetailedTiming: the client has no configuration for this framebuffer,
			                                       // answer "no opinion" and let the kext's own validation decide
			// kAGDCRegisterLinkModesetCallback: the client would pick the resource-config entry for this mode. The injected
			// framebuffer has exactly one (cloned) entry: index 0.
			wr<int32_t>(reinterpret_cast<void *>(port), L->agdcCfgIndex, 0);
			return true;
		}
		return orgCallbackAGDC(c, ev, port, arg);
	}
	// HDMI has no AUX channel, but the accelerator keeps reading DPCD through GetStreamAccessAUX on the HDMI stream (AGDC port =
	// framebuffer index + 1). Every read times out inside the controller (thousands per minute) and stalls the system for a minute
	// after plugging. Fail them at once with the error Apple returns after the timeout.
	static IOReturn wrapStreamAux(void *c, void *in, void *out) {
		uint32_t port = (in && ptrOk(in)) ? *reinterpret_cast<uint32_t *>(in) : 0xffff;
		if (auxBlockEnabled && port == (uint32_t)fbIdx + 1 && out && ptrOk(out)) {
			*reinterpret_cast<uint16_t *>(reinterpret_cast<uint8_t *>(out) + 0xe) = 2;
			auxBlocked++;
			return (IOReturn)0xe00002bc;
		}
		return orgStreamAux(c, in, out);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Hardware cursor. The injected framebuffer has no cursor memory (fb+fbCursorMem never set up): Apple's code then faults in
	// hwSetCursorImage. With igfxhdmib-hwcursor=1 we call Apple's own hwUpdateCursorMemory() once and check the buffers lie inside
	// the controller's cursor region; if anything looks wrong we refuse the hardware cursor and IOFramebuffer falls back to software.
	// ---------------------------------------------------------------------------------------------------------------------
	static bool fbHwCursorOk(void *ctl, void *fb) {
		if (!hwCursorFb || swCursorAll || !orgUpdateCursorMem || !ptrOk(ctl) || !ptrOk(fb)) return false;
		uint32_t a0 = rd<uint32_t>(fb, L->fbCursorMem), a1 = rd<uint32_t>(fb, L->fbCursorMem + 4);
		if (a0 == 0 && a1 == 0) {
			auto pth = rd<uint8_t *>(fb, rd<uint32_t>(fb, L->fbPathSel) == 0xffffffffu ? L->fbPath0 : L->fbPathAlt);
			if (!ptrOk(pth)) return false;
			orgUpdateCursorMem(ctl, fb);
			a0 = rd<uint32_t>(fb, L->fbCursorMem); a1 = rd<uint32_t>(fb, L->fbCursorMem + 4);
		}
		uint64_t base = rd<uint64_t>(ctl, L->ctlCursorPhys);
		uint32_t size = rd<uint32_t>(ctl, L->ctlCursorSize);
		uint64_t cpu = rd<uint64_t>(ctl, L->ctlCursorCpu);
		uint32_t o0 = a0 - (uint32_t)base, o1 = a1 - (uint32_t)base;
		return cpu != 0 && size != 0 && o0 < size && o1 < size && (uint64_t)o0 + 0x40000 <= size && (uint64_t)o1 + 0x40000 <= size;
	}
	static bool cursorBlockedFor(void *ctl, void *fb) {
		if (!ptrOk(fb)) return false;
		bool tgt = isTargetFb(fb);
		if (tgt && fbHwCursorOk(ctl, fb)) return false;
		return tgt || swCursorAll != 0;
	}
	static uint32_t wrapHwSetCursorImage(void *ctl, void *fb, void *info) {
		if (cursorBlockedFor(ctl, fb)) { if (++cursorBlocked == 1) HLOG("hardware cursor refused on the HDMI framebuffer (software cursor)"); return 0xe00002c7; }
		return orgHwSetCursorImage(ctl, fb, info);
	}
	static uint32_t wrapHwSetCursorState(void *ctl, void *fb, int a, int b, bool c, bool d) {
		if (cursorBlockedFor(ctl, fb)) { cursorBlocked++; return 0xe00002c7; }
		return orgHwSetCursorState(ctl, fb, a, b, c, d);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Work-arounds
	// ---------------------------------------------------------------------------------------------------------------------
	// The HDMI timing generates VRR errors every ~11 s: no VRR on that framebuffer.
	static void wrapInitVRRCaps(void *fb) {
		orgInitVRRCaps(fb);
		if (isTargetFb(fb)) { for (uint32_t i = 0; i < 5; i++) wr<uint8_t>(fb, L->fbVrr + i, 0); vrrOff++; }
	}
	// Apple panics ("[IGFB][PANIC][POWER] DC6 is already enabled - bailing out") when enableHWDC6() runs while its reference count is
	// already > 0, i.e. two enables without a disable in between. This happens from setDisplayMode -> disableDisplay during repeated
	// HDMI hot-plugging. Skipping the extra enable leaves DC6 enabled by the first one; a later disable turns it off.
	static void wrapEnableHWDC6(void *c) {
		if (dc6FixEnabled && ptrOk(c) && rd<int32_t>(c, L->ctlDC6Count) > 0) { dc6Skipped++; return; }
		orgEnableHWDC6(c);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Installation. Called from IGFX::processKext for the framebuffer kext.
	// ---------------------------------------------------------------------------------------------------------------------
	static inline int intArg(const char *name, int def) { int v = def; PE_parse_boot_argn(name, &v, sizeof(v)); return v; }

	template <typename F> static inline void optRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size, const char *sym, F wrap, F &org, const char *what) {
		KernelPatcher::RouteRequest rq(sym, wrap, org);
		if (!patcher.routeMultiple(index, &rq, 1, address, size)) HLOG_ALWAYS("optional hook %s could not be routed", what);
	}

	static inline void install(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (!checkKernelArgument("-igfxhdmib")) return;
		if (kextIntelICLLPFb.loadIndex != index) return;      // only the Ice Lake LP framebuffer kext
		HLOG_ALWAYS("v%s starting on Darwin %u", Version, (unsigned)getKernelVersion());

		const LayoutEntry *le = nullptr;
		for (auto &e : layouts) if (e.darwin == (uint32_t)getKernelVersion()) { le = &e; break; }
		if (!le) {
			HLOG_ALWAYS("Darwin %u has no known structure layout: module idle", (unsigned)getKernelVersion());
			setRoot("HDMIB-layout", "unknown-darwin: idle");
			return;
		}
		if (!le->verified && intArg("igfxhdmib-force", 0) == 0) {
			HLOG_ALWAYS("layout for %s is not verified: module idle (igfxhdmib-force=1 to run anyway at your own risk)", le->name);
			setRoot("HDMIB-layout", "unverified: idle");
			return;
		}
		L = &le->l;
		HLOG_ALWAYS("layout: %s%s", le->name, le->verified ? "" : " (UNVERIFIED, forced)");
		setRoot("HDMIB-layout", le->verified ? le->name : "unverified: forced");

		setRoot("HDMIB-version", Version);
		{ int d = intArg("igfxhdmib-ddi", 1); tddi = (uint32_t)((d >= 0 && d <= 5) ? d : 1); }
		{ int f = intArg("igfxhdmib-fb", -1); if (f >= 0 && f < 3) { fbIdx = f; fbIdxFixed = true; } }
		bootDetectSec = intArg("igfxhdmib-bootdetect", 3);
		if (bootDetectSec < 0 || bootDetectSec > 60) bootDetectSec = 3;
		hwCursorFb = intArg("igfxhdmib-hwcursor", 1);
		swCursorAll = intArg("igfxhdmib-swcursor", 0);
		auxBlockEnabled = intArg("igfxhdmib-aux", 1);
		dc6FixEnabled = intArg("igfxhdmib-dc6fix", 1);
		edidFallback = intArg("igfxhdmib-edid-fallback", 0);

		auto solve = [&](const char *sym) { return patcher.solveSymbol(index, sym, address, size); };
		auto gp = solve("__ZN14AppleIntelPort12getPortByDDIEj");
		auto hp = solve("__ZN14AppleIntelPort8isHPDLowEv");
		auto rr = solve("__ZN31AppleIntelFramebufferController14ReadRegister32Em");
		auto wrg = solve("__ZN31AppleIntelFramebufferController15WriteRegister32Emj");
		auto cc = solve("__ZN21AppleIntelFramebuffer17connectionChangedEv");
		auto adp = solve("__ZN21AppleIntelFramebuffer14addDisplayPathEj");
		auto apr = solve("__ZN21AppleIntelFramebuffer28allocateDisplayPathResourcesEjj");
		auto ihpd = solve("__ZN31AppleIntelFramebufferController9InjectHPDEP21AppleIntelFramebuffer");
		auto uc = solve("__ZN31AppleIntelFramebufferController20hwUpdateCursorMemoryEP21AppleIntelFramebuffer");
		if (!gp || !hp || !rr || !wrg || !ihpd) {
			HLOG_ALWAYS("required symbols missing (getPortByDDI=%d isHPDLow=%d ReadRegister32=%d WriteRegister32=%d InjectHPD=%d): module idle",
			            gp != 0, hp != 0, rr != 0, wrg != 0, ihpd != 0);
			setRoot("HDMIB-hooks", "symbols-missing: idle");
			return;
		}
		getPortByDDI = reinterpret_cast<void *(*)(uint32_t)>(gp);
		isHPDLow = reinterpret_cast<bool (*)(void *)>(hp);
		readReg = reinterpret_cast<uint32_t (*)(void *, unsigned long)>(rr);
		writeReg = reinterpret_cast<void (*)(void *, unsigned long, uint32_t)>(wrg);
		injectHPD = reinterpret_cast<void (*)(void *, void *)>(ihpd);
		if (cc) connChanged = reinterpret_cast<uint32_t (*)(void *)>(cc);
		if (adp) addDispPath = reinterpret_cast<void *(*)(void *, uint32_t)>(adp);
		if (apr) allocPathRes = reinterpret_cast<uint8_t (*)(void *, uint32_t, uint32_t)>(apr);
		if (uc) orgUpdateCursorMem = reinterpret_cast<void (*)(void *, void *)>(uc);

		// Core hooks: without all of them the port cannot work, so they are routed as one group.
		KernelPatcher::RouteRequest core[] = {
			{"__ZN14AppleIntelPort14probePortStateEv", wrapProbePortState, orgProbePortState},
			{"__ZN21AppleIntelFramebuffer13getOnlineInfoEP21AppleIntelDisplayPathPhS2_", wrapGetOnlineInfo, orgGetOnlineInfo},
			{"__ZN31AppleIntelFramebufferController20invokeHotplugHandlerEjN14AppleIntelPort8PortModeE", wrapHotplug, orgHotplug},
			{"__ZN14AppleIntelPort15setDPPowerStateEh", wrapSetDPPower, orgSetDPPower},
			{"__ZN14AppleIntelPort14getDPSinkCountEPh", wrapGetSinkCount, orgGetSinkCount},
			{"__ZN14AppleIntelPort11getDPCDInfoEv", wrapGetDPCDInfo, orgGetDPCDInfo},
			{"__ZN14AppleIntelPort11getDDCBlockEjPhb", wrapGetDDCBlock, orgGetDDCBlock},
			{"__ZN31AppleIntelFramebufferController18GetStreamAccessAUXEP21AGDCStreamAccessAUX_tS1_", wrapStreamAux, orgStreamAux},
			{"__ZN31AppleIntelFramebufferController12CallBackAGDCE31kAGDCRegisterLinkControlEvent_tmj", wrapCallbackAGDC, orgCallbackAGDC},
		};
		if (!patcher.routeMultiple(index, core, arrsize(core), address, size)) {
			HLOG_ALWAYS("failed to route the core hooks (error %d): HDMI will not work", (int)patcher.getError());
			setRoot("HDMIB-hooks", "core-route-failed");
			return;
		}
		// Hooks that improve behaviour but are not essential: a failure of one does not stop the others.
		optRoute(patcher, index, address, size, "__ZN31AppleIntelFramebufferController11LightUpTMDSEP21AppleIntelFramebufferP21AppleIntelDisplayPathPK29IODetailedTimingInformationV2", wrapLightUpTMDS, orgLightUpTMDS, "LightUpTMDS");
		optRoute(patcher, index, address, size, "__ZN31AppleIntelFramebufferController9hwSetModeEP21AppleIntelFramebufferP21AppleIntelDisplayPathiPK29IODetailedTimingInformationV2", wrapHwSetMode, orgHwSetMode, "hwSetMode");
		optRoute(patcher, index, address, size, "__ZN31AppleIntelFramebufferController16hwSetCursorImageEP21AppleIntelFramebufferP20IOHardwareCursorInfo", wrapHwSetCursorImage, orgHwSetCursorImage, "hwSetCursorImage");
		optRoute(patcher, index, address, size, "__ZN31AppleIntelFramebufferController16hwSetCursorStateEP21AppleIntelFramebufferiibb", wrapHwSetCursorState, orgHwSetCursorState, "hwSetCursorState");
		optRoute(patcher, index, address, size, "__ZN21AppleIntelFramebuffer11initVRRCapsEv", wrapInitVRRCaps, orgInitVRRCaps, "initVRRCaps");
		if (dc6FixEnabled)
			optRoute(patcher, index, address, size, "__ZN31AppleIntelFramebufferController11enableHWDC6Ev", wrapEnableHWDC6, orgEnableHWDC6, "enableHWDC6");

		HLOG_ALWAYS("hooks installed (ddi=%u fb=%d bootdetect=%ds hwcursor=%d)", tddi, fbIdx, bootDetectSec, hwCursorFb);
		setRoot("HDMIB-hooks", "installed");
		publishStatus();
	}

} // namespace HDMIB

#endif /* kern_igfx_hdmib_hpp */

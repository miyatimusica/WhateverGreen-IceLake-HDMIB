#!/bin/bash
# hdmib_diag.sh - recoge todo lo necesario para saber por que el modulo HDMIB no enciende el HDMI. Solo LEE; no cambia nada.
# Uso: 1) arranca con el cable HDMI puesto (o enchufalo y espera 15 s)  2) bash hdmib_diag.sh  3) envia ~/Desktop/hdmib_diag.txt
# --- contrasena al inicio, siempre; se mantiene viva hasta que termine el script ---
sudo -v || { echo "Se necesita la contrasena de administrador."; exit 1; }
( while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) &
KEEPSUDO=$!
trap 'kill $KEEPSUDO 2>/dev/null' EXIT
# ------------------------------------------------------------------------------------
OUT="$HOME/Desktop/hdmib_diag.txt"; : > "$OUT"
s() { echo; echo "##### $*" | tee -a "$OUT" >/dev/null; }
{
echo "=== hdmib_diag $(date) ==="
echo "macOS $(sw_vers -productVersion) build $(sw_vers -buildVersion)  Darwin $(uname -r)  $(sysctl -n hw.model)"
s "boot-args realmente activos"; nvram boot-args 2>&1; sysctl -n kern.bootargs 2>&1
s "kexts cargados (WhateverGreen / Lilu)"; kextstat 2>/dev/null | grep -iE "whatevergreen|lilu"; kmutil showloaded 2>/dev/null | grep -iE "whatevergreen|lilu"
s "estado del modulo (ioreg)"; ioreg -lw0 | grep -E "HDMIB" | sed 's/^[ |+-]*//'
s "iGPU: platform-id / device-id / patches inyectados"; ioreg -lw0 -n IGPU 2>/dev/null | grep -E "ig-platform-id|\"device-id\"|framebuffer-patch|framebuffer-stolenmem|framebuffer-fbmem|framebuffer-unifiedmem|enable-dbuf|override-no-connect" | cut -c1-220
s "framebuffers y pantallas"; ioreg -lw0 -c AppleIntelFramebuffer | grep -E "AppleIntelFramebuffer@|IOFBCurrentPixelCount|\"connector-type\"|display0|AppleDisplay |IODisplayEDID\"|IOFBCurrentPixelClock" | cut -c1-200
s "pantallas segun CoreGraphics"
T=$(mktemp -d /tmp/hd.XXXX); cat > "$T/d.m" <<'M'
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
int main(void){ CGDirectDisplayID d[16]; uint32_t n=0; CGGetOnlineDisplayList(16,d,&n); for(uint32_t i=0;i<n;i++){ CGRect b=CGDisplayBounds(d[i]); printf("id %u %.0fx%.0f @ %.0f,%.0f builtin=%d active=%d asleep=%d vendor=%x model=%x\n",d[i],b.size.width,b.size.height,b.origin.x,b.origin.y,CGDisplayIsBuiltin(d[i]),CGDisplayIsActive(d[i]),CGDisplayIsAsleep(d[i]),CGDisplayVendorNumber(d[i]),CGDisplayModelNumber(d[i])); } printf("online=%u\n",n); return 0; }
M
clang -x objective-c "$T/d.m" -framework Foundation -framework CoreGraphics -o "$T/d" 2>&1 && "$T/d"; rm -rf "$T"
s "log del modulo desde el arranque (HDMIB)"; log show --last boot --predicate 'eventMessage CONTAINS "HDMIB"' --style compact 2>/dev/null | cut -c1-260 | head -150
s "log del driver grafico desde el arranque (hotplug / ddi / IGFB)"; log show --last boot --predicate 'eventMessage CONTAINS "Hotplug" OR eventMessage CONTAINS "unsupported ddi" OR eventMessage CONTAINS "IGFB" OR eventMessage CONTAINS "AppleIntelFramebuffer"' --style compact 2>/dev/null | cut -c1-260 | tail -80
s "paneles/kernel panic recientes"; ls -t /Library/Logs/DiagnosticReports/*.panic /Library/Logs/DiagnosticReports/Retired/*.panic 2>/dev/null | head -3
} >> "$OUT" 2>&1
echo "Listo: $OUT   (envialo)"

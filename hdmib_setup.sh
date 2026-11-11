#!/bin/bash
# hdmib_setup.sh - macOS-only helper for the HDMIB module (no Linux needed).
#   ./hdmib_setup.sh check              diagnose the running system (read-only)
#   ./hdmib_setup.sh patch  [config]    add the DeviceProperties + boot-arg to an OpenCore config.plist (backup first)
#   ./hdmib_setup.sh verify <kext>      check that a kext file is the HDMIB build (not an old one)
[ -n "$BASH_VERSION" ] || exec bash "$0" "$@"
set -u
sudo -v || exit 1
( while true; do sudo -n true; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) 2>/dev/null &
PB=/usr/libexec/PlistBuddy
IGPU='Device/PciRoot(0x0)/Pci(0x2,0x0)'
cmd=${1:-check}

pid=$(ioreg -rd1 -c IOPCIDevice -n IGPU 2>/dev/null | awk -F'[<>]' '/AAPL,ig-platform-id/{print $2}' | head -1)
[ -z "$pid" ] && pid=$(ioreg -l | awk -F'[<>]' '/AAPL,ig-platform-id/{print $2; exit}')

case $cmd in
check)
  echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(sysctl -n hw.model)"
  echo "platform-id (little-endian bytes): ${pid:-?}"
  echo "--- HDMIB keys ---"; ioreg -l | grep -E 'HDMIB-(version|layout|hooks|status|port)'
  echo "--- boot-args ---"; nvram boot-args 2>/dev/null; sysctl -n kern.bootargs 2>/dev/null
  echo "--- displays ---"; system_profiler SPDisplaysDataType | grep -E 'Resolution|Display Type|Connection|Online'
  ioreg -l | grep -q 'HDMIB-version' || echo "!! No HDMIB-version: the OLD kext (or none) is loaded. Check EFI/OC/Kexts/WhateverGreen.kext and OC/config.plist Kernel>Add."
  ioreg -l | grep 'HDMIB-port' | grep -q 'NOT HDMI' && echo "!! Connector table is not HDMI: run './hdmib_setup.sh patch' and reboot."
  ;;
patch)
  cfg=${2:-}
  [ -f "$cfg" ] || { echo "usage: $0 patch /Volumes/EFI/EFI/OC/config.plist  (mount the EFI first, e.g. with MountEFI)"; exit 1; }
  case $pid in 01005c8a) : ;; *) echo "Platform-id is '$pid'. The preset below is for 01005c8a (ICL G1, ddi3/pin10 DP -> ddi1/pin2 HDMI)."
     echo "Other platform-ids: not verified. Continue only if you know the 24-byte connector entry (see README, 'Porting')."; read -r -p "Continue with the preset anyway? [y/N] " a; [ "$a" = y ] || exit 1;; esac
  b="$cfg.bak.$(date +%Y%m%d-%H%M%S)"; cp "$cfg" "$b" && echo "backup: $b"
  P="DeviceProperties:Add:$IGPU"
  $PB -c "Print :DeviceProperties:Add" "$cfg" >/dev/null 2>&1 || { echo "config.plist has no DeviceProperties>Add"; exit 1; }
  $PB -c "Print :$P" "$cfg" >/dev/null 2>&1 || $PB -c "Add :$P dict" "$cfg"
  FIND_HEX=03000000.0a000000.01000000.01000000.00040000.c1020000
  REPL_HEX=01000000.02000000.02000000.00000000.00080000.c1020000
  FIND_HEX=${FIND_HEX//./}; REPL_HEX=${REPL_HEX//./}
  b64() { printf '%s' "$1" | xxd -r -p | base64; }
  tmp=$(mktemp /tmp/hdmib.XXXXXX.plist)
  cat > "$tmp" <<EOP
<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>
<key>framebuffer-patch-enable</key><data>$(b64 01000000)</data>
<key>framebuffer-patch0-enable</key><data>$(b64 01000000)</data>
<key>framebuffer-patch0-find</key><data>$(b64 $FIND_HEX)</data>
<key>framebuffer-patch0-replace</key><data>$(b64 $REPL_HEX)</data>
</dict></plist>
EOP
  $PB -c "Merge $tmp :$P" "$cfg" && echo "DeviceProperties merged into $IGPU"
  rm -f "$tmp"
  cur=$($PB -c "Print :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args" "$cfg" 2>/dev/null)
  case " $cur " in *" -igfxhdmib "*) echo "boot-arg -igfxhdmib already present";;
    *) $PB -c "Set :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args $cur -igfxhdmib" "$cfg" && echo "boot-arg added";; esac
  echo "Now copy WhateverGreen-HDMIB.kext over EFI/OC/Kexts/WhateverGreen.kext and reboot. Then: $0 check"
  ;;
verify)
  k=${2:-}; bin=$(ls "$k"/Contents/MacOS/* 2>/dev/null | head -1)
  [ -f "$bin" ] || { echo "usage: $0 verify /path/WhateverGreen.kext"; exit 1; }
  if strings "$bin" | grep -q 'HDMIB-version'; then echo "OK: new build ($(strings "$bin" | grep -m1 -E '^1\.0-rc'))"; else echo "OLD or stock kext (no HDMIB-version)"; fi
  ;;
*) echo "usage: $0 check|patch <config.plist>|verify <kext>";;
esac

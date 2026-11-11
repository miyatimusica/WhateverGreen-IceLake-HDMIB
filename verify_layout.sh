#!/bin/bash
# verify_layout.sh - comprueba si las estructuras privadas del driver de ESTA version de macOS coinciden con las del modulo HDMIB.
# Uso (en la carpeta del modulo):  bash verify_layout.sh      -> escribe ~/Desktop/hdmib_verify.txt y ~/Desktop/hdmib_verify.zip
# Solo LEE el kernel collection y no modifica nada del sistema. Necesita internet una vez (pip install capstone, en un entorno temporal).
[ -n "$BASH_VERSION" ] || exec bash "$0" "$@"
# --- contrasena al inicio, siempre; se mantiene viva hasta que termine el script ---
sudo -v || { echo "Se necesita la contrasena de administrador."; exit 1; }
( while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) &
KEEPSUDO=$!
trap 'kill $KEEPSUDO 2>/dev/null' EXIT
# ------------------------------------------------------------------------------------
HERE="$(cd "$(dirname "$0")" && pwd)"; T="$HERE/tools"; OUT="$HOME/Desktop/hdmib_verify.txt"; W="$HOME/hdmib_verify"
for f in "$T/kcx.pl" "$T/layout_fingerprint.py" "$T/reference_darwin25.json" "$HERE/kern_igfx_hdmib.hpp"; do [ -f "$f" ] || { echo "Falta $f (ejecuta el script desde la carpeta completa del modulo)."; exit 1; }; done
command -v python3 >/dev/null || { echo "Falta python3 (instala las Command Line Tools: xcode-select --install)."; exit 1; }
rm -rf "$W"; mkdir -p "$W"; : > "$OUT"
{
echo "=== hdmib verify_layout $(date) ==="
echo "macOS $(sw_vers -productVersion) build $(sw_vers -buildVersion)  Darwin $(uname -r)  modelo $(sysctl -n hw.model)"
echo "kernel: $(uname -v | cut -c1-120)"
} | tee -a "$OUT"
echo "--- 1) entorno python con capstone" | tee -a "$OUT"
python3 -m venv "$W/venv" >>"$OUT" 2>&1 && "$W/venv/bin/pip" install -q capstone >>"$OUT" 2>&1 || { echo "No se pudo instalar capstone (revisa internet). Detalle en $OUT"; exit 1; }
echo "--- 2) buscando el kext en los Kernel Collections (el driver puede estar en el Boot KC o en el System KC)" | tee -a "$OUT"
K=""
for KC in /System/Library/KernelCollections/BootKernelExtensions.kc /System/Library/KernelCollections/SystemKernelExtensions.kc /Library/KernelCollections/AuxiliaryKernelExtensions.kc; do
  [ -f "$KC" ] || { echo "(no existe $KC)" | tee -a "$OUT"; continue; }
  echo ">> $KC" | tee -a "$OUT"
  rm -rf "$W/kcx"; sudo perl "$T/kcx.pl" "$KC" "$W/kcx" AppleIntelICLLPGraphicsFramebuffer 2>&1 | tee -a "$OUT"
  sudo chown -R "$(id -un)" "$W/kcx" 2>/dev/null
  K="$(ls -d "$W"/kcx/*AppleIntelICLLPGraphicsFramebuffer* 2>/dev/null | head -1)"
  [ -n "$K" ] && [ -f "$K/index.txt" ] && { echo "encontrado en $KC" | tee -a "$OUT"; break; }
  K=""
done
[ -n "$K" ] || { echo "No se extrajo el kext de Ice Lake LP de ningun KC." | tee -a "$OUT"; exit 1; }
echo "--- 3) huella (dump)" | tee -a "$OUT"
"$W/venv/bin/python3" -I "$T/layout_fingerprint.py" dump "$K" "$W/new.json" "$HERE/kern_igfx_hdmib.hpp" 2>&1 | tee -a "$OUT"
echo "--- 4) comparacion con la referencia de macOS 26 (Darwin 25)" | tee -a "$OUT"
"$W/venv/bin/python3" -I "$T/layout_fingerprint.py" diff "$T/reference_darwin25.json" "$W/new.json" "$HERE/kern_igfx_hdmib.hpp" 2>&1 | tee -a "$OUT"
cp "$K/index.txt" "$W/index.txt" 2>/dev/null
( cd "$W" && rm -f "$HOME/Desktop/hdmib_verify.zip" && zip -q "$HOME/Desktop/hdmib_verify.zip" new.json index.txt )
echo; echo "Listo: $OUT  y  ~/Desktop/hdmib_verify.zip  (envialos los dos)"

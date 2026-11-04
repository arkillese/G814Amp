#!/bin/bash
# Installs build/G814Amp.kext into /Library/Extensions and rebuilds the auxiliary
# kernel collection (the same way the NullMoth installer does). Run with sudo.
# The driver is idle at boot; it only touches the hardware when probe-tool asks.
set -uo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "Run this with sudo:  sudo ./install.sh"; exit 1; }
[ -d build/G814Amp.kext ] || { echo "build/G814Amp.kext is missing - run ./build.sh first"; exit 1; }

EXT=/Library/Extensions/G814Amp.kext
LOG=/Library/Logs/G814Amp-kmutil.log
rm -rf "$EXT"
ditto build/G814Amp.kext "$EXT"
chown -R root:wheel "$EXT"; chmod -R go-w "$EXT"

rebuild() {
  K=/System/Library/Kernels/kernel; KA=(--allow-missing-kdk); [ -f "$K" ] && KA+=(--kernel "$K")
  kmutil create -n aux --volume-root / "${KA[@]}" -B /System/Library/KernelCollections/BootKernelExtensions.kc \
    -S /System/Library/KernelCollections/SystemKernelExtensions.kc --repository /Library/Extensions \
    -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc -z > "$LOG" 2>&1
}

echo "Rebuilding the kernel collection (takes a minute)..."
rebuild
if kmutil inspect -a x86_64 -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -q com.local.G814Amp; then
  echo "OK: G814Amp is installed. Restart the Mac (normal Restart from the Apple menu)."
else
  echo "FAILED: G814Amp did not make it into the kernel collection (log: $LOG). Undoing."
  rm -rf "$EXT"; rebuild
  exit 1
fi

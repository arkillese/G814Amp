#!/bin/bash
# Removes G814Amp and rebuilds the auxiliary kernel collection. Run with sudo.
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "Run this with sudo:  sudo ./uninstall.sh"; exit 1; }
rm -rf /Library/Extensions/G814Amp.kext
K=/System/Library/Kernels/kernel; KA=(--allow-missing-kdk); [ -f "$K" ] && KA+=(--kernel "$K")
kmutil create -n aux --volume-root / "${KA[@]}" -B /System/Library/KernelCollections/BootKernelExtensions.kc \
  -S /System/Library/KernelCollections/SystemKernelExtensions.kc --repository /Library/Extensions \
  -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc -z > /Library/Logs/G814Amp-kmutil.log 2>&1
echo "G814Amp removed. Restart the Mac."

# CLAUDE.md: working on G814Amp

G814Amp is a macOS kernel extension (IOKit, C++) that drives the two Cirrus Logic CS35L41
speaker amps on the ASUS ROG Strix G18 G814J (HD Audio subsystem 1043:1ccf). Read README.md
for what it does. This file is for an AI assistant helping someone build, debug or adapt it.

## Safety rules (always)

- **Boost values can damage hardware.** Only use inductor, capacitor and peak-current values
  from the laptop's own ACPI `_DSD` or from Linux's `sound/hda/codecs/side-codecs/
  cs35l41_hda_property.c` table for that exact subsystem ID. Never estimate them. If neither
  source has them, stop and tell the user.
- Keep the board check in `G814Amp::start()` (`nameMatching("pci1043,1ccf")`) strict. When
  adapting, replace it with the new model's ID; never remove it.
- Test in stages, each with a restart and the user's confirmation: (1) read-only probe of
  pads and chip IDs, (2) amp setup without firmware at low volume, (3) firmware. Tell the user
  to keep the volume low until protection is confirmed running.
- The user installs with `sudo ./install.sh` themselves and should **Shut Down**, not
  Restart, between tests (the NullMoth NVIDIA driver sometimes fails after warm restarts).
- Don't touch the Realtek/HD Audio controller at runtime except through the Immediate
  Command interface with commands spaced about 50 ms apart; bursts jam it while another
  driver owns the CORB/RIRB.

## Layout

- `G814Amp.cpp`: the whole driver: GPIO pads (INTC1085 communities), LPSS SPI (pxa2xx SSP,
  polled), CS35L41 register access (regmap SPI framing: 32-bit BE address with bit 31 for
  reads, 16 pad bits, 32-bit BE value), amp init (errata, OTP unpack, boost), Halo DSP
  firmware loading (WMFW/WMDR parsers, algorithm list, MPU, core start, CSPL mailbox),
  playback following, and sleep recovery.
- `build.sh`: builds the kext with plain clang (no Xcode project) and generates
  `build/Firmware.h` from `firmware/`.
- `install.sh` / `uninstall.sh`: copy to /Library/Extensions and rebuild the auxiliary
  kernel collection (`kmutil create -n aux ...`).
- `probe-tool.c`: sets registry properties on the driver (Enable, Firmware, VolumeDB, Probe,
  ReadReg, WriteReg, CodecVerb, PlaybackVerbs). Results show up as properties:
  `ioreg -l -w0 -r -c G814Amp -d1`.
- `tools/set-output.c`: switches CoreAudio's default output. `tools/hda.sh`: shell helpers
  for sending Realtek verbs through probe-tool (debug).

## Facts for the G814J (from its DSDT and Linux)

- SPI3 = PCI 8086:7a79, 4 MHz, mode 0. Amp 0 on native CS0; amp 1's CS is GPIO 468 = GPP_D20.
- Reset is GPIO 320 = GPP_S0 (active low), speaker ID is 192 = GPP_G0, IRQ is 321 = GPP_S1.
  ACPI pin numbers map to pads through `pinctrl-alderlake.c` (`adls_communities`).
- Linux maps 10431CCF to: 2 amps, internal boost, 1000 nH, 4500 mA, 24 µF, channels L/R.
- Firmware: `halo_cspl_RAM_revB2_29.63.1.wmfw` plus `cs35l41-dsp1-spk-prot-10431ccf-
  spkid{0,1}-{l0,r0}.bin`. Gain with firmware is 17.5 dB PCM / 19.5 dB PDM (Linux defaults).
- The Realtek ALC294 only clocks I2S to the amps while pin 0x17 streams (NMXAudio's
  "Internal Analog" output). The amps' `IRQ1_RAW_STATUS3` bit 1 (PLL_LOCK) shows whether
  BCLK arrives.

## Adapting to another laptop

The user-facing walkthrough is `docs/PORTING.md`; follow its stages and keep the user in the
loop at each restart. The user may not be technical: give exact commands, one step at a
time, and read Terminal output for them if they can't see it.

1. Get the subsystem ID (`ioreg -l | grep -i '"compatible" = <"pci'` on the HD Audio device)
   and the ACPI tables. Windows: `acpidump.exe -b` from Intel's ACPICA tools. Linux:
   `sudo acpidump`. OpenCore: SysReport. Find the `CSC3551` device and decode its `_CRS`
   (SpiSerialBus or I2cSerialBus, GpioIo, GpioInt) and any `_DSD`.
2. Look the subsystem ID up in Linux `cs35l41_hda_property.c` (boost, GPIO indices: reset,
   speaker ID, chip-select) and in linux-firmware's WHENCE (cirrus/ files for that ID).
3. Map the ACPI GPIO numbers to pads for that platform's Intel pinctrl driver, and check the
   GPIO controller `_HID`.
4. I2C-connected amps need an I2C transport in place of `spiFrame()` (LPSS I2C,
   DesignWare). Everything above the register access layer stays the same.
5. Change the board check, pads, boost constants, channel map and firmware file names, then
   test in the staged order above.

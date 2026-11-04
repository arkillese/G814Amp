# G814Amp: internal speakers for the ASUS ROG Strix G16 (G814J) on macOS

The G814J's internal speakers are driven by two **Cirrus Logic CS35L41** smart amplifiers
on an SPI bus, not by the Realtek codec. macOS has no driver for them, so on a Hackintosh
the speakers stay silent even when audio "plays". G814Amp is a kernel extension that drives
those amps, the way Linux's `cs35l41_hda` driver does, including Cirrus's speaker-protection
firmware.

**Status:** working on a G814J (Intel i7-13650HX, RTX 4060) with macOS 26.7.2 (Tahoe) and the
NullMoth driver package 2.7.1. Both speakers play at full, protected volume, start
automatically at boot and recover after sleep.

> ⚠️ **This is an unofficial, experimental kernel driver.** It writes to amplifier, boost and
> GPIO hardware registers. It refuses to run on anything except the G814J (HD Audio subsystem
> `1043:1ccf`), because other laptops use different boost settings, and wrong boost settings
> can damage the amplifiers. Use it at your own risk. Keep a known-good EFI on a USB stick.

## Requirements

- **ASUS ROG Strix G16 G814J** (G814JU / JV / JI). Check: *System Information → Audio* or
  `ioreg -l | grep pci1043,1ccf` must find something.
- macOS with a working **Realtek ALC294 driver** that outputs to pin 0x17. Tested with
  **NullMoth 2.7.1's NMXAudio** on macOS 26. The amps take their audio from the Realtek
  chip, so without a Realtek driver there is nothing to play.
- An OpenCore setup that allows unsigned kexts in `/Library/Extensions` (SIP kext and
  filesystem bits relaxed, AMFI relaxed). This is the same requirement as NullMoth's drivers.

## Install (prebuilt)

1. Download `G814Amp-<version>.zip` from the **Releases** page and unzip it.
2. Open **Terminal** in the unzipped folder (right-click the folder → *New Terminal at Folder*).
3. Run:
   ```bash
   sudo ./install.sh
   ```
   Wait for `OK: G814Amp is installed`.
4. **Shut down** (don't restart), wait 10 seconds and power on.
5. In **System Settings → Sound → Output**, choose **Realtek ALC294 (Internal Analog)**.
6. Start with the volume low and turn it up gradually.

## Uninstall

```bash
sudo ./uninstall.sh
```
Then shut down and power on.

## Build from source

Needs Xcode (tested with 26.5).
```bash
./build.sh
sudo ./install.sh
```
`build.sh` embeds the Cirrus firmware files from `firmware/` into the kext unmodified.

## Checking that it works

```bash
ioreg -l -w0 -r -c G814Amp -d1 | grep -E '"(Status|Mode|speaker-id|amp[01]-dsp"|amp[01]-errors-seen)"'
```
You should see `Mode = "firmware (speaker protection, 17.5 dB gain)"`, both amps
`running`, `errors-seen = 0`, and `Status = "playing"` while sound plays.

`build/probe-tool` sends commands to the driver:

| Command | What it does |
|---|---|
| `probe-tool disable` / `enable` | power the amps down / set them up again |
| `probe-tool firmware off` / `on` | run without / with the Cirrus firmware |
| `probe-tool volume <dB>` | amp volume, −60 to 0 dB (default 0) |
| `probe-tool read <reg>` | read an amp register from both amps (debug) |

`build/set-output "Internal Analog"` switches macOS's output device from the command line.

## Troubleshooting

- **No sound:** make sure the output is *Realtek ALC294 (Internal Analog)*, not Headphones.
  The amps only get audio while that output is playing.
- **Very quiet:** check `Mode`. If it says "no firmware", the firmware didn't start. Look at
  `amp0-dsp` / `amp1-dsp` for the reason.
- **Graphics fall back to a basic display after an install:** that's a NullMoth NVIDIA quirk.
  Shut down fully and power on.
- **Restarting from Windows freezes the boot:** always use Shut Down when leaving Windows.

## How it works

1. **Bus and pins:** from the laptop's DSDT (`SPK1`, `_HID CSC3551` under `SPI3`). The amps are
   on SPI3 at 4 MHz, mode 0. Amp 0 uses native CS0, amp 1 uses GPIO `GPP_D20` as chip-select.
   Reset is `GPP_S0` (active low), speaker ID is `GPP_G0`, IRQ is `GPP_S1`. The GPIO
   controller is `INTC1085` (Raptor Lake-S), which VoodooGPIO doesn't support, so the
   driver programs those four pads directly.
2. **SPI controller:** Intel LPSS SPI (`8086:7a79`), which macOS never powers up. The driver
   runs its ACPI `_PS0`, puts it in D0 and drives it in polled mode.
3. **Amp setup:** ported from Linux `cs35l41-lib.c` and `cs35l41_hda.c`: software reset, B2
   errata, OTP trim unpacking, internal boost (1000 nH / 4500 mA / 24 µF from
   `cs35l41_hda_property.c` for `10431CCF`), and I2S (ASP) setup.
4. **Firmware:** ported from Linux `cs_dsp.c`. It loads the `.wmfw` program and the
   speaker-ID-specific `.bin` tuning into each amp's Halo DSP, starts it, and drives it
   through the CSPL mailbox.
5. **Playback:** the driver watches NMXAudio's output engine on the Realtek codec. When it
   runs, the amps are resumed, powered up and unmuted. The Realtek chip clocks the I2S bus
   to the amps while pin 0x17 streams.

## Adapting it to another laptop with CS35L41 amps

Many laptops (other ASUS, Lenovo, HP, Dell models) use the same amps, but **pins, bus
(SPI or I2C), boost values and firmware differ per model**.

👉 **Step-by-step guide: [docs/PORTING.md](docs/PORTING.md).** No driver experience needed.
You adapt it with [Claude Code](https://claude.com/claude-code), which reads this repo's
`CLAUDE.md` (how the code works, plus the safety rules) and does the technical work, while
you dump your ACPI tables, install test builds and restart.

Short version:
1. Clone this repo and open it in Claude Code.
2. Put your ACPI tables in `acpi/`.
3. Ask: *"I want to adapt G814Amp to my laptop. Read CLAUDE.md and docs/PORTING.md first."*
4. Follow the staged tests: read-only probe, then low-volume sound, then firmware.

Never guess boost values: if your laptop isn't in Linux's `cs35l41_hda_property.c` table and
its ACPI has no `_DSD` boost properties, don't enable the boost.

## Credits and licences

- Driver code: **GPL-2.0-only** (see `LICENSE`). The amp, DSP and firmware-loading sequences
  are ported from the Linux kernel (Cirrus Logic and Linux contributors).
- `firmware/`: Cirrus Logic firmware from
  [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware), redistributed
  unmodified under `firmware/LICENSE.cirrus`, for use with Cirrus Logic devices only.
- NullMoth's drivers are a separate project and aren't included.

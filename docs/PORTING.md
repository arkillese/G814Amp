# Making G814Amp work on your laptop

This guide is for people with a **different laptop** whose speakers don't work on macOS
because they use **Cirrus Logic CS35L41** amplifiers. You don't need to know how to write
drivers. You'll do it with **Claude Code** (an AI coding assistant), which reads this repo's
`CLAUDE.md` and does the technical work, while you run a few commands and restart.

The G814J version took about a dozen build-and-restart rounds. Expect something similar.

---

## 0. Is this guide for you?

Your laptop probably has CS35L41 amps if **any** of these is true:

- On Linux, `dmesg | grep -i cs35l41` shows messages, or Linux's audio needs
  "cs35l41" firmware.
- On Windows, *Device Manager → System devices* lists something with **Cirrus Logic** or
  **CSC3551**.
- Your laptop's ACPI tables (step 2) contain the text `CSC3551`.

Common examples: many ASUS ROG/TUF/Zenbook models from 2022 on, Lenovo Legion and Yoga, HP
Envy/Spectre/OMEN, and some Dell XPS models.

If your speakers are silent but your laptop **doesn't** have CS35L41 amps, this repo won't
help: your problem is probably the Realtek codec layout (AppleALC/SimpleHDA).

## What you need

- A Mac / Hackintosh with **Xcode** installed (free, from the App Store).
- **Claude Code**: <https://claude.com/claude-code>. The desktop app is easiest.
- A **working audio driver for your Realtek codec** on macOS (for example NullMoth's
  NMXAudio on macOS 26, or AppleALC on older macOS). The amps take their sound from the
  Realtek chip, so that part must already work.
- A **USB stick with a known-good copy of your EFI**, in case a test build stops macOS
  booting.
- Your laptop's **ACPI tables** (step 2).
- Patience: each test means installing, shutting down and powering on.

> ⚠️ **Safety first.** These amps have a built-in voltage booster. Wrong boost settings can
> **permanently damage the amplifier chips**. Only use values from your laptop's ACPI or from
> Linux's own table for your exact model. If Claude can't find them, **stop**. Never guess.

---

## 1. Get the repo onto your Mac

In Terminal:
```bash
git clone https://github.com/arkillese/G814Amp.git
```
Then open the `G814Amp` folder in Claude Code. In the desktop app, choose the folder when
starting a session.

## 2. Dump your ACPI tables

Claude needs these to find your amps' bus and pins. Pick whichever is easiest:

**From Windows (easiest if you dual-boot):**
1. Download Intel's *"ACPICA Windows binary tools"* (search for that name on intel.com).
2. Unzip to `C:\acpi`.
3. Open **Command Prompt as administrator** and run:
   ```
   cd C:\acpi
   acpidump.exe -b
   ```
4. You'll get `dsdt.dat` and several `ssdt*.dat` files. Copy them to your Mac. (macOS can read
   the Windows drive directly.)
5. Don't share `msdm.dat` with anyone: it contains your Windows licence key.
6. Leave Windows with **Shut Down**, not Restart.

**From a Linux live USB:** `sudo acpidump > acpi.txt`

**From OpenCore:** use a DEBUG build with `Misc → Debug → SysReport = true`. The tables end up
in `SysReport/ACPI` on your EFI partition.

Put the files in a folder called `acpi/` inside the repo folder.

## 3. Ask Claude to start

Paste this into Claude Code:

> I want to adapt G814Amp to my laptop. Read CLAUDE.md and docs/PORTING.md first.
> My laptop is **[brand and model]**, macOS **[version]**, Realtek driver **[NMXAudio /
> AppleALC / …]**. My ACPI tables are in the `acpi/` folder. Please start by finding out
> whether my laptop has CS35L41 amps, how they're connected, and whether Linux has a
> configuration and firmware for my model. Don't change any code yet.

Claude will:
- find your HD Audio **subsystem ID** (like `1043:1ccf`) with `ioreg`
- decode your amps' entry (`CSC3551`) in the ACPI tables: **SPI or I2C** bus, chip-select,
  reset and speaker-ID pins, and any boost properties (`_DSD`)
- look your subsystem ID up in Linux's `cs35l41_hda_property.c` for the **boost values** and
  pin roles
- check **linux-firmware** for your model's tuning files (`cs35l41-dsp1-spk-prot-<id>-…`)

**Stop here if** Claude says there are no boost values for your model anywhere, or your amps
use *external* boost and it can't find how that's switched. Don't continue on guesses.

## 4. The staged tests

Claude changes the code a little at a time. After each change:

1. Claude builds it (`./build.sh`).
2. **You** install it:
   ```bash
   sudo ./install.sh
   ```
   You type your password; Claude can't do this step for you.
3. **Shut down**, wait 10 seconds, power on.
4. Tell Claude you're back. It reads the driver's status itself (`ioreg`, `probe-tool`).

The stages, in order. Don't let anyone skip ahead:

| Stage | What it does | Risk |
|---|---|---|
| **1. Probe** | Reads the GPIO pins and asks each amp for its chip ID (`0x35A40`). Changes nothing important. | Very low |
| **2. No-firmware sound** | Resets and sets up the amps with your boost values, at low volume and low gain. You test with the Mac volume low. | Low, if the boost values are right |
| **3. Firmware** | Loads Cirrus's speaker-protection firmware and your model's tuning into the amps. Full volume. | Low; it falls back to stage 2 if anything fails |

**If your amps are on I2C** (common on Lenovo/HP), Claude also needs to add an I2C transport
for the Intel LPSS I2C controller. That's extra work between stages 1 and 2, but everything
above it stays the same.

## 5. Things that confused us (so they don't confuse you)

- **"Playing" but silent:** the amps only get sound while the macOS output is the Realtek
  **speaker** output ("Internal Analog" or similar), not Headphones. The Realtek chip only
  sends the amps their clock while its speaker pin is streaming. Claude can check the amps'
  `PLL_LOCK` bit to see whether the clock arrives.
- **Quiet without firmware:** expected. Without the protection firmware the amps run at low
  gain (Linux does the same). Stage 3 is where the volume comes from.
- **Graphics fall back to a basic display after an install** (NullMoth NVIDIA users): shut
  down fully and power on.
- **Restarting from Windows freezes the boot:** always use Shut Down when leaving Windows.
- **The Terminal panel shows nothing:** ask Claude to read the Terminal tab for you.

## 6. When it works, share it

- Keep the board check strict: your build should only run on **your** subsystem ID.
- If your laptop has two speaker suppliers (a speaker-ID pin), include both tuning files.
- Fork this repo, or open an issue or pull request here with your model's values, so others
  with the same laptop can use it.

## Prompt cheat sheet

| Situation | Say to Claude |
|---|---|
| Starting | "Read CLAUDE.md and docs/PORTING.md, then check whether my laptop has CS35L41 amps using the ACPI files in acpi/." |
| After a restart | "I'm back. Check the driver status." |
| Can't see Terminal output | "Read the terminal tab for me." |
| No sound | "Check whether the amps' PLL is locked while audio plays on the speaker output." |
| Something sounds wrong | "Disable the amps now." (`probe-tool disable`) |
| Want to stop | "Help me uninstall G814Amp." (`sudo ./uninstall.sh`) |

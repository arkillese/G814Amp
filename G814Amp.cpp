// SPDX-License-Identifier: GPL-2.0-only
//
// G814Amp - driver for the two Cirrus Logic CS35L41 speaker amps on the
// ASUS ROG Strix G18 (G814J, HD Audio subsystem 1043:1ccf) under macOS.
//
// The driver attaches to the Intel LPSS SPI controller (8086:7a79, ACPI SPI3). A few
// seconds after boot it resets both amps, applies the B2 errata and factory trims,
// sets up the internal boost, loads Cirrus's speaker-protection firmware and tuning
// (picked by the speaker ID pin) into each amp's Halo DSP, and then follows playback:
// whenever an NMXAudio output on the Realtek codec (00:1f.3) is running, the amps are
// powered up and unmuted; when it stops they power down. If the amps lose state (for
// example in sleep) they are set up again. If the firmware cannot be started, the amps
// fall back to Linux's no-DSP path at low gain.
//
// Commands arrive as registry properties (see probe-tool.c): Enable, Firmware,
// VolumeDB, Probe, and the debug commands ReadReg, WriteReg, CodecVerb and
// PlaybackVerbs. Results and amp status are published as registry properties.
// Personality keys AutoEnable and UseFirmware (default Yes) control the boot behaviour.
//
// Pin and bus details come from this laptop's DSDT (SPK1, _HID CSC3551). The amp and
// DSP sequences are ported from Linux (sound/soc/codecs/cs35l41-lib.c,
// sound/hda/codecs/side-codecs/cs35l41_hda*.c, drivers/firmware/cirrus/cs_dsp.c):
// 10431CCF = 2 amps, internal boost, 1000 nH, 4500 mA, 24 uF.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/OSByteOrder.h>

#include "Firmware.h"

#define LOG(fmt, ...) IOLog("G814Amp: " fmt "\n", ##__VA_ARGS__)

// ---- Intel LPSS SPI (pxa2xx SSP + LPSS private registers at 0x200) ----
#define SSCR0           0x00
#define SSCR1           0x04
#define SSSR            0x08
#define SSDR            0x10
#define SSTO            0x28
#define SSPSP           0x2c
#define SSCR0_DSS8      0x7
#define SSCR0_SSE       (1u << 7)
#define SSCR0_SCR(x)    ((uint32_t)(x) << 8)
#define SSSR_TNF        (1u << 2)
#define SSSR_RNE        (1u << 3)
#define SSSR_BSY        (1u << 4)

#define PRIV            0x200
#define PRIV_CLOCK      (PRIV + 0x00)   // bit0 enable, M [15:1], N [30:16], bit31 update
#define PRIV_RESETS     (PRIV + 0x04)
#define PRIV_CS_CTRL    (PRIV + 0x24)   // bit0 SW mode, bit1 CS high, [9:8] CS select
#define PRIV_CLOCK_GATE (PRIV + 0x38)
#define PRIV_CAPS       (PRIV + 0xfc)
#define CS_SW_MODE      (1u << 0)
#define CS_HIGH         (1u << 1)
#define CS_SEL_MASK     (3u << 8)
#define LPSS_INPUT_HZ   100000000u
#define SPI_HZ          4000000u

// ---- Intel GPIO community (INTC1085, Raptor Lake-S PCH) ----
#define GPIO_REVID      0x000
#define GPIO_PADBAR     0x00c
#define GPIO_PADOWN     0x0a0
#define PAD_TX          (1u << 0)
#define PAD_RX          (1u << 1)
#define PAD_TXDIS       (1u << 8)
#define PAD_PMODE(v)    (((v) >> 10) & 0xf)

struct Pad {
    const char *name;   // property prefix
    const char *pch;    // PCH pad name, for the log
    uint8_t community;  // index into GPI0's memory ranges
    uint16_t padno;     // pad number inside the community
    uint8_t ownReg;     // PAD_OWN register index inside the community
    uint8_t ownNibble;
};

// ACPI pin -> pad, from pinctrl-alderlake.c (adls_communities)
//   320 GPP_S0  community 3, padno 0      reset (output, active low)
//   468 GPP_D20 community 4, padno 20     chip-select for amp 2 (output, active low)
//   192 GPP_G0  community 1, padno 24     speaker ID (input)
//   321 GPP_S1  community 3, padno 1      amp IRQ (input)
enum { PAD_RESET, PAD_CS2, PAD_SPKID, PAD_IRQ, PAD_COUNT };
static const Pad kPads[PAD_COUNT] = {
    { "reset", "GPP_S0",  3, 0,  0, 0 },
    { "cs2",   "GPP_D20", 4, 20, 2, 4 },
    { "spkid", "GPP_G0",  1, 24, 3, 0 },
    { "irq",   "GPP_S1",  3, 1,  0, 1 },
};

#define GPIO_COMMUNITIES 5

// ---- CS35L41 registers (include/sound/cs35l41.h) ----
#define CS_DEVID                0x00000000
#define CS_REVID                0x00000004
#define CS_OTPID                0x00000010
#define CS_SFT_RESET            0x00000020
#define CS_TEST_KEY_CTL         0x00000040
#define CS_OTP_MEM0             0x00000400
#define CS_PWR_CTRL1            0x00002014
#define CS_PWR_CTRL2            0x00002018
#define CS_PLL_CLK_CTRL         0x00002C04
#define CS_DSP_CLK_CTRL         0x00002C08
#define CS_GLOBAL_CLK_CTRL      0x00002C0C
#define CS_PROTECT_REL_ERR_IGN  0x00002034
#define CS_BSTCVRT_PEAK_CUR     0x00003808
#define CS_BSTCVRT_COEFF        0x00003810
#define CS_BSTCVRT_SLOPE_LBST   0x00003814
#define CS_BSTCVRT_DCM_CTRL     0x0000381C
#define CS_VIMON_SPKMON_RESYNC  0x00004100
#define CS_VPVBST_FS_SEL        0x00004400
#define CS_SP_ENABLES           0x00004800
#define CS_SP_RATE_CTRL         0x00004804
#define CS_SP_FORMAT            0x00004808
#define CS_SP_HIZ_CTRL          0x0000480C
#define CS_SP_FRAME_RX_SLOT     0x00004820
#define CS_SP_TX_WL             0x00004830
#define CS_SP_RX_WL             0x00004840
#define CS_DAC_PCM1_SRC         0x00004C00
#define CS_ASP_TX1_SRC          0x00004C20
#define CS_ASP_TX2_SRC          0x00004C24
#define CS_ASP_TX3_SRC          0x00004C28
#define CS_ASP_TX4_SRC          0x00004C2C
#define CS_DSP1_RX1_SRC         0x00004C40
#define CS_DSP1_RX2_SRC         0x00004C44
#define CS_DSP1_RX3_SRC         0x00004C48
#define CS_DSP1_RX4_SRC         0x00004C4C
#define CS_DSP1_RX5_SRC         0x00004C50
#define CS_DSP1_RX6_SRC         0x00004C54
#define CS_AMP_DIG_VOL_CTRL     0x00006000
#define CS_AMP_GAIN_CTRL        0x00006C04
#define CS_IRQ1_STATUS1         0x00010010
#define CS_IRQ1_STATUS3         0x00010018
#define CS_IRQ1_STATUS4         0x0001001C
#define CS_DSP1_CCM_CORE_CTRL   0x02BC1000
#define CS_DSP1_XM_ACCEL_PL0    0x02BC2020
#define CS_DSP1_YM_ACCEL_PL0    0x02BC20E0

#define CS_CHIP_ID              0x35a40
#define CS_REVID_B2             0xB2
#define CS_SOFTWARE_RESET       0x5A000000
#define CS_OTP_BOOT_DONE        0x02
#define CS_OTP_BOOT_ERR         0x80000000
#define CS_PUP_DONE             0x01000000
#define CS_PDN_DONE             0x00800000
#define CS_GLOBAL_EN            0x01
#define CS_AMP_EN               0x01
#define CS_BST_EN_MASK          0x30
#define CS_BST_EN_DEFAULT       (0x2 << 4)
#define CS_ASP_RX1_EN           0x00010000
#define CS_ERR_MASK             0x80020000u     // AMP_SHORT_ERR | TEMP_ERR (status1 errors below too)
#define CS_ERR_RLS_ALL          0x7E            // all *_ERR_RLS bits
#define CS_VOL_MUTE             0xA678          // AMP_HPF_PCM_EN, AMP_VOL_PCM mute
#define CS_GAIN_PCM_HDA         0x84            // Linux no-DSP unmute gain (AMP_GAIN_PCM 4.5 dB)

// The internal-boost configuration for 10431CCF (cs35l41_boost_config):
// 1000 nH -> LBST 0, 24 uF -> CBST range 1, 4500 mA peak.
#define BST_K1                  0x32
#define BST_K2                  0x49
#define BST_SLOPE               0x75
#define BST_LBST                0
#define BST_IPK                 (((4500 - 1600) / 50) + 0x10)

static const int kDefaultVolumeDB = 0;
static const int kMaxVolumeDB = 0;      // Linux's no-DSP level; no speaker protection without the firmware
static const int kMinVolumeDB = -60;

struct RegWrite { uint32_t reg, val; };

// cs35l41_revb2_errata_patch
static const RegWrite kErrataB2[] = {
    { CS_VIMON_SPKMON_RESYNC, 0 }, { 0x00004310, 0 }, { CS_VPVBST_FS_SEL, 0 },
    { CS_BSTCVRT_DCM_CTRL, 0x51 }, { CS_DSP1_YM_ACCEL_PL0, 0 }, { CS_DSP1_XM_ACCEL_PL0, 0 },
    { CS_PWR_CTRL2, 0 }, { CS_AMP_GAIN_CTRL, 0 }, { CS_ASP_TX3_SRC, 0 }, { CS_ASP_TX4_SRC, 0 },
};

// cs35l41_hda_config + cs35l41_hda_config_no_dsp
static const RegWrite kHDAConfig[] = {
    { CS_PLL_CLK_CTRL, 0x430 },         // 3.072 MHz BCLK input, PLL_REFCLK_EN
    { CS_DSP_CLK_CTRL, 0x3 },
    { CS_GLOBAL_CLK_CTRL, 0x3 },        // 48 kHz
    { CS_SP_RATE_CTRL, 0x21 },          // ASP_BCLK_FREQ 3.072 MHz
    { CS_SP_FORMAT, 0x20200200 },       // 32-bit slots, I2S, clock consumer
    { CS_SP_TX_WL, 0x18 },
    { CS_SP_RX_WL, 0x18 },
    { CS_ASP_TX1_SRC, 0x18 },
    { CS_ASP_TX2_SRC, 0x19 },
    { CS_DSP1_RX3_SRC, 0x18 },
    { CS_DSP1_RX4_SRC, 0x19 },
    { CS_SP_HIZ_CTRL, 0x2 },
    { CS_DAC_PCM1_SRC, 0x8 },           // DAC <- ASPRX1
    { CS_ASP_TX3_SRC, 0 },
    { CS_ASP_TX4_SRC, 0 },
    { CS_DSP1_RX5_SRC, 0x20 },
    { CS_DSP1_RX6_SRC, 0x21 },
    { CS_SP_ENABLES, CS_ASP_RX1_EN },
    { CS_DSP1_RX1_SRC, 0x8 },
    { CS_DSP1_RX2_SRC, 0x8 },
};

// OTP trim layout for OTP IDs 2, 3, 6 (otp_map_2) and 1, 8 (otp_map_1).
struct OTPField { uint32_t reg; uint8_t shift, size; };
static const OTPField kOTPMapCommon[] = {
    { 0x00002030, 0, 4 }, { 0x00002030, 7, 1 }, { 0x0000208c, 24, 6 }, { 0x00002090, 14, 4 },
    { 0x00002090, 10, 4 }, { 0x0000300C, 11, 4 }, { 0x0000394C, 23, 2 }, { 0x00003950, 0, 7 },
    { 0x00003950, 8, 7 }, { 0x00003950, 16, 8 }, { 0x00003950, 24, 8 }, { 0x00003954, 0, 7 },
    { 0x00003954, 8, 7 }, { 0x00003954, 16, 8 }, { 0x00003954, 24, 8 }, { 0x00003958, 0, 7 },
    { 0x00003958, 8, 7 }, { 0x00003958, 16, 8 }, { 0x00003958, 24, 8 }, { 0x0000395C, 0, 7 },
    { 0x0000395C, 8, 7 }, { 0x0000395C, 16, 8 }, { 0x0000395C, 24, 8 }, { 0x0000416C, 0, 8 },
    { 0x00004160, 0, 7 }, { 0x0000416C, 8, 8 }, { 0x00004160, 16, 10 }, { 0x0000416C, 16, 12 },
    { 0x0000416C, 28, 1 }, { 0x00004170, 0, 6 }, { 0x00004170, 6, 1 }, { 0x00004170, 8, 6 },
    { 0x00004170, 14, 1 }, { 0x00004170, 16, 9 }, { 0x00004360, 0, 5 }, { 0x00004360, 6, 9 },
    { 0x00004448, 0, 8 }, { 0x00004448, 8, 8 }, { 0x00004448, 16, 8 }, { 0x00004448, 24, 8 },
    { 0x0000444C, 0, 3 }, { 0x00006E30, 0, 5 }, { 0x00006E30, 8, 5 }, { 0x00006E30, 16, 5 },
    { 0x00006E30, 24, 5 }, { 0x00006E34, 0, 5 }, { 0x00006E34, 8, 5 }, { 0x00006E34, 16, 5 },
    { 0x00006E34, 24, 5 }, { 0x00006E38, 0, 5 }, { 0x00006E38, 8, 5 }, { 0x00006E38, 16, 5 },
    { 0x00006E38, 24, 5 }, { 0x00006E3C, 0, 5 }, { 0x00006E3C, 8, 5 }, { 0x00006E3C, 16, 5 },
    { 0x00006E3C, 24, 5 }, { 0x00006E40, 0, 5 }, { 0x00006E40, 8, 5 }, { 0x00006E40, 16, 5 },
    { 0x00006E40, 24, 5 }, { 0x00006E44, 0, 5 }, { 0x00006E48, 0, 10 }, { 0x00006E48, 10, 10 },
    { 0x00006E48, 20, 10 }, { 0x00006E4C, 0, 10 }, { 0x00006E4C, 10, 10 }, { 0x00006E4C, 20, 10 },
    { 0x00006E50, 0, 10 }, { 0x00006E50, 10, 10 }, { 0x00006E50, 20, 10 }, { 0x00006E54, 0, 10 },
    { 0x00006E54, 10, 10 }, { 0x00006E54, 20, 10 }, { 0x00006E58, 0, 10 }, { 0x00006E58, 10, 10 },
    { 0x00006E58, 20, 10 }, { 0x00006E5C, 0, 10 }, { 0x00006E5C, 10, 10 }, { 0x00006E5C, 20, 10 },
    { 0x00006E60, 0, 10 }, { 0x00006E60, 10, 10 }, { 0x00006E60, 20, 10 }, { 0x00006E64, 0, 10 },
    { 0x00007418, 7, 5 }, { 0x0000741C, 0, 5 }, { 0x0000741C, 11, 4 }, { 0x0000741C, 19, 4 },
    { 0x00007434, 17, 1 }, { 0x00007434, 18, 7 }, { 0x00007068, 0, 9 }, { 0x0000410C, 7, 1 },
    { 0x0000400C, 0, 7 },
};
// The maps differ only in the field after VIMON_DLY.
static const OTPField kOTPMap1Tail[] = {
    { 0x00000000, 0, 1 },                                   // extra bit
    { 0x00017040, 0, 8 }, { 0x00017040, 8, 8 }, { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 },
    { 0x00017044, 0, 24 },
};
static const OTPField kOTPMap2Tail[] = {
    { 0x00004000, 11, 1 },                                  // VMON_POL
    { 0x00017040, 0, 8 }, { 0x00017040, 8, 8 }, { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 },
    { 0x00017044, 0, 24 },
};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

class G814Amp : public IOService {
    OSDeclareDefaultStructors(G814Amp);

    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fSPIMap = nullptr;
    volatile uint8_t *fSPI = nullptr;
    IOMemoryMap *fGPIOMap[GPIO_COMMUNITIES] = {};
    volatile uint8_t *fGPIO[GPIO_COMMUNITIES] = {};
    IOLock *fLock = nullptr;
    IOWorkLoop *fWorkLoop = nullptr;
    IOTimerEventSource *fTimer = nullptr;

    bool fHWReady = false;      // GPIO mapped, pads checked, SPI controller set up
    bool fAmpsReady = false;    // both amps reset, patched and configured
    bool fEnabled = false;      // following playback
    bool fPlaying = false;      // amps powered and unmuted
    int fVolumeDB = kDefaultVolumeDB;
    uint32_t fErrors[2] = {};
    uint32_t fStatusUpdates = 0;
    uint32_t fTicks = 0;
    uint32_t fRecoveries = 0;
    int fAutoTries = 0;         // >0 while waiting to start the amps at boot
    bool fUseFirmware = true;   // load the Cirrus DSP firmware when setting up the amps
    bool fDSP = false;          // firmware running on both amps
    static const uint32_t kMaxAlgs = 32;
    struct Alg { uint32_t id, xm, ym; };
    Alg fAlgs[2][kMaxAlgs] = {};
    uint32_t fAlgCount[2] = {};

    // Realtek codec on the HD Audio controller (00:1f.3), owned by NMXAudio. We only
    // use the controller's Immediate Command interface, never its CORB/RIRB rings.
    IOMemoryMap *fHDAMap = nullptr;
    volatile uint8_t *fHDA = nullptr;
    OSArray *fPlaybackVerbs = nullptr;   // sent to the codec each time playback starts

    uint32_t spiRead(uint32_t off) { return *(volatile uint32_t *)(fSPI + off); }
    void spiWrite(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fSPI + off) = v; }
    uint32_t gpioRead(int c, uint32_t off) { return *(volatile uint32_t *)(fGPIO[c] + off); }
    void gpioWrite(int c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(fGPIO[c] + off) = v; }

    bool mapGPIO();
    uint32_t padOffset(const Pad &p);
    uint32_t padDW0(const Pad &p) { return gpioRead(p.community, padOffset(p)); }
    uint32_t padOwner(const Pad &p);
    bool padIsHostOutput(const Pad &p, char *why, size_t whyLen);
    void padSetTX(const Pad &p, bool high);

    void spiPowerUp();
    bool spiInit();
    void spiCS(bool assert);
    bool spiXfer(const uint8_t *tx, uint8_t *rx, size_t len);
    bool spiFrame(int amp, const uint8_t *tx, uint8_t *rx, size_t len);

    bool ampRead(int amp, uint32_t reg, uint32_t *val);
    bool ampWrite(int amp, uint32_t reg, uint32_t val);
    bool ampUpdate(int amp, uint32_t reg, uint32_t mask, uint32_t val);
    bool ampWriteSeq(int amp, const RegWrite *seq, size_t n);
    bool ampPoll(int amp, uint32_t reg, uint32_t mask, uint32_t timeoutMs, uint32_t *last);
    bool ampOTPUnpack(int amp);
    bool ampInit(int amp);
    bool ampsInit();
    void ampsPlay(bool on);
    bool ampWriteBlock(int amp, uint32_t reg, const uint8_t *data, size_t len);
    bool dspLoadWMFW(int amp, const uint8_t *fw, size_t size);
    bool dspReadAlgs(int amp);
    bool dspLoadBin(int amp, const uint8_t *bin, size_t size);
    bool dspMailbox(int amp, uint32_t cmd, uint32_t want);
    bool dspStart(int amp);
    void dspStop(int amp);
    uint32_t volumeReg() const;

    bool hwPrepare(bool publishPads);
    bool playbackActive();
    void timerFired(IOTimerEventSource *sender);

    void setNum(const char *key, uint32_t v) { setProperty(key, v, 32); }
    void setStr(const char *key, const char *v) { setProperty(key, v); }
    void runProbe();
    void setEnabled(bool on);
    void setEnabledLocked(bool on);
    bool ampsHealthy();
    void publishStatus();
    bool mapHDA();
    bool codecVerb(uint32_t verb, uint32_t *resp);
    void sendPlaybackVerbs();
    static bool regWritable(uint32_t reg);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *props) override;
};

OSDefineMetaClassAndStructors(G814Amp, IOService);

// ---------------------------------------------------------------------------

bool G814Amp::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;

    // Only the G814J: its HD Audio subsystem ID is 1043:1ccf. The pins and boost
    // values below are for this board only; wrong boost settings can damage the amps.
    OSDictionary *board = IOService::nameMatching("pci1043,1ccf");
    IOService *hda = board ? IOService::waitForMatchingService(board, 2ULL * 1000 * 1000 * 1000) : nullptr;
    OSSafeReleaseNULL(board);
    if (!hda) {
        LOG("not an ASUS G814J (no HD Audio subsystem 1043:1ccf), not attaching");
        return false;
    }
    hda->release();

    // And its ACPI must describe the CS35L41 amps on this bus.
    OSDictionary *amps = IOService::nameMatching("CSC3551");
    IOService *spk = amps ? IOService::waitForMatchingService(amps, 1ULL * 1000 * 1000 * 1000) : nullptr;
    OSSafeReleaseNULL(amps);
    if (!spk) {
        LOG("no CSC3551 amp in ACPI, not attaching");
        return false;
    }
    spk->release();

    fLock = IOLockAlloc();
    if (!fLock)
        return false;

    fPCI->setMemoryEnable(true);
    fSPIMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fSPIMap || fSPIMap->getLength() < 0x300) {
        LOG("could not map the SPI controller");
        return false;
    }
    fSPI = (volatile uint8_t *)fSPIMap->getVirtualAddress();

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        return false;
    fTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &G814Amp::timerFired));
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        return false;

    setStr("Status", "idle");
    setStr("Stage", "2 (amps follow playback, no DSP firmware)");
    setNum("VolumeDB", (uint32_t)fVolumeDB);
    registerService();

    // Start the amps a few seconds after boot unless the personality says not to.
    OSBoolean *useFw = OSDynamicCast(OSBoolean, getProperty("UseFirmware"));
    fUseFirmware = !useFw || useFw->isTrue();
    OSBoolean *autoEnable = OSDynamicCast(OSBoolean, getProperty("AutoEnable"));
    if (!autoEnable || autoEnable->isTrue()) {
        fAutoTries = 15;
        fTimer->setTimeoutMS(3000);
    }
    LOG("attached to SPI3");
    return true;
}

void G814Amp::stop(IOService *provider) {
    if (fTimer)
        fTimer->cancelTimeout();
    if (fLock) {
        IOLockLock(fLock);
        if (fPlaying)
            ampsPlay(false);
        fEnabled = false;
        IOLockUnlock(fLock);
    }
    IOService::stop(provider);
}

void G814Amp::free() {
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fTimer);
    }
    OSSafeReleaseNULL(fTimer);
    OSSafeReleaseNULL(fWorkLoop);
    OSSafeReleaseNULL(fPlaybackVerbs);
    OSSafeReleaseNULL(fHDAMap);
    for (int i = 0; i < GPIO_COMMUNITIES; i++)
        OSSafeReleaseNULL(fGPIOMap[i]);
    OSSafeReleaseNULL(fSPIMap);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    IOService::free();
}

IOReturn G814Amp::setProperties(OSObject *props) {
    OSDictionary *dict = OSDynamicCast(OSDictionary, props);
    if (!dict)
        return kIOReturnBadArgument;

    bool handled = false;
    if (OSNumber *vol = OSDynamicCast(OSNumber, dict->getObject("VolumeDB"))) {
        int db = (int)(int32_t)vol->unsigned32BitValue();
        if (db > kMaxVolumeDB)
            db = kMaxVolumeDB;
        if (db < kMinVolumeDB)
            db = kMinVolumeDB;
        IOLockLock(fLock);
        fVolumeDB = db;
        setNum("VolumeDB", (uint32_t)db);
        if (fPlaying) {
            uint32_t v = volumeReg();
            ampWrite(0, CS_AMP_DIG_VOL_CTRL, v);
            ampWrite(1, CS_AMP_DIG_VOL_CTRL, v);
        }
        IOLockUnlock(fLock);
        handled = true;
    }
    if (dict->getObject("Probe") == kOSBooleanTrue) {
        IOLockLock(fLock);
        if (fEnabled)
            setStr("Status", "probe refused: amps are enabled (send Enable=No first)");
        else
            runProbe();
        IOLockUnlock(fLock);
        handled = true;
    }
    // Firmware = Yes/No switches the DSP firmware on or off and sets the amps up again.
    if (OSBoolean *fw = OSDynamicCast(OSBoolean, dict->getObject("Firmware"))) {
        IOLockLock(fLock);
        fUseFirmware = fw->isTrue();
        if (fEnabled) {
            if (fPlaying)
                ampsPlay(false);
            ampsInit();
        }
        IOLockUnlock(fLock);
        handled = true;
    }
    if (OSBoolean *en = OSDynamicCast(OSBoolean, dict->getObject("Enable"))) {
        setEnabled(en->isTrue());
        handled = true;
    }

    // Diagnostics: ReadReg = address reads it from both amps into reg-amp0/1.
    // WriteReg = [amp, address, value] writes one register (clock, audio port and
    // routing registers only - never boost or power).
    if (OSNumber *addr = OSDynamicCast(OSNumber, dict->getObject("ReadReg"))) {
        IOLockLock(fLock);
        if (fHWReady) {
            uint32_t a = addr->unsigned32BitValue(), v0 = 0, v1 = 0;
            bool ok0 = ampRead(0, a, &v0), ok1 = ampRead(1, a, &v1);
            setNum("reg-addr", a);
            setNum("reg-amp0", ok0 ? v0 : 0xdeadbeef);
            setNum("reg-amp1", ok1 ? v1 : 0xdeadbeef);
        }
        IOLockUnlock(fLock);
        handled = true;
    }
    // CodecVerb = full 32-bit HDA verb: sent to the Realtek codec, answer in codec-resp.
    if (OSNumber *verb = OSDynamicCast(OSNumber, dict->getObject("CodecVerb"))) {
        IOLockLock(fLock);
        uint32_t resp = 0;
        bool ok = codecVerb(verb->unsigned32BitValue(), &resp);
        setNum("codec-verb", verb->unsigned32BitValue());
        setNum("codec-resp", resp);
        setStr("codec-result", ok ? "ok" : "timeout (immediate command interface busy or unavailable)");
        IOLockUnlock(fLock);
        handled = true;
    }
    // PlaybackVerbs = array of verbs sent to the codec whenever playback starts
    // (and right away if it is already playing). An empty array clears it.
    if (OSArray *verbs = OSDynamicCast(OSArray, dict->getObject("PlaybackVerbs"))) {
        IOLockLock(fLock);
        OSSafeReleaseNULL(fPlaybackVerbs);
        if (verbs->getCount())
            fPlaybackVerbs = OSArray::withArray(verbs);
        setNum("playback-verbs-count", fPlaybackVerbs ? fPlaybackVerbs->getCount() : 0);
        if (fPlaying)
            sendPlaybackVerbs();
        IOLockUnlock(fLock);
        handled = true;
    }
    if (OSArray *w = OSDynamicCast(OSArray, dict->getObject("WriteReg"))) {
        OSNumber *amp = OSDynamicCast(OSNumber, w->getObject(0));
        OSNumber *addr = OSDynamicCast(OSNumber, w->getObject(1));
        OSNumber *val = OSDynamicCast(OSNumber, w->getObject(2));
        if (!amp || !addr || !val || amp->unsigned32BitValue() > 1)
            return kIOReturnBadArgument;
        if (!regWritable(addr->unsigned32BitValue()))
            return kIOReturnNotPermitted;
        IOLockLock(fLock);
        bool ok = fHWReady && ampWrite((int)amp->unsigned32BitValue(), addr->unsigned32BitValue(),
                                       val->unsigned32BitValue());
        IOLockUnlock(fLock);
        return ok ? kIOReturnSuccess : kIOReturnNotReady;
    }
    return handled ? kIOReturnSuccess : kIOReturnUnsupported;
}

// ---------------------------------------------------------------------------
// Realtek codec commands through the HD Audio Immediate Command interface

#define HDA_ICOI    0x60    // immediate command out
#define HDA_ICII    0x64    // immediate response in
#define HDA_ICIS    0x68    // 16-bit status: bit0 busy, bit1 result valid (write 1 to clear)
#define HDA_ICB     0x1
#define HDA_IRV     0x2

bool G814Amp::mapHDA() {
    if (fHDA)
        return true;
    OSDictionary *match = IOService::nameMatching("pci8086,7a50");
    if (!match)
        return false;
    IOService *hdef = IOService::copyMatchingService(match);
    match->release();
    if (!hdef)
        return false;
    IODeviceMemory *mem = hdef->getDeviceMemoryWithIndex(0);
    if (mem && mem->getLength() >= 0x100)
        fHDAMap = mem->map(kIOMapInhibitCache);
    hdef->release();
    if (!fHDAMap)
        return false;
    fHDA = (volatile uint8_t *)fHDAMap->getVirtualAddress();
    return true;
}

bool G814Amp::codecVerb(uint32_t verb, uint32_t *resp) {
    if (!mapHDA())
        return false;
    volatile uint16_t *icis = (volatile uint16_t *)(fHDA + HDA_ICIS);
    for (int t = 0; *icis & HDA_ICB; t++) {
        if (t > 1000)
            return false;
        IODelay(1);
    }
    *icis = HDA_IRV;                                    // clear a stale result
    *(volatile uint32_t *)(fHDA + HDA_ICOI) = verb;
    *icis = HDA_ICB;
    for (int t = 0; !(*icis & HDA_IRV); t++) {
        if (t > 1000)
            return false;
        IODelay(1);
    }
    uint32_t r = *(volatile uint32_t *)(fHDA + HDA_ICII);
    *icis = HDA_IRV;
    if (resp)
        *resp = r;
    return true;
}

void G814Amp::sendPlaybackVerbs() {
    if (!fPlaybackVerbs)
        return;
    uint32_t sent = 0;
    for (unsigned i = 0; i < fPlaybackVerbs->getCount(); i++) {
        OSNumber *n = OSDynamicCast(OSNumber, fPlaybackVerbs->getObject(i));
        if (n && codecVerb(n->unsigned32BitValue(), nullptr))
            sent++;
    }
    setNum("playback-verbs-sent", sent);
}

bool G814Amp::regWritable(uint32_t reg) {
    return (reg >= CS_PLL_CLK_CTRL && reg <= CS_GLOBAL_CLK_CTRL) ||
           (reg >= CS_SP_ENABLES && reg <= CS_DSP1_RX6_SRC) ||
           reg == CS_AMP_DIG_VOL_CTRL || reg == CS_IRQ1_STATUS1 || reg == CS_IRQ1_STATUS3;
}

// Status registers, plus the audio-port and clock settings, as registry properties.
void G814Amp::publishStatus() {
    static const struct { uint32_t reg; const char *name; } kRegs[] = {
        { CS_IRQ1_STATUS1, "irq1-status1" }, { 0x00010014, "irq1-status2" },
        { CS_IRQ1_STATUS3, "irq1-status3" }, { CS_IRQ1_STATUS4, "irq1-status4" },
        { CS_PWR_CTRL1, "pwr-ctrl1" }, { CS_PWR_CTRL2, "pwr-ctrl2" },
        { CS_PLL_CLK_CTRL, "pll-clk-ctrl" }, { CS_SP_ENABLES, "sp-enables" },
        { CS_AMP_DIG_VOL_CTRL, "dig-vol" }, { CS_AMP_GAIN_CTRL, "amp-gain" },
        { CS_SP_FORMAT, "sp-format" }, { CS_DAC_PCM1_SRC, "dac-src" },
    };
    char key[48];
    for (int amp = 0; amp < 2; amp++) {
        for (size_t i = 0; i < ARRAY_COUNT(kRegs); i++) {
            uint32_t v;
            if (ampRead(amp, kRegs[i].reg, &v)) {
                snprintf(key, sizeof(key), "amp%d-%s", amp, kRegs[i].name);
                setNum(key, v);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// GPIO

bool G814Amp::mapGPIO() {
    if (fGPIO[0])
        return true;
    OSDictionary *match = IOService::nameMatching("INTC1085");
    if (!match)
        return false;
    IOService *gpi0 = IOService::waitForMatchingService(match, 2ULL * 1000 * 1000 * 1000);
    match->release();
    if (!gpi0) {
        LOG("GPIO controller INTC1085 not found");
        return false;
    }
    bool ok = gpi0->getDeviceMemoryCount() >= GPIO_COMMUNITIES;
    for (unsigned i = 0; ok && i < GPIO_COMMUNITIES; i++) {
        IODeviceMemory *mem = gpi0->getDeviceMemoryWithIndex(i);
        if (!mem || mem->getLength() < 0x1000) {
            ok = false;
            break;
        }
        fGPIOMap[i] = mem->map(kIOMapInhibitCache);
        if (!fGPIOMap[i]) {
            ok = false;
            break;
        }
        fGPIO[i] = (volatile uint8_t *)fGPIOMap[i]->getVirtualAddress();
    }
    gpi0->release();
    if (!ok) {
        LOG("could not map the GPIO communities");
        for (int i = 0; i < GPIO_COMMUNITIES; i++) {
            OSSafeReleaseNULL(fGPIOMap[i]);
            fGPIO[i] = nullptr;
        }
    }
    return ok;
}

uint32_t G814Amp::padOffset(const Pad &p) {
    // Communities with REVID >= 0x94 use 4 dwords per pad, older ones 2.
    uint32_t rev = gpioRead(p.community, GPIO_REVID) >> 16;
    uint32_t stride = rev >= 0x94 ? 16 : 8;
    uint32_t padbar = gpioRead(p.community, GPIO_PADBAR);
    return padbar + p.padno * stride;
}

uint32_t G814Amp::padOwner(const Pad &p) {
    return (gpioRead(p.community, GPIO_PADOWN + p.ownReg * 4) >> (p.ownNibble * 4)) & 0xf;
}

bool G814Amp::padIsHostOutput(const Pad &p, char *why, size_t whyLen) {
    uint32_t own = padOwner(p);
    uint32_t dw0 = padDW0(p);
    if (own != 0) {
        snprintf(why, whyLen, "%s not host owned (%u)", p.pch, own);
        return false;
    }
    if (PAD_PMODE(dw0) != 0) {
        snprintf(why, whyLen, "%s not in GPIO mode (pmode %u)", p.pch, PAD_PMODE(dw0));
        return false;
    }
    if (dw0 & PAD_TXDIS) {
        snprintf(why, whyLen, "%s output disabled (dw0 0x%08x)", p.pch, dw0);
        return false;
    }
    return true;
}

void G814Amp::padSetTX(const Pad &p, bool high) {
    uint32_t off = padOffset(p);
    uint32_t dw0 = gpioRead(p.community, off);
    dw0 = high ? (dw0 | PAD_TX) : (dw0 & ~PAD_TX);
    gpioWrite(p.community, off, dw0);
    (void)gpioRead(p.community, off);
}

// ---------------------------------------------------------------------------
// SPI

// Nothing in macOS powers SPI3 up (no driver ever used it), so its registers read
// all ones. Run the ACPI _PS0 method, put the PCI function in D0 and enable decoding.
void G814Amp::spiPowerUp() {
    IOACPIPlatformDevice *acpi = OSDynamicCast(IOACPIPlatformDevice, fPCI->getProperty("acpi-device"));
    if (acpi) {
        IOReturn r = acpi->evaluateObject("_PS0");
        setNum("SPI-acpi-PS0", (uint32_t)r);
    } else {
        setStr("SPI-acpi-PS0", "no ACPI device");
    }

    uint8_t pm = 0;
    if (fPCI->findPCICapability(kIOPCIPowerManagementCapability, &pm) && pm) {
        uint16_t pmcsr = fPCI->configRead16(pm + 4);
        setNum("SPI-pmcsr-before", pmcsr);
        if (pmcsr & 0x3) {
            fPCI->configWrite16(pm + 4, (uint16_t)(pmcsr & ~0x3));
            IOSleep(20);
        }
        setNum("SPI-pmcsr-after", fPCI->configRead16(pm + 4));
    }

    uint16_t cmd = fPCI->configRead16(kIOPCIConfigCommand);
    setNum("SPI-command-before", cmd);
    fPCI->configWrite16(kIOPCIConfigCommand, (uint16_t)(cmd | kIOPCICommandMemorySpace));
    setNum("SPI-bar0", fPCI->configRead32(kIOPCIConfigBaseAddress0));
}

bool G814Amp::spiInit() {
    spiPowerUp();

    uint32_t caps = spiRead(PRIV_CAPS);
    setNum("SPI-caps", caps);
    if (caps == 0xffffffff) {
        setStr("SPI-error", "controller reads all ones (powered off?)");
        return false;
    }

    // Take the controller out of reset.
    spiWrite(PRIV_RESETS, 0x7);

    // Input clock: keep the firmware's M/N divider if set, otherwise use 1/1 (100 MHz).
    uint32_t clk = spiRead(PRIV_CLOCK);
    uint32_t m = (clk >> 1) & 0x7fff, n = (clk >> 16) & 0x7fff;
    if (m == 0 || n == 0 || m > n) {
        m = n = 1;
    }
    spiWrite(PRIV_CLOCK, (1u << 31) | (n << 16) | (m << 1) | 1u);
    setNum("SPI-clock-before", clk);
    uint64_t inHz = (uint64_t)LPSS_INPUT_HZ * m / n;
    uint32_t scr = (uint32_t)((inHz + SPI_HZ - 1) / SPI_HZ) - 1;
    if (scr > 0xfff)
        scr = 0xfff;
    setNum("SPI-input-hz", (uint32_t)inHz);
    setNum("SPI-scr", scr);

    // Software chip select, deasserted, CS0 selected.
    uint32_t cs = spiRead(PRIV_CS_CTRL);
    cs = (cs & ~CS_SEL_MASK) | CS_SW_MODE | CS_HIGH;
    spiWrite(PRIV_CS_CTRL, cs);

    // Motorola SPI mode 0, 8-bit, polled.
    spiWrite(SSCR0, 0);
    spiWrite(SSCR1, 0);
    spiWrite(SSTO, 0);
    spiWrite(SSPSP, 0);
    spiWrite(SSCR0, SSCR0_SCR(scr) | SSCR0_DSS8);
    spiWrite(SSCR0, SSCR0_SCR(scr) | SSCR0_DSS8 | SSCR0_SSE);

    // Drain anything left in the RX FIFO.
    for (int i = 0; i < 64 && (spiRead(SSSR) & SSSR_RNE); i++)
        (void)spiRead(SSDR);
    return true;
}

void G814Amp::spiCS(bool assert) {
    uint32_t v = spiRead(PRIV_CS_CTRL);
    v = assert ? (v & ~CS_HIGH) : (v | CS_HIGH);
    spiWrite(PRIV_CS_CTRL, v);
    // With dynamic clock gating the CS line only moves while the clock runs.
    uint32_t gate = spiRead(PRIV_CLOCK_GATE);
    spiWrite(PRIV_CLOCK_GATE, (gate & ~0x3u) | 0x3u);
    spiWrite(PRIV_CLOCK_GATE, gate);
}

bool G814Amp::spiXfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int t = 0;
        while (!(spiRead(SSSR) & SSSR_TNF)) {
            if (++t > 10000)
                return false;
            IODelay(1);
        }
        spiWrite(SSDR, tx ? tx[i] : 0);
        t = 0;
        while (!(spiRead(SSSR) & SSSR_RNE)) {
            if (++t > 10000)
                return false;
            IODelay(1);
        }
        uint8_t b = (uint8_t)spiRead(SSDR);
        if (rx)
            rx[i] = b;
    }
    int t = 0;
    while (spiRead(SSSR) & SSSR_BSY) {
        if (++t > 10000)
            return false;
        IODelay(1);
    }
    return true;
}

// Amp 0 uses the controller's CS0; amp 1's chip select is GPIO GPP_D20.
bool G814Amp::spiFrame(int amp, const uint8_t *tx, uint8_t *rx, size_t len) {
    bool ok;
    if (amp == 0) {
        spiCS(true);
        ok = spiXfer(tx, rx, len);
        spiCS(false);
    } else {
        padSetTX(kPads[PAD_CS2], false);
        IODelay(1);
        ok = spiXfer(tx, rx, len);
        padSetTX(kPads[PAD_CS2], true);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// CS35L41 register access (regmap SPI: 32-bit BE address, 16 pad bits, 32-bit BE value;
// bit 31 of the address marks a read).

bool G814Amp::ampRead(int amp, uint32_t reg, uint32_t *val) {
    uint8_t tx[10] = {}, rx[10] = {};
    OSWriteBigInt32(tx, 0, reg | 0x80000000u);
    if (!spiFrame(amp, tx, rx, sizeof(tx)))
        return false;
    *val = OSReadBigInt32(rx, 6);
    return true;
}

bool G814Amp::ampWrite(int amp, uint32_t reg, uint32_t val) {
    uint8_t tx[10] = {};
    OSWriteBigInt32(tx, 0, reg & 0x7fffffffu);
    OSWriteBigInt32(tx, 6, val);
    return spiFrame(amp, tx, nullptr, sizeof(tx));
}

bool G814Amp::ampUpdate(int amp, uint32_t reg, uint32_t mask, uint32_t val) {
    uint32_t cur;
    if (!ampRead(amp, reg, &cur))
        return false;
    return ampWrite(amp, reg, (cur & ~mask) | (val & mask));
}

bool G814Amp::ampWriteSeq(int amp, const RegWrite *seq, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!ampWrite(amp, seq[i].reg, seq[i].val))
            return false;
    return true;
}

bool G814Amp::ampPoll(int amp, uint32_t reg, uint32_t mask, uint32_t timeoutMs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t t = 0; t <= timeoutMs; t++) {
        if (ampRead(amp, reg, &v) && (v & mask)) {
            if (last)
                *last = v;
            return true;
        }
        IOSleep(1);
    }
    if (last)
        *last = v;
    return false;
}

// cs35l41_otp_unpack: copy the factory trims from OTP memory into their registers.
// Must be called with the test key unlocked.
bool G814Amp::ampOTPUnpack(int amp) {
    uint32_t otpID;
    if (!ampRead(amp, CS_OTPID, &otpID))
        return false;

    const OTPField *tail;
    if (otpID == 1 || otpID == 8)
        tail = kOTPMap1Tail;
    else if (otpID == 2 || otpID == 3 || otpID == 6)
        tail = kOTPMap2Tail;
    else {
        char key[32];
        snprintf(key, sizeof(key), "amp%d-otp-id-unknown", amp);
        setNum(key, otpID);
        return false;
    }

    uint32_t mem[32];
    for (int i = 0; i < 32; i++)
        if (!ampRead(amp, CS_OTP_MEM0 + i * 4, &mem[i]))
            return false;

    const size_t nCommon = ARRAY_COUNT(kOTPMapCommon), nTail = ARRAY_COUNT(kOTPMap1Tail);
    int bitOffset = 16, wordOffset = 2;
    for (size_t i = 0; i < nCommon + nTail; i++) {
        if (wordOffset >= 32)
            return false;
        const OTPField &f = i < nCommon ? kOTPMapCommon[i] : tail[i - nCommon];
        uint32_t val;
        if (bitOffset + f.size - 1 >= 32) {
            val = mem[wordOffset] >> bitOffset;
            wordOffset++;
            if (wordOffset >= 32)
                return false;
            uint32_t hiBits = (uint32_t)(bitOffset + f.size - 32);
            val |= (mem[wordOffset] & ((1u << hiBits) - 1)) << (32 - bitOffset);
            bitOffset += f.size - 32;
        } else {
            uint32_t mask = f.size >= 32 ? 0xffffffffu : ((1u << f.size) - 1);
            val = (mem[wordOffset] >> bitOffset) & mask;
            bitOffset += f.size;
        }
        if (bitOffset == 32) {
            bitOffset = 0;
            wordOffset++;
        }
        if (f.reg) {
            uint32_t mask = (f.size >= 32 ? 0xffffffffu : ((1u << f.size) - 1)) << f.shift;
            if (!ampUpdate(amp, f.reg, mask, val << f.shift))
                return false;
        }
    }
    return true;
}

// Linux probe (cs35l41_hda_common_probe) plus cs35l41_hda_apply_properties for one amp.
// The shared reset line has already been pulsed.
bool G814Amp::ampInit(int amp) {
    char key[48];
    uint32_t v = 0;

    ampWrite(amp, CS_SFT_RESET, CS_SOFTWARE_RESET);
    IOSleep(3);
    if (!ampPoll(amp, CS_IRQ1_STATUS4, CS_OTP_BOOT_DONE, 100, &v)) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "no OTP_BOOT_DONE after reset");
        return false;
    }
    if (ampRead(amp, CS_IRQ1_STATUS3, &v) && (v & CS_OTP_BOOT_ERR)) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "OTP boot error");
        return false;
    }

    uint32_t devid = 0, revid = 0;
    if (!ampRead(amp, CS_DEVID, &devid) || !ampRead(amp, CS_REVID, &revid) || devid != CS_CHIP_ID) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "wrong chip ID after reset");
        return false;
    }
    if ((revid & 0xff) != CS_REVID_B2) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "not a B2 revision; errata unknown");
        return false;
    }

    // Errata and factory trims need the test key.
    ampWrite(amp, CS_TEST_KEY_CTL, 0x55);
    ampWrite(amp, CS_TEST_KEY_CTL, 0xAA);
    bool ok = ampWriteSeq(amp, kErrataB2, ARRAY_COUNT(kErrataB2)) &&
              ampWrite(amp, CS_DSP1_CCM_CORE_CTRL, 0) &&
              ampOTPUnpack(amp);
    ampWrite(amp, CS_TEST_KEY_CTL, 0xCC);
    ampWrite(amp, CS_TEST_KEY_CTL, 0x33);
    if (!ok) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "errata/OTP unpack failed");
        return false;
    }

    // Muted until playback.
    ampWrite(amp, CS_AMP_GAIN_CTRL, 0);
    ampWrite(amp, CS_AMP_DIG_VOL_CTRL, CS_VOL_MUTE);

    // Internal boost (cs35l41_boost_config).
    ok = ampUpdate(amp, CS_BSTCVRT_COEFF, 0xffff, (BST_K2 << 8) | BST_K1) &&
         ampUpdate(amp, CS_BSTCVRT_SLOPE_LBST, 0xff03, (BST_SLOPE << 8) | BST_LBST) &&
         ampUpdate(amp, CS_BSTCVRT_PEAK_CUR, 0x7f, BST_IPK) &&
         ampUpdate(amp, CS_PWR_CTRL2, CS_BST_EN_MASK, CS_BST_EN_DEFAULT);
    if (!ok) {
        snprintf(key, sizeof(key), "amp%d-error", amp);
        setStr(key, "boost config failed");
        return false;
    }

    // Channel: amp 0 = left (slot 0), amp 1 = right (slot 1).
    ampUpdate(amp, CS_SP_FRAME_RX_SLOT, 0x3f, (uint32_t)amp);

    snprintf(key, sizeof(key), "amp%d-result", amp);
    setStr(key, "configured");
    return true;
}

bool G814Amp::ampsInit() {
    fAmpsReady = false;

    // Shared reset: low 2 ms, then high (cs35l41_hda_common_probe).
    padSetTX(kPads[PAD_CS2], true);
    padSetTX(kPads[PAD_RESET], false);
    IOSleep(3);
    padSetTX(kPads[PAD_RESET], true);
    IOSleep(3);

    bool ok = ampInit(0) && ampInit(1);

    // Speaker-protection firmware; if either amp fails, both use the no-DSP path.
    fDSP = false;
    if (ok && fUseFirmware) {
        bool l = dspStart(0);
        bool r = l && dspStart(1);
        if (l && r) {
            fDSP = true;
        } else {
            if (l)
                dspStop(0);
            // Firmware load left the amps in an unknown state: set them up again plainly.
            padSetTX(kPads[PAD_RESET], false);
            IOSleep(3);
            padSetTX(kPads[PAD_RESET], true);
            IOSleep(3);
            ok = ampInit(0) && ampInit(1);
        }
    }
    setStr("Mode", fDSP ? "firmware (speaker protection, 17.5 dB gain)" : "no firmware (4.5 dB gain)");
    fAmpsReady = ok;
    return ok;
}

// AMP_VOL_PCM: 11-bit two's complement in 1/8 dB at bits [13:3]; bit 15 = HPF enable.
uint32_t G814Amp::volumeReg() const {
    int steps = fVolumeDB * 8;
    uint32_t field = (uint32_t)steps & 0x7ff;
    return 0x8000u | (field << 3);
}

// ---------------------------------------------------------------------------
// Stage 3: Cirrus speaker-protection firmware on each amp's Halo DSP
// (drivers/firmware/cirrus/cs_dsp.c and cs35l41_hda.c: cs35l41_smart_amp).

// DSP memory windows (cs35l41_dsp1_regions) and region types (wmfw.h).
#define DSP_PMEM_PACKED     0x03800000u
#define DSP_XMEM_PACKED     0x02000000u
#define DSP_YMEM_PACKED     0x02C00000u
#define DSP_XMEM_UNPACKED   0x02800000u
#define DSP_YMEM_UNPACKED   0x03400000u
#define DSP_CTRL_BASE       0x02B80000u
#define HALO_CCM_CORE_CTRL  (DSP_CTRL_BASE + 0x41000)
#define HALO_MPU_BASE       (DSP_CTRL_BASE + 0x43000)
#define HALO_MPU_LOCK_CFG   (DSP_CTRL_BASE + 0x43140)
#define HALO_CORE_RESET     0x200
#define HALO_CORE_EN        0x1
#define CS_DSP_MBOX_2       0x00013004
#define CS_DSP_VIRT1_MBOX_1 0x00013020
#define MBOX_CMD_PAUSE      1
#define MBOX_CMD_RESUME     2
#define MBOX_STS_RUNNING    0
#define MBOX_STS_PAUSED     1
#define CS_VMON_IMON_EN     0x3000
#define CS_ASP_TX1_EN       0x1
#define CS_GAIN_PCM_DSP     ((17u << 5) | 19u)  // DEFAULT_AMP_GAIN_PCM 17.5 dB, PDM 19.5 dB

enum { WMFW_ADSP2_XM = 5, WMFW_ADSP2_YM = 6, WMFW_HALO_PM_PACKED = 0x10,
       WMFW_HALO_XM_PACKED = 0x11, WMFW_HALO_YM_PACKED = 0x12, WMFW_ABSOLUTE = 0xf0 };

static uint32_t dspRegionBase(unsigned type) {
    switch (type) {
    case WMFW_HALO_PM_PACKED: return DSP_PMEM_PACKED;
    case WMFW_HALO_XM_PACKED: return DSP_XMEM_PACKED;
    case WMFW_HALO_YM_PACKED: return DSP_YMEM_PACKED;
    case WMFW_ADSP2_XM: return DSP_XMEM_UNPACKED;
    case WMFW_ADSP2_YM: return DSP_YMEM_UNPACKED;
    default: return 0;
    }
}

// cs_dsp_halo_region_to_reg: DSP word offset -> register address.
static uint32_t dspRegionReg(unsigned type, uint32_t offset) {
    uint32_t base = dspRegionBase(type);
    switch (type) {
    case WMFW_ADSP2_XM:
    case WMFW_ADSP2_YM: return base + offset * 4;
    case WMFW_HALO_XM_PACKED:
    case WMFW_HALO_YM_PACKED: return (base + offset * 3) & ~3u;
    case WMFW_HALO_PM_PACKED: return base + offset * 5;
    default: return 0;
    }
}

static uint32_t le32(const uint8_t *p) { return OSReadLittleInt32(p, 0); }
static uint16_t le16(const uint8_t *p) { return OSReadLittleInt16(p, 0); }

// regmap raw write: one SPI frame per chunk, the amp auto-increments the address.
bool G814Amp::ampWriteBlock(int amp, uint32_t reg, const uint8_t *data, size_t len) {
    static uint8_t frame[6 + 512];
    while (len) {
        size_t n = len > 512 ? 512 : len;
        memset(frame, 0, 6);
        OSWriteBigInt32(frame, 0, reg & 0x7fffffffu);
        memcpy(frame + 6, data, n);
        if (!spiFrame(amp, frame, nullptr, 6 + n))
            return false;
        reg += (uint32_t)n;
        data += n;
        len -= n;
    }
    return true;
}

// cs_dsp_load: write every memory region of the .wmfw program.
bool G814Amp::dspLoadWMFW(int amp, const uint8_t *fw, size_t size) {
    if (size < 40 || memcmp(fw, "WMFW", 4) || fw[10] != 4 /* Halo */ || fw[11] != 3)
        return false;
    size_t pos = le32(fw + 4);                  // header + sizes + footer
    while (pos + 8 <= size) {
        uint32_t word = le32(fw + pos), len = le32(fw + pos + 4);
        if (len > size - pos - 8)
            return false;
        unsigned type = word >> 24;
        uint32_t reg = 0;
        if (type == WMFW_ABSOLUTE)
            reg = word & 0xffffff;
        else if (dspRegionBase(type))
            reg = dspRegionReg(type, word & 0xffffff);
        if (reg && (len % 4 || !ampWriteBlock(amp, reg, fw + pos + 8, len)))
            return false;
        pos += 8 + len;
    }
    return true;
}

// cs_dsp_halo_setup_algs: read the algorithm list the firmware put at the start of XM.
bool G814Amp::dspReadAlgs(int amp) {
    fAlgCount[amp] = 0;
    uint32_t hdr[10];
    for (int i = 0; i < 10; i++)
        if (!ampRead(amp, DSP_XMEM_UNPACKED + i * 4, &hdr[i]))
            return false;
    uint32_t n = hdr[9];
    if (n == 0 || n > kMaxAlgs)
        return false;
    uint32_t end = 0;
    if (!ampRead(amp, DSP_XMEM_UNPACKED + (10 + n * 6) * 4, &end) || (end & 0xffffff) != 0xbedead)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a[6];
        for (int j = 0; j < 6; j++)
            if (!ampRead(amp, DSP_XMEM_UNPACKED + (10 + i * 6 + j) * 4, &a[j]))
                return false;
        fAlgs[amp][i] = { a[0], a[2], a[4] };   // id, xm_base, ym_base
    }
    fAlgCount[amp] = n;
    char key[32];
    snprintf(key, sizeof(key), "amp%d-dsp-algs", amp);
    setNum(key, n);
    return true;
}

// cs_dsp_load_coeff: write the tuning (.bin) blocks into each algorithm's memory.
bool G814Amp::dspLoadBin(int amp, const uint8_t *bin, size_t size) {
    if (size < 16 || memcmp(bin, "WMDR", 4))
        return false;
    size_t pos = le32(bin + 4);
    int written = 0;
    while (pos + 20 <= size) {
        const uint8_t *b = bin + pos;
        uint32_t offset = le16(b), type = le16(b + 2), id = le32(b + 4), len = le32(b + 16);
        if (len > size - pos - 20)
            return false;
        if (type >= 0xf400) {                   // *_LONG variants carry a 32-bit offset
            offset = le32(b + 12);
            type &= 0xff;
        }
        uint32_t reg = 0;
        if (type == (WMFW_ABSOLUTE << 8)) {
            reg = offset;
        } else if (dspRegionBase(type)) {
            for (uint32_t i = 0; i < fAlgCount[amp]; i++) {
                if (fAlgs[amp][i].id != id)
                    continue;
                bool ym = type == WMFW_ADSP2_YM || type == WMFW_HALO_YM_PACKED;
                reg = dspRegionReg(type, ym ? fAlgs[amp][i].ym : fAlgs[amp][i].xm) + offset;
                break;
            }
            if (!reg)
                return false;                   // tuning for an algorithm the firmware lacks
        }
        if (reg) {
            if (len % 4 || !ampWriteBlock(amp, reg, b + 20, len))
                return false;
            written++;
        }
        pos += (20 + len + 3) & ~3u;
    }
    char key[32];
    snprintf(key, sizeof(key), "amp%d-dsp-tuning-blocks", amp);
    setNum(key, (uint32_t)written);
    return written > 0;
}

bool G814Amp::dspMailbox(int amp, uint32_t cmd, uint32_t want) {
    ampWrite(amp, CS_DSP_VIRT1_MBOX_1, cmd);
    for (int i = 0; i < 20; i++) {
        uint32_t sts = 0;
        IOSleep(1);
        if (!ampRead(amp, CS_DSP_MBOX_2, &sts))
            continue;
        if (sts == want)
            return true;
        if (sts == 0xffffffff || sts == 0x00ffffff)
            return false;                       // CSPL error
    }
    return false;
}

// cs35l41_smart_amp: load, start and pause the firmware on one amp.
bool G814Amp::dspStart(int amp) {
    char key[48];
    snprintf(key, sizeof(key), "amp%d-dsp", amp);
    // Tuning depends on which speakers ASUS fitted: GPIO GPP_G0 reads the speaker ID.
    bool spk1 = (padDW0(kPads[PAD_SPKID]) & PAD_RX) != 0;
    setNum("speaker-id", spk1 ? 1 : 0);
    const uint8_t *bin = amp == 0 ? (spk1 ? fw_bin_spkid1_left : fw_bin_spkid0_left)
                                  : (spk1 ? fw_bin_spkid1_right : fw_bin_spkid0_right);
    size_t binLen = amp == 0 ? (spk1 ? fw_bin_spkid1_left_len : fw_bin_spkid0_left_len)
                             : (spk1 ? fw_bin_spkid1_right_len : fw_bin_spkid0_right_len);

    if (!dspLoadWMFW(amp, fw_wmfw, fw_wmfw_len)) {
        setStr(key, "program load failed");
        return false;
    }
    if (!dspReadAlgs(amp)) {
        setStr(key, "no algorithm list after load");
        return false;
    }
    if (!dspLoadBin(amp, bin, binLen)) {
        setStr(key, "tuning load failed");
        return false;
    }

    // cs35l41_write_fs_errata: all DSP RX/TX rates to 48 kHz.
    for (uint32_t r = 0; r < 8; r++) {
        ampWrite(amp, 0x02B80080 + r * 8, 1);
        ampWrite(amp, 0x02B80280 + r * 8, 1);
    }

    // cs_dsp_halo_configure_mpu with lock_regions = 0xffffffff, then start the core.
    ampWrite(amp, HALO_MPU_LOCK_CFG, 0x5555);
    ampWrite(amp, HALO_MPU_LOCK_CFG, 0xAAAA);
    static const uint32_t kMPUBlocks[4] = { 0x00, 0x18, 0x30, 0x48 };
    for (uint32_t b : kMPUBlocks) {
        ampWrite(amp, HALO_MPU_BASE + b + 0x00, 0xffffffff);   // XMEM
        ampWrite(amp, HALO_MPU_BASE + b + 0x04, 0xffffffff);   // YMEM
        ampWrite(amp, HALO_MPU_BASE + b + 0x08, 0xffffffff);   // WINDOW
        ampWrite(amp, HALO_MPU_BASE + b + 0x0C, 0xffffffff);   // XREG
        ampWrite(amp, HALO_MPU_BASE + b + 0x14, 0xffffffff);   // YREG
    }
    ampWrite(amp, HALO_MPU_LOCK_CFG, 0);
    ampUpdate(amp, HALO_CCM_CORE_CTRL, HALO_CORE_RESET | HALO_CORE_EN, HALO_CORE_RESET | HALO_CORE_EN);
    ampUpdate(amp, HALO_CCM_CORE_CTRL, HALO_CORE_RESET, 0);

    IOSleep(20);                                // the firmware boots in well under 15 ms
    uint32_t sts = 0xdead;
    ampRead(amp, CS_DSP_MBOX_2, &sts);
    snprintf(key, sizeof(key), "amp%d-dsp-boot-status", amp);
    setNum(key, sts);
    snprintf(key, sizeof(key), "amp%d-dsp", amp);
    if (sts != MBOX_STS_RUNNING && sts != MBOX_STS_PAUSED) {
        setStr(key, "firmware did not start");
        dspStop(amp);
        return false;
    }
    if (!dspMailbox(amp, MBOX_CMD_PAUSE, MBOX_STS_PAUSED)) {
        setStr(key, "firmware did not pause");
        dspStop(amp);
        return false;
    }
    setStr(key, "running (paused until playback)");
    return true;
}

// cs_dsp_halo_stop_core.
void G814Amp::dspStop(int amp) {
    ampUpdate(amp, HALO_CCM_CORE_CTRL, HALO_CORE_EN, 0);
    ampUpdate(amp, DSP_CTRL_BASE + 0x10, 1, 1);    // HALO_CORE_SOFT_RESET
}

// cs35l41_hda_play_start/play_done and pause_start/pause_done. With the firmware the
// DAC is fed from the DSP and the amps run at the tuned gain; otherwise the no-DSP
// path (DAC from ASP RX1, low gain) is used.
void G814Amp::ampsPlay(bool on) {
    char key[48];
    if (on) {
        for (int amp = 0; amp < 2; amp++) {
            ampWriteSeq(amp, kHDAConfig, ARRAY_COUNT(kHDAConfig));
            if (fDSP) {
                ampWrite(amp, CS_SP_HIZ_CTRL, 0x3);
                ampWrite(amp, CS_DAC_PCM1_SRC, 0x32);       // DSP1TX1
                ampWrite(amp, CS_ASP_TX3_SRC, 0x28);        // VPMON
                ampWrite(amp, CS_ASP_TX4_SRC, 0x29);        // VBSTMON
                ampWrite(amp, CS_DSP1_RX6_SRC, 0x29);       // VBSTMON
                ampWrite(amp, CS_DSP1_RX5_SRC, 0x28);       // VPMON (internal boost)
                ampWrite(amp, CS_SP_ENABLES, CS_ASP_RX1_EN | CS_ASP_TX1_EN);
                ampUpdate(amp, CS_PWR_CTRL2, CS_VMON_IMON_EN, CS_VMON_IMON_EN);
                dspMailbox(amp, MBOX_CMD_RESUME, MBOX_STS_RUNNING);
            }
            ampUpdate(amp, CS_PWR_CTRL2, CS_AMP_EN, CS_AMP_EN);
        }
        for (int amp = 0; amp < 2; amp++) {
            uint32_t st = 0;
            ampUpdate(amp, CS_PWR_CTRL1, CS_GLOBAL_EN, CS_GLOBAL_EN);
            bool up = ampPoll(amp, CS_IRQ1_STATUS1, CS_PUP_DONE, 100, &st);
            ampWrite(amp, CS_IRQ1_STATUS1, CS_PUP_DONE);
            snprintf(key, sizeof(key), "amp%d-powerup", amp);
            setStr(key, up ? "PUP_DONE" : "timeout waiting for PUP_DONE");
        }
        for (int amp = 0; amp < 2; amp++) {
            ampWrite(amp, CS_AMP_DIG_VOL_CTRL, volumeReg());
            ampWrite(amp, CS_AMP_GAIN_CTRL, fDSP ? CS_GAIN_PCM_DSP : CS_GAIN_PCM_HDA);
        }
        fPlaying = true;
        setStr("Status", "playing");
    } else {
        for (int amp = 0; amp < 2; amp++) {
            ampWrite(amp, CS_AMP_GAIN_CTRL, 0);
            ampWrite(amp, CS_AMP_DIG_VOL_CTRL, CS_VOL_MUTE);
        }
        for (int amp = 0; amp < 2; amp++) {
            ampUpdate(amp, CS_PWR_CTRL1, CS_GLOBAL_EN, 0);
            ampPoll(amp, CS_IRQ1_STATUS1, CS_PDN_DONE, 100, nullptr);
            ampWrite(amp, CS_IRQ1_STATUS1, CS_PDN_DONE);
            ampUpdate(amp, CS_PWR_CTRL2, CS_AMP_EN, 0);
            if (fDSP) {
                dspMailbox(amp, MBOX_CMD_PAUSE, MBOX_STS_PAUSED);
                ampUpdate(amp, CS_PWR_CTRL2, CS_VMON_IMON_EN, 0);
            }
            // Release any protection errors so the next start is not stuck in safe mode.
            ampWrite(amp, CS_PROTECT_REL_ERR_IGN, 0);
            ampWrite(amp, CS_PROTECT_REL_ERR_IGN, CS_ERR_RLS_ALL);
            ampWrite(amp, CS_PROTECT_REL_ERR_IGN, 0);
        }
        fPlaying = false;
        setStr("Status", fEnabled ? "enabled, waiting for playback" : "idle");
    }
}

// ---------------------------------------------------------------------------

bool G814Amp::hwPrepare(bool publishPads) {
    char key[64], why[96];
    if (fHWReady)
        return true;
    if (!mapGPIO()) {
        setStr("Status", "failed: GPIO controller not mapped");
        return false;
    }
    if (publishPads) {
        for (int i = 0; i < PAD_COUNT; i++) {
            const Pad &p = kPads[i];
            uint32_t off = padOffset(p);
            snprintf(key, sizeof(key), "pad-%s-%s-dw0", p.name, p.pch);
            setNum(key, gpioRead(p.community, off));
            snprintf(key, sizeof(key), "pad-%s-%s-owner", p.name, p.pch);
            setNum(key, padOwner(p));
        }
        setNum("speaker-id", (padDW0(kPads[PAD_SPKID]) & PAD_RX) ? 1 : 0);
    }
    static const int kControlPads[] = { PAD_RESET, PAD_CS2 };
    for (int i : kControlPads) {
        if (!padIsHostOutput(kPads[i], why, sizeof(why))) {
            setStr("Status", "stopped: a control pad is not a usable output");
            setStr("Reason", why);
            LOG("stopped: %s", why);
            return false;
        }
    }
    padSetTX(kPads[PAD_CS2], true);
    if (!spiInit()) {
        setStr("Status", "failed: SPI controller did not respond");
        return false;
    }
    fHWReady = true;
    return true;
}

void G814Amp::runProbe() {
    char key[64];
    setStr("Status", "probing");
    fHWReady = false;
    if (!hwPrepare(true))
        return;
    if (!(padDW0(kPads[PAD_RESET]) & PAD_TX)) {
        padSetTX(kPads[PAD_RESET], true);
        IOSleep(20);
    }
    int found = 0;
    for (int amp = 0; amp < 2; amp++) {
        uint32_t devid = 0, revid = 0;
        bool ok = ampRead(amp, CS_DEVID, &devid) && ampRead(amp, CS_REVID, &revid);
        snprintf(key, sizeof(key), "amp%d-devid", amp);
        setNum(key, devid);
        snprintf(key, sizeof(key), "amp%d-revid", amp);
        setNum(key, revid);
        if (ok && devid == CS_CHIP_ID)
            found++;
    }
    setStr("Status", found == 2 ? "probe: both amps answered" :
                     found == 1 ? "probe: one amp answered" : "probe: no amp answered");
}

// True while any NMXAudio output engine on the Realtek codec (PCI 00:1f.3) runs.
bool G814Amp::playbackActive() {
    OSDictionary *match = IOService::serviceMatching("IOAudioEngine");
    if (!match)
        return false;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it)
        return false;
    bool active = false;
    while (OSObject *obj = it->getNextObject()) {
        IOService *eng = OSDynamicCast(IOService, obj);
        if (!eng)
            continue;
        OSString *uid = OSDynamicCast(OSString, eng->getProperty("IOAudioEngineGlobalUniqueID"));
        if (!uid)
            continue;
        const char *s = uid->getCStringNoCopy();
        size_t len = strlen(s);
        if (strncmp(s, "NMXAudio:00:1f.3/", 17) != 0 || len < 4 || strcmp(s + len - 4, ":out") != 0)
            continue;
        OSNumber *state = OSDynamicCast(OSNumber, eng->getProperty("IOAudioEngineState"));
        if (state && state->unsigned32BitValue() == 1) {
            active = true;
            break;
        }
    }
    it->release();
    return active;
}

// A register we set during ampsInit; after sleep (or anything that resets the amps or
// the SPI controller) it reads back wrong and the amps are set up again.
bool G814Amp::ampsHealthy() {
    uint32_t v = 0;
    return ampRead(0, CS_BSTCVRT_PEAK_CUR, &v) && (v & 0x7f) == BST_IPK &&
           ampRead(1, CS_BSTCVRT_PEAK_CUR, &v) && (v & 0x7f) == BST_IPK;
}

void G814Amp::timerFired(IOTimerEventSource *sender) {
    IOLockLock(fLock);

    // Boot-time start: retry every 2 s until the amps come up.
    if (fAutoTries > 0 && !fEnabled) {
        fAutoTries--;
        setEnabledLocked(true);
        if (!fEnabled) {
            if (fAutoTries > 0)
                sender->setTimeoutMS(2000);
            IOLockUnlock(fLock);
            return;
        }
        fAutoTries = 0;
    }

    if (fEnabled) {
        fTicks++;
        if (fTicks % 13 == 0 && !ampsHealthy()) {
            // Woke from sleep or lost power: redo the SPI controller and the amps.
            fPlaying = false;
            fHWReady = false;
            bool ok = hwPrepare(false) && ampsInit();
            setNum("recoveries", ++fRecoveries);
            setStr("Status", ok ? "recovered, waiting for playback" : "recovery failed, retrying");
        }
    }

    if (fEnabled && fAmpsReady) {
        bool want = playbackActive();
        if (want != fPlaying) {
            if (want)
                sendPlaybackVerbs();
            ampsPlay(want);
            publishStatus();
        }

        // Track protection errors; while playing, keep the full status current.
        for (int amp = 0; amp < 2; amp++) {
            uint32_t st = 0;
            if (ampRead(amp, CS_IRQ1_STATUS1, &st)) {
                fErrors[amp] |= st & (CS_ERR_MASK | 0x1C0 | 0x8000);
                char key[48];
                snprintf(key, sizeof(key), "amp%d-errors-seen", amp);
                setNum(key, fErrors[amp]);
            }
        }
        if (fPlaying) {
            publishStatus();
            setNum("playing-status-updates", ++fStatusUpdates);
            // NMXAudio may rewrite the pin later; repeat the verbs every ~2 s.
            if (fStatusUpdates % 13 == 0)
                sendPlaybackVerbs();
        }
    }
    if (fEnabled)
        sender->setTimeoutMS(150);
    IOLockUnlock(fLock);
}

void G814Amp::setEnabled(bool on) {
    IOLockLock(fLock);
    fAutoTries = 0;
    setEnabledLocked(on);
    IOLockUnlock(fLock);
}

void G814Amp::setEnabledLocked(bool on) {
    if (on && !fEnabled) {
        setStr("Status", "setting up amps");
        if (hwPrepare(false) && ampsInit()) {
            fEnabled = true;
            setStr("Status", "enabled, waiting for playback");
            fTimer->setTimeoutMS(150);
        } else if (fHWReady) {
            setStr("Status", "failed: amp setup (see ampN-error)");
        }
    } else if (!on && fEnabled) {
        fEnabled = false;
        fTimer->cancelTimeout();
        if (fPlaying)
            ampsPlay(false);
        setStr("Status", "idle");
    }
}

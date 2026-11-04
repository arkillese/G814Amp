// Sends commands to G814Amp:
//   probe-tool probe        read the pads and both amps' chip IDs
//   probe-tool enable       set up the amps and let them follow playback
//   probe-tool disable      power the amps down
//   probe-tool volume <dB>  amp volume in dB (the driver caps it)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "probe";
    CFStringRef key;
    CFTypeRef value;
    int db = 0;

    if (!strcmp(cmd, "probe")) {
        key = CFSTR("Probe");
        value = kCFBooleanTrue;
    } else if (!strcmp(cmd, "enable")) {
        key = CFSTR("Enable");
        value = kCFBooleanTrue;
    } else if (!strcmp(cmd, "disable")) {
        key = CFSTR("Enable");
        value = kCFBooleanFalse;
    } else if (!strcmp(cmd, "firmware") && argc > 2) {
        key = CFSTR("Firmware");
        value = strcmp(argv[2], "off") ? kCFBooleanTrue : kCFBooleanFalse;
    } else if (!strcmp(cmd, "volume") && argc > 2) {
        db = atoi(argv[2]);
        key = CFSTR("VolumeDB");
        value = CFNumberCreate(NULL, kCFNumberIntType, &db);
    } else if (!strcmp(cmd, "read") && argc > 2) {
        unsigned int addr = (unsigned int)strtoul(argv[2], NULL, 0);
        key = CFSTR("ReadReg");
        value = CFNumberCreate(NULL, kCFNumberIntType, &addr);
    } else if (!strcmp(cmd, "write") && argc > 4) {
        unsigned int nums[3] = { (unsigned int)strtoul(argv[2], NULL, 0),
                                 (unsigned int)strtoul(argv[3], NULL, 0),
                                 (unsigned int)strtoul(argv[4], NULL, 0) };
        CFNumberRef items[3];
        for (int i = 0; i < 3; i++)
            items[i] = CFNumberCreate(NULL, kCFNumberIntType, &nums[i]);
        key = CFSTR("WriteReg");
        value = CFArrayCreate(NULL, (const void **)items, 3, &kCFTypeArrayCallBacks);
    } else if (!strcmp(cmd, "verb") && argc > 2) {
        unsigned int verb = (unsigned int)strtoul(argv[2], NULL, 0);
        key = CFSTR("CodecVerb");
        value = CFNumberCreate(NULL, kCFNumberSInt64Type, &(long long){ verb });
    } else if (!strcmp(cmd, "playback-verbs")) {
        int n = argc - 2;
        CFNumberRef items[64];
        if (n > 64)
            n = 64;
        for (int i = 0; i < n; i++) {
            long long v = (long long)strtoul(argv[2 + i], NULL, 0);
            items[i] = CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
        }
        key = CFSTR("PlaybackVerbs");
        value = CFArrayCreate(NULL, (const void **)items, n, &kCFTypeArrayCallBacks);
    } else {
        fprintf(stderr, "usage: %s probe | enable | disable | volume <dB> | read <addr> |"
                " write <amp> <addr> <value> | verb <hex> | playback-verbs [<hex> ...]\n", argv[0]);
        return 2;
    }

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("G814Amp"));
    if (!svc) {
        fprintf(stderr, "G814Amp is not loaded.\n");
        return 1;
    }
    kern_return_t kr = IORegistryEntrySetCFProperty(svc, key, value);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "%s failed: 0x%x\n", cmd, kr);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? 0 : 1;
}

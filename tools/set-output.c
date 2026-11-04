// set-output "<name substring>": make the first output device whose name contains it the
// default (and system) output. With no argument, lists output devices.
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>

static int name_of(AudioObjectID dev, char *buf, size_t len) {
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &name) || !name)
        return 0;
    CFStringGetCString(name, buf, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(name);
    return 1;
}

static int has_output(AudioObjectID dev) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) == 0 && size > 0;
}

int main(int argc, char **argv) {
    AudioObjectPropertyAddress all = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, NULL, &size);
    AudioObjectID devs[64];
    if (size > sizeof(devs))
        size = sizeof(devs);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, NULL, &size, devs);

    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        char name[256];
        if (!has_output(devs[i]) || !name_of(devs[i], name, sizeof(name)))
            continue;
        if (argc < 2) {
            printf("%s\n", name);
            continue;
        }
        if (!strstr(name, argv[1]))
            continue;
        AudioObjectPropertyAddress def = { kAudioHardwarePropertyDefaultOutputDevice,
                                           kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectPropertyAddress sys = { kAudioHardwarePropertyDefaultSystemOutputDevice,
                                           kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        OSStatus e1 = AudioObjectSetPropertyData(kAudioObjectSystemObject, &def, 0, NULL, sizeof(devs[i]), &devs[i]);
        OSStatus e2 = AudioObjectSetPropertyData(kAudioObjectSystemObject, &sys, 0, NULL, sizeof(devs[i]), &devs[i]);
        printf("default output -> %s (%d/%d)\n", name, (int)e1, (int)e2);
        return e1 || e2;
    }
    if (argc >= 2) {
        fprintf(stderr, "no output device matching \"%s\"\n", argv[1]);
        return 1;
    }
    return 0;
}

/*
 * DroidScreen macOS - Menu Bar Application
 *
 * A macOS status-bar (menu bar) app that manages a virtual display,
 * captures it via ScreenCaptureKit, encodes with VideoToolbox, and
 * streams HEVC to an Android tablet over USB.
 *
 * No dock icon -- runs entirely from the system menu bar.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <IOKit/hidsystem/ev_keymap.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <ApplicationServices/ApplicationServices.h>

#include "sck_capturer.h"
#include "vt_encoder.h"
#include "virtual_display.h"
#include "droidscreen/deck_manager.h"
#include "droidscreen/pipeline.h"
#include "mouse_injector_mac.h"
#include "droidscreen/server.h"
#include "touch_injector_mac.h"

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/handshake.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <unordered_map>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <execinfo.h>

// =============================================================================
#pragma mark - Now Playing Helper
// =============================================================================

static void post_media_key_event(int keyType) {
    @autoreleasepool {
        auto post = ^(int keyState) {
            NSInteger data1 = ((keyType & 0xFFFF) << 16) | ((keyState & 0xFF) << 8);
            NSEvent* event = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                                location:NSZeroPoint
                                           modifierFlags:0
                                               timestamp:0
                                            windowNumber:0
                                                 context:nil
                                                 subtype:NX_SUBTYPE_AUX_CONTROL_BUTTONS
                                                   data1:data1
                                                   data2:-1];
            if (event) {
                CGEventPost(kCGHIDEventTap, event.CGEvent);
            }
        };

        // 0xA = key down, 0xB = key up for system media keys.
        post(0xA);
        post(0xB);
    }
}

static std::string json_escape_nsstring(NSString* s) {
    if (!s) return "";
    std::string out;
    const char* utf8 = [s UTF8String];
    if (!utf8) return "";
    for (const char* p = utf8; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += *p;     break;
        }
    }
    return out;
}

static NSString* mediaTrackKeyForSnapshot(NSDictionary* snapshot) {
    NSString* title = snapshot[@"title"];
    if (title.length == 0) return nil;
    NSString* artist = snapshot[@"artist"] ?: @"";
    NSString* album = snapshot[@"album"] ?: @"";
    return [NSString stringWithFormat:@"%@\n%@\n%@", title, artist, album];
}

static NSDictionary* fetchNowPlayingSnapshot(BOOL* didTimeoutOut = nil) {
    @autoreleasepool {
        if (didTimeoutOut) *didTimeoutOut = NO;
        NSBundle* bundle = [NSBundle mainBundle];
        NSString* scriptPath = [bundle pathForResource:@"mediaremote-mini" ofType:@"pl"];
        NSString* dylibPath = [bundle pathForResource:@"MediaRemoteMini" ofType:@"dylib"];
        if (scriptPath.length == 0 || dylibPath.length == 0) {
            NSLog(@"[deck] MediaRemote helper resources are missing");
            return nil;
        }

        int stdoutPipe[2] = {-1, -1};
        int stderrPipe[2] = {-1, -1};
        if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
            NSLog(@"[deck] Failed to create pipes for MediaRemote helper");
            if (stdoutPipe[0] >= 0) close(stdoutPipe[0]);
            if (stdoutPipe[1] >= 0) close(stdoutPipe[1]);
            if (stderrPipe[0] >= 0) close(stderrPipe[0]);
            if (stderrPipe[1] >= 0) close(stderrPipe[1]);
            return nil;
        }

        pid_t pid = fork();
        if (pid < 0) {
            NSLog(@"[deck] Failed to fork MediaRemote helper");
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            close(stderrPipe[0]);
            close(stderrPipe[1]);
            return nil;
        }
        if (pid == 0) {
            dup2(stdoutPipe[1], STDOUT_FILENO);
            dup2(stderrPipe[1], STDERR_FILENO);
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            close(stderrPipe[0]);
            close(stderrPipe[1]);
            execl("/usr/bin/perl", "perl",
                  [scriptPath fileSystemRepresentation],
                  [dylibPath fileSystemRepresentation],
                  "adapter_get_env",
                  (char*)nullptr);
            _exit(127);
        }

        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        const NSTimeInterval timeoutSeconds = 1.25;
        BOOL timedOut = NO;
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
        std::string stdoutBuf;
        std::string stderrBuf;
        bool stdoutOpen = true;
        bool stderrOpen = true;
        int childStatus = 0;
        bool childExited = false;

        while (stdoutOpen || stderrOpen || !childExited) {
            NSTimeInterval remaining = [deadline timeIntervalSinceNow];
            if (remaining <= 0) {
                timedOut = YES;
                break;
            }

            fd_set readfds;
            FD_ZERO(&readfds);
            int maxfd = -1;
            if (stdoutOpen) {
                FD_SET(stdoutPipe[0], &readfds);
                maxfd = MAX(maxfd, stdoutPipe[0]);
            }
            if (stderrOpen) {
                FD_SET(stderrPipe[0], &readfds);
                maxfd = MAX(maxfd, stderrPipe[0]);
            }

            struct timeval tv;
            tv.tv_sec = (int)remaining;
            tv.tv_usec = (int)((remaining - tv.tv_sec) * 1000000.0);
            int selectResult = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
            if (selectResult > 0) {
                char buffer[4096];
                if (stdoutOpen && FD_ISSET(stdoutPipe[0], &readfds)) {
                    ssize_t count = read(stdoutPipe[0], buffer, sizeof(buffer));
                    if (count > 0) stdoutBuf.append(buffer, (size_t)count);
                    else {
                        close(stdoutPipe[0]);
                        stdoutOpen = false;
                    }
                }
                if (stderrOpen && FD_ISSET(stderrPipe[0], &readfds)) {
                    ssize_t count = read(stderrPipe[0], buffer, sizeof(buffer));
                    if (count > 0) stderrBuf.append(buffer, (size_t)count);
                    else {
                        close(stderrPipe[0]);
                        stderrOpen = false;
                    }
                }
            } else if (selectResult < 0) {
                break;
            }

            pid_t waitResult = waitpid(pid, &childStatus, WNOHANG);
            if (waitResult == pid) {
                childExited = true;
                if (!stdoutOpen && !stderrOpen) break;
            }
        }

        if (timedOut) {
            timedOut = YES;
            kill(pid, SIGKILL);
            waitpid(pid, &childStatus, 0);
        } else if (!childExited) {
            waitpid(pid, &childStatus, 0);
        }
        if (stdoutOpen) close(stdoutPipe[0]);
        if (stderrOpen) close(stderrPipe[0]);

        NSString* stdoutStr = [[NSString alloc] initWithBytes:stdoutBuf.data()
                                                       length:stdoutBuf.size()
                                                     encoding:NSUTF8StringEncoding];
        NSString* stderrStr = [[NSString alloc] initWithBytes:stderrBuf.data()
                                                       length:stderrBuf.size()
                                                     encoding:NSUTF8StringEncoding];

        if (timedOut) {
            if (didTimeoutOut) *didTimeoutOut = YES;
            NSLog(@"[deck] MediaRemote helper timed out after %.0fms",
                  timeoutSeconds * 1000.0);
        }

        if ((timedOut || childStatus != 0) || stdoutStr.length == 0) {
            if (stderrStr.length > 0) {
                NSLog(@"[deck] MediaRemote helper failed: %@", stderrStr);
            }
            return nil;
        }

        NSString* trimmed = [stdoutStr stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (trimmed.length == 0 || [trimmed isEqualToString:@"null"]) return nil;

        NSData* jsonData = [trimmed dataUsingEncoding:NSUTF8StringEncoding];
        NSError* error = nil;
        id obj = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
        if (![obj isKindOfClass:[NSDictionary class]]) {
            if (error) {
                NSLog(@"[deck] Failed to parse helper JSON: %@", error);
            }
            return nil;
        }
        return (NSDictionary*)obj;
    }
}

static std::string buildMediaStateJSON(NSDictionary* snapshot, bool includeArtwork) {
    static NSString* cachedTrackKey = nil;
    static NSString* cachedArtworkB64 = nil;

    NSString* title = snapshot[@"title"];
    if (title.length == 0) {
        return "{\"playing\":false,\"title\":\"\",\"artist\":\"\","
               "\"progress\":0,\"duration_sec\":0}";
    }

    NSString* artist = snapshot[@"artist"] ?: @"";
    NSString* album = snapshot[@"album"] ?: @"";
    NSNumber* duration = snapshot[@"duration"] ?: @0;
    NSNumber* elapsed = snapshot[@"elapsedTimeNow"] ?: snapshot[@"elapsedTime"] ?: @0;
    NSNumber* playing = snapshot[@"playing"] ?: @NO;
    NSString* trackKey = [NSString stringWithFormat:@"%@\n%@\n%@", title, artist, album];
    NSString* artworkB64 = snapshot[@"artworkData"];

    if (artworkB64.length > 0) {
        cachedTrackKey = [trackKey copy];
        cachedArtworkB64 = [artworkB64 copy];
    } else if ([cachedTrackKey isEqualToString:trackKey] && cachedArtworkB64.length > 0) {
        artworkB64 = cachedArtworkB64;
    } else {
        cachedTrackKey = [trackKey copy];
        cachedArtworkB64 = nil;
    }

    double progress = (duration.doubleValue > 0.0)
        ? (elapsed.doubleValue / duration.doubleValue)
        : 0.0;
    progress = MAX(0.0, MIN(1.0, progress));

    std::string json = "{";
    json += "\"playing\":" + std::string(playing.boolValue ? "true" : "false");
    json += ",\"title\":\"" + json_escape_nsstring(title) + "\"";
    json += ",\"artist\":\"" + json_escape_nsstring(artist) + "\"";
    json += ",\"progress\":" + std::to_string(progress);
    json += ",\"duration_sec\":" + std::to_string((int)duration.doubleValue);
    if (includeArtwork && artworkB64.length > 0) {
        json += ",\"album_art_b64\":\"" + std::string([artworkB64 UTF8String]) + "\"";
    }
    json += "}";

    return json;
}

// =============================================================================
#pragma mark - Debug File Logger
// =============================================================================

static NSString* g_logFilePath = nil;
static FILE*     g_logFile     = nullptr;

/// Sets up file-based logging. All NSLog and stderr output goes to this file.
static void setup_debug_logging() {
    // Log to ~/Library/Logs/DroidScreen/droidscreen.log
    NSString* logDir = [@"~/Library/Logs/DroidScreen" stringByExpandingTildeInPath];
    [[NSFileManager defaultManager] createDirectoryAtPath:logDir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];

    // Rotate: keep previous log as .prev.log
    g_logFilePath = [logDir stringByAppendingPathComponent:@"droidscreen.log"];
    NSString* prevPath = [logDir stringByAppendingPathComponent:@"droidscreen.prev.log"];
    [[NSFileManager defaultManager] removeItemAtPath:prevPath error:nil];
    [[NSFileManager defaultManager] moveItemAtPath:g_logFilePath toPath:prevPath error:nil];

    g_logFile = fopen([g_logFilePath UTF8String], "w");
    if (g_logFile) {
        // Redirect stderr to our log file — catches NSLog, fprintf(stderr,...), etc.
        dup2(fileno(g_logFile), STDERR_FILENO);
        setvbuf(g_logFile, nullptr, _IOLBF, 0);  // line-buffered

        fprintf(stderr, "=== DroidScreen Debug Log ===\n");
        fprintf(stderr, "Started: %s\n",
                [[[NSDate date] description] UTF8String]);
        fprintf(stderr, "Log file: %s\n\n",
                [g_logFilePath UTF8String]);
    }
}

/// Signal/crash handler — writes backtrace to log before dying.
static void crash_signal_handler(int sig) {
    const char* signame = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: signame = "SIGSEGV"; break;
        case SIGBUS:  signame = "SIGBUS";  break;
        case SIGABRT: signame = "SIGABRT"; break;
        case SIGFPE:  signame = "SIGFPE";  break;
        case SIGILL:  signame = "SIGILL";  break;
    }

    // Write directly to log file (signal-safe as possible).
    if (g_logFile) {
        fprintf(g_logFile, "\n\n=== CRASH: signal %d (%s) ===\n", sig, signame);

        void* callstack[128];
        int frames = backtrace(callstack, 128);
        backtrace_symbols_fd(callstack, frames, fileno(g_logFile));

        fprintf(g_logFile, "\n=== END CRASH ===\n");
        fflush(g_logFile);
    }

    // Also dump to original stderr if possible.
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    backtrace_symbols_fd(callstack, frames, STDOUT_FILENO);

    // Re-raise to get the default handler (generates crash report).
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers() {
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGBUS,  crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGFPE,  crash_signal_handler);
    signal(SIGILL,  crash_signal_handler);
}

/// Set uncaught Objective-C exception handler.
static void objc_exception_handler(NSException* exception) {
    NSLog(@"\n\n=== UNCAUGHT EXCEPTION ===");
    NSLog(@"Name: %@", exception.name);
    NSLog(@"Reason: %@", exception.reason);
    NSLog(@"Stack:\n%@", [exception callStackSymbols]);
    NSLog(@"=== END EXCEPTION ===\n");

    if (g_logFile) fflush(g_logFile);

    // Re-throw to get the default crash report.
    @throw exception;
}

// =============================================================================
#pragma mark - macOS System Volume (CoreAudio)
// =============================================================================

/// Returns the default output audio device ID.
static AudioDeviceID getDefaultOutputDevice() {
    AudioDeviceID deviceId = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceId);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &deviceId);
    return deviceId;
}

/// Get the macOS system output volume (0.0 - 1.0).
static float getSystemVolume() {
    AudioDeviceID deviceId = getDefaultOutputDevice();
    if (deviceId == kAudioObjectUnknown) return 0.0f;

    AudioObjectPropertyAddress addr = {
        kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    Float32 volume = 0;
    UInt32 size = sizeof(volume);
    OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, NULL, &size, &volume);
    if (status != noErr) {
        fprintf(stderr, "[volume] getSystemVolume failed: %d\n", (int)status);
        return 0.0f;
    }
    return volume;
}

/// Set the macOS system output volume (0.0 - 1.0).
static void setSystemVolume(float volume) {
    AudioDeviceID deviceId = getDefaultOutputDevice();
    if (deviceId == kAudioObjectUnknown) return;

    Float32 vol = fminf(1.0f, fmaxf(0.0f, volume));
    AudioObjectPropertyAddress addr = {
        kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(deviceId, &addr, 0, NULL, sizeof(vol), &vol);
    if (status != noErr) {
        fprintf(stderr, "[volume] setSystemVolume failed: %d\n", (int)status);
    }
}

/// Check if the macOS system output is muted.
static bool getSystemMuted() {
    AudioDeviceID deviceId = getDefaultOutputDevice();
    if (deviceId == kAudioObjectUnknown) return false;

    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 muted = 0;
    UInt32 size = sizeof(muted);
    OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, NULL, &size, &muted);
    if (status != noErr) return false;
    return muted != 0;
}

static void setSystemMuted(bool muted) {
    AudioDeviceID deviceId = getDefaultOutputDevice();
    if (deviceId == kAudioObjectUnknown) return;

    UInt32 mutedValue = muted ? 1 : 0;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(
        deviceId, &addr, 0, NULL, sizeof(mutedValue), &mutedValue
    );
    if (status != noErr) {
        fprintf(stderr, "[volume] setSystemMuted failed: %d\n", (int)status);
    }
}

// =============================================================================
#pragma mark - Settings Keys (NSUserDefaults)
// =============================================================================

static NSString* const kSettingFPS          = @"DroidScreenFPS";
static NSString* const kSettingBitrate      = @"DroidScreenBitrate";      // kbps
static NSString* const kSettingResolution   = @"DroidScreenResolution";   // index
static NSString* const kSettingScale        = @"DroidScreenScale";        // index
static NSString* const kSettingPort         = @"DroidScreenPort";
static NSString* const kSettingDeckApps     = @"DroidScreenDeckApps";     // array of dicts

// Resolution presets: logical (point) resolution of the virtual display.
// This is what macOS shows as the display size — how much content fits.
// The stream is captured at this resolution regardless of HiDPI setting.
struct ResolutionPreset {
    uint32_t width;
    uint32_t height;
    const char* label;
};

static const ResolutionPreset kResolutions[] = {
    { 2560, 1600, "2560x1600 (Native)" },
    { 1920, 1200, "1920x1200"          },
    { 1600, 1000, "1600x1000"          },
    { 1280,  800, "1280x800"           },
};
static const int kResolutionCount = sizeof(kResolutions) / sizeof(kResolutions[0]);

// Bitrate presets in kbps.
static const int kBitrates[]     = { 5000, 10000, 15000, 20000, 25000, 30000 };
static const char* kBitrateLabels[] = {
    "5 Mbps", "10 Mbps", "15 Mbps", "20 Mbps", "25 Mbps", "30 Mbps"
};
static const int kBitrateCount = sizeof(kBitrates) / sizeof(kBitrates[0]);

// =============================================================================
#pragma mark - ADB Helpers
// =============================================================================

/// Finds the full path to the `adb` binary. Caches the result.
/// Searches common Homebrew/Android SDK locations + user's shell PATH.
static NSString* adb_find_path() {
    static NSString* cached = nil;
    if (cached) return cached;

    // Common locations to check.
    NSArray<NSString*>* candidates = @[
        @"/opt/homebrew/bin/adb",
        @"/usr/local/bin/adb",
        [@"~/Library/Android/sdk/platform-tools/adb" stringByExpandingTildeInPath],
        @"/Applications/Android Studio.app/Contents/platform-tools/adb",
    ];

    for (NSString* path in candidates) {
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:path]) {
            cached = path;
            NSLog(@"[ADB] Found adb at: %@", cached);
            return cached;
        }
    }

    // Last resort: ask a login shell to resolve it.
    @autoreleasepool {
        NSTask* task = [[NSTask alloc] init];
        task.launchPath = @"/bin/zsh";
        task.arguments  = @[@"-l", @"-c", @"which adb"];

        NSPipe* pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError  = [NSPipe pipe];

        @try {
            [task launch];
            [task waitUntilExit];
        } @catch (NSException*) {
            return nil;
        }

        NSData* data = [pipe.fileHandleForReading readDataToEndOfFile];
        NSString* output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        output = [output stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (output.length > 0 && [[NSFileManager defaultManager] isExecutableFileAtPath:output]) {
            cached = output;
            NSLog(@"[ADB] Found adb via shell: %@", cached);
            return cached;
        }
    }

    NSLog(@"[ADB] ERROR: adb not found!");
    return nil;
}

/// Runs an adb command using the resolved full path. Returns exit code.
static int adb_run(NSArray<NSString*>* args) {
    NSString* adbPath = adb_find_path();
    if (!adbPath) return -1;

    @autoreleasepool {
        NSTask* task = [[NSTask alloc] init];
        task.launchPath = adbPath;
        task.arguments  = args;
        task.standardOutput = [NSPipe pipe];
        task.standardError  = [NSPipe pipe];

        @try {
            [task launch];
            [task waitUntilExit];
        } @catch (NSException* e) {
            NSLog(@"[ADB] Exception running adb: %@", e);
            return -1;
        }

        return task.terminationStatus;
    }
}

/// Runs an adb command and returns its stdout output.
static NSString* adb_run_output(NSArray<NSString*>* args) {
    NSString* adbPath = adb_find_path();
    if (!adbPath) return @"";

    @autoreleasepool {
        NSTask* task = [[NSTask alloc] init];
        task.launchPath = adbPath;
        task.arguments  = args;

        NSPipe* pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError  = [NSPipe pipe];

        @try {
            [task launch];
            [task waitUntilExit];
        } @catch (NSException*) {
            return @"";
        }

        NSData* data = [pipe.fileHandleForReading readDataToEndOfFile];
        return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    }
}

static bool adb_forward_setup(uint16_t port) {
    NSString* tcpArg = [NSString stringWithFormat:@"tcp:%u", port];
    NSLog(@"[ADB] forward %@ %@", tcpArg, tcpArg);
    int ret = adb_run(@[@"forward", tcpArg, tcpArg]);
    if (ret != 0) {
        NSLog(@"[ADB] adb forward failed (exit %d)", ret);
        return false;
    }
    return true;
}

static void adb_forward_remove(uint16_t port) {
    NSString* tcpArg = [NSString stringWithFormat:@"tcp:%u", port];
    adb_run(@[@"forward", @"--remove", tcpArg]);
}

/// Queries the connected Android device's physical screen resolution via ADB.
/// Returns as landscape (width >= height). Returns {0,0} on failure.
static CGSize adb_device_resolution() {
    NSString* output = adb_run_output(@[@"shell", @"wm", @"size"]);
    // Output format: "Physical size: 1600x2560\n" (portrait: H×W)
    // May also contain "Override size: ..." — we want Physical.
    for (NSString* line in [output componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"Physical size:"]) {
            NSString* dims = [[line componentsSeparatedByString:@":"].lastObject
                              stringByTrimmingCharactersInSet:
                                  [NSCharacterSet whitespaceAndNewlineCharacterSet]];
            NSArray<NSString*>* parts = [dims componentsSeparatedByString:@"x"];
            if (parts.count == 2) {
                int a = [parts[0] intValue];
                int b = [parts[1] intValue];
                if (a > 0 && b > 0) {
                    // Return as landscape (wider dimension first).
                    int w = (a >= b) ? a : b;
                    int h = (a >= b) ? b : a;
                    NSLog(@"[ADB] Device resolution: %dx%d (landscape)", w, h);
                    return CGSizeMake(w, h);
                }
            }
        }
    }
    NSLog(@"[ADB] Could not detect device resolution");
    return CGSizeMake(0, 0);
}

/// Returns YES if `adb devices` shows at least one device/emulator attached.
static BOOL adb_device_connected() {
    NSString* output = adb_run_output(@[@"devices"]);
    // Output format: "List of devices attached\n<serial>\tdevice\n"
    NSArray<NSString*>* lines = [output componentsSeparatedByString:@"\n"];
    for (NSString* line in lines) {
        if ([line hasSuffix:@"\tdevice"]) {
            return YES;
        }
    }
    return NO;
}

/// Returns YES if the DroidScreen Android app is currently running on the device.
static BOOL adb_droidscreen_running() {
    NSString* output = adb_run_output(@[@"shell", @"pidof", @"com.droidscreen.app"]);
    // pidof returns the PID (a number) if the process is running, empty otherwise.
    for (NSUInteger i = 0; i < output.length; i++) {
        unichar c = [output characterAtIndex:i];
        if (c >= '0' && c <= '9') return YES;
    }
    return NO;
}

// =============================================================================
#pragma mark - Menu Bar Icon
// =============================================================================

/// Creates a small monitor+phone icon drawn programmatically.
static NSImage* CreateStatusBarIcon() {
    NSImage* image = [NSImage imageWithSize:NSMakeSize(18, 18)
                                   flipped:NO
                            drawingHandler:^BOOL(NSRect rect) {
        [[NSColor blackColor] setStroke];

        // Monitor body (left portion).
        NSBezierPath* monitor = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(1, 5, 10, 8)
                                                                xRadius:1.0
                                                                yRadius:1.0];
        monitor.lineWidth = 1.2;
        [monitor stroke];

        // Monitor stand.
        NSBezierPath* stand = [NSBezierPath bezierPath];
        [stand moveToPoint:NSMakePoint(4, 5)];
        [stand lineToPoint:NSMakePoint(3, 3)];
        [stand moveToPoint:NSMakePoint(8, 5)];
        [stand lineToPoint:NSMakePoint(9, 3)];
        [stand moveToPoint:NSMakePoint(2, 3)];
        [stand lineToPoint:NSMakePoint(10, 3)];
        stand.lineWidth = 1.0;
        [stand stroke];

        // Phone (right portion, slightly overlapping).
        NSBezierPath* phone = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(12, 3, 5, 10)
                                                               xRadius:0.8
                                                               yRadius:0.8];
        phone.lineWidth = 1.2;
        [phone stroke];

        // Phone screen line.
        NSBezierPath* screenLine = [NSBezierPath bezierPath];
        [screenLine moveToPoint:NSMakePoint(12.5, 5)];
        [screenLine lineToPoint:NSMakePoint(16.5, 5)];
        [screenLine moveToPoint:NSMakePoint(12.5, 11)];
        [screenLine lineToPoint:NSMakePoint(16.5, 11)];
        screenLine.lineWidth = 0.8;
        [screenLine stroke];

        return YES;
    }];

    [image setTemplate:YES];
    return image;
}

// =============================================================================
#pragma mark - AppDelegate
// =============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>

// Menu bar.
@property (nonatomic, strong) NSStatusItem* statusItem;
@property (nonatomic, strong) NSMenu*       statusMenu;
@property (nonatomic, strong) NSMenuItem*   statusMenuItem;
@property (nonatomic, strong) NSMenuItem*   connectMenuItem;

// Settings window.
@property (nonatomic, strong) NSWindow* settingsWindow;
@property (nonatomic, strong) NSPopUpButton* fpsPopup;
@property (nonatomic, strong) NSPopUpButton* bitratePopup;
@property (nonatomic, strong) NSPopUpButton* resolutionPopup;
@property (nonatomic, strong) NSPopUpButton* scalePopup;       // unused, kept for compat
@property (nonatomic, strong) NSButton*      retinaCheckbox;
@property (nonatomic, strong) NSTextField*   portField;
@property (nonatomic, strong) NSButton*      autoDetectButton;
@property (nonatomic, strong) NSProgressIndicator* speedTestSpinner;

// Deck Shortcuts section in settings.
@property (nonatomic, strong) NSScrollView*  deckAppListScrollView;
@property (nonatomic, strong) NSView*        deckAppListContainer;
@property (nonatomic, strong) NSMutableArray<NSDictionary*>* deckApps;

// State.
@property (nonatomic, assign) BOOL isStreaming;
@property (nonatomic, assign) BOOL isBusy;  // Prevents concurrent connect/disconnect ops.
@property (nonatomic, assign) BOOL userDisconnected;  // Suppresses auto-connect until device is re-plugged or user clicks Connect.

- (void)pushDeckState;
- (void)pushDeckVolumeState;
- (void)pushDeckMediaState;
- (void)scheduleDeckMediaRefreshBurst;

@end

@implementation AppDelegate {
    // Pipeline components (C++ objects, owned here).
    std::unique_ptr<droidscreen::VirtualDisplay>    _virtualDisplay;
    std::unique_ptr<droidscreen::SCKCapturer>       _capturer;
    std::unique_ptr<droidscreen::VTEncoder>         _encoder;
    std::unique_ptr<droidscreen::TCPClient>         _client;
    std::unique_ptr<droidscreen::MacMouseInjector> _mouse;
    std::unique_ptr<droidscreen::MacTouchInjector> _touch;
    std::unique_ptr<droidscreen::DeckManager>       _deckManager;
    std::unique_ptr<droidscreen::Pipeline>          _pipeline;

    // Background queue for streaming operations.
    dispatch_queue_t _streamQueue;

    // ADB device detection timer.
    NSTimer* _deviceTimer;

    // Stats timer.
    NSTimer* _statsTimer;

    // GCD timer for pushing volume/media state to Android.
    dispatch_source_t _deckTimer;
    dispatch_queue_t _deckStateQueue;

    // Current streaming parameters (for status display).
    uint32_t _streamWidth;
    uint32_t _streamHeight;
    uint32_t _streamFPS;

    NSString* _lastMediaArtworkTrackKey;
    BOOL _forceArtworkPush;
    NSDictionary* _lastNowPlayingSnapshot;
    NSDate* _lastNowPlayingSnapshotAt;
}

// -----------------------------------------------------------------------------
#pragma mark - App Lifecycle
// -----------------------------------------------------------------------------

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // Register default settings.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kSettingFPS:         @60,
        kSettingBitrate:     @15000,
        kSettingResolution:  @0,
        kSettingScale:       @0,
        kSettingPort:        @38271,
        kSettingDeckApps:    @[
            @{@"id": @"app_safari",   @"label": @"Safari",   @"bundle_id": @"com.apple.Safari",   @"path": @"/Applications/Safari.app"},
            @{@"id": @"app_music",    @"label": @"Music",    @"bundle_id": @"com.apple.Music",    @"path": @"/Applications/Music.app"},
            @{@"id": @"app_notes",    @"label": @"Notes",    @"bundle_id": @"com.apple.Notes",    @"path": @"/Applications/Notes.app"},
            @{@"id": @"app_terminal", @"label": @"Terminal", @"bundle_id": @"com.apple.Terminal", @"path": @"/Applications/Utilities/Terminal.app"},
        ],
    }];

    _streamQueue = dispatch_queue_create("com.droidscreen.stream", DISPATCH_QUEUE_SERIAL);
    _deckStateQueue = dispatch_queue_create("com.droidscreen.deck-state", DISPATCH_QUEUE_SERIAL);
    _isStreaming = NO;

    [self buildStatusBar];
    [self buildSettingsWindow];
    [self startDeviceDetectionTimer];

    // Request screen capture permission once at launch.
    if (!CGPreflightScreenCaptureAccess()) {
        NSLog(@"[DroidScreen] Screen recording permission not granted, requesting...");
        CGRequestScreenCaptureAccess();
    }

    NSLog(@"[DroidScreen] Menu bar app launched");
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    [self stopDeviceDetectionTimer];
    [self stopStatsTimer];

    if (_isStreaming) {
        [self disconnectSync];
    }

    NSLog(@"[DroidScreen] App terminated");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return NO; // Keep running when settings window is closed.
}

// -----------------------------------------------------------------------------
#pragma mark - Status Bar
// -----------------------------------------------------------------------------

- (void)buildStatusBar {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    self.statusItem.button.image = CreateStatusBarIcon();
    self.statusItem.button.toolTip = @"DroidScreen";

    self.statusMenu = [[NSMenu alloc] init];

    // Title item.
    NSMenuItem* titleItem = [[NSMenuItem alloc] initWithTitle:@"DroidScreen"
                                                       action:nil
                                                keyEquivalent:@""];
    [titleItem setEnabled:NO];
    NSDictionary* boldAttrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:13]
    };
    titleItem.attributedTitle = [[NSAttributedString alloc] initWithString:@"DroidScreen"
                                                               attributes:boldAttrs];
    [self.statusMenu addItem:titleItem];

    // Status item.
    self.statusMenuItem = [[NSMenuItem alloc] initWithTitle:@"Status: Disconnected"
                                                     action:nil
                                              keyEquivalent:@""];
    [self.statusMenuItem setEnabled:NO];
    [self.statusMenu addItem:self.statusMenuItem];

    [self.statusMenu addItem:[NSMenuItem separatorItem]];

    // Settings.
    NSMenuItem* settingsItem = [[NSMenuItem alloc] initWithTitle:@"Settings..."
                                                          action:@selector(showSettings:)
                                                   keyEquivalent:@","];
    settingsItem.target = self;
    [self.statusMenu addItem:settingsItem];

    // Stream Deck.
    NSMenuItem* deckItem = [[NSMenuItem alloc] initWithTitle:@"Configure Deck..."
                                                       action:@selector(showDeckConfig:)
                                                keyEquivalent:@""];
    deckItem.target = self;
    [self.statusMenu addItem:deckItem];

    [self.statusMenu addItem:[NSMenuItem separatorItem]];

    // Connect / Disconnect toggle.
    self.connectMenuItem = [[NSMenuItem alloc] initWithTitle:@"Connect"
                                                      action:@selector(toggleConnect:)
                                               keyEquivalent:@""];
    self.connectMenuItem.target = self;
    [self.statusMenu addItem:self.connectMenuItem];

    [self.statusMenu addItem:[NSMenuItem separatorItem]];

    // Quit.
    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(quitApp:)
                                               keyEquivalent:@"q"];
    quitItem.target = self;
    [self.statusMenu addItem:quitItem];

    self.statusItem.menu = self.statusMenu;
}

- (void)updateStatusText:(NSString*)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        self.statusMenuItem.title = text;
    });
}

- (void)updateConnectMenuTitle:(NSString*)title {
    dispatch_async(dispatch_get_main_queue(), ^{
        self.connectMenuItem.title = title;
    });
}

// -----------------------------------------------------------------------------
#pragma mark - Settings Window
// -----------------------------------------------------------------------------

- (void)buildSettingsWindow {
    // Load deck apps from defaults.
    NSArray* savedDeckApps = [[NSUserDefaults standardUserDefaults] arrayForKey:kSettingDeckApps];
    self.deckApps = savedDeckApps ? [savedDeckApps mutableCopy] : [NSMutableArray new];

    NSRect frame = NSMakeRect(0, 0, 500, 580);
    NSWindowStyleMask style = NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable;

    self.settingsWindow = [[NSWindow alloc] initWithContentRect:frame
                                                      styleMask:style
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
    self.settingsWindow.title = @"DroidScreen Settings";
    self.settingsWindow.delegate = self;

    // Force dark appearance — all standard AppKit controls automatically render dark.
    self.settingsWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

    // Keep settings window on main display — never let it migrate to virtual display.
    self.settingsWindow.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace
                                          | NSWindowCollectionBehaviorTransient;
    self.settingsWindow.level = NSFloatingWindowLevel;
    [self.settingsWindow setReleasedWhenClosed:NO];
    [self.settingsWindow center];

    // Prevent resizing.
    self.settingsWindow.minSize = frame.size;
    self.settingsWindow.maxSize = frame.size;

    // Dark background color for the window content.
    self.settingsWindow.backgroundColor =
        [NSColor colorWithCalibratedRed:0.08 green:0.08 blue:0.12 alpha:1.0];

    NSView* contentView = self.settingsWindow.contentView;
    CGFloat leftMargin  = 24;
    CGFloat labelWidth  = 110;
    CGFloat controlLeft = leftMargin + labelWidth + 10;
    CGFloat controlWidth = 310;
    CGFloat rowHeight   = 32;
    CGFloat startY      = frame.size.height - 50;

    // --- Section: Streaming Settings ---
    CGFloat y = startY;
    NSTextField* streamingHeader = [NSTextField labelWithString:@"Streaming"];
    streamingHeader.frame = NSMakeRect(leftMargin, y + 2, 200, 20);
    streamingHeader.font = [NSFont boldSystemFontOfSize:15];
    streamingHeader.textColor = [NSColor whiteColor];
    [contentView addSubview:streamingHeader];

    // Separator line.
    y -= 14;
    NSBox* sep1 = [[NSBox alloc] initWithFrame:NSMakeRect(leftMargin, y, frame.size.width - 2 * leftMargin, 1)];
    sep1.boxType = NSBoxSeparator;
    [contentView addSubview:sep1];

    // --- Row 1: FPS ---
    y -= rowHeight;
    [self addLabel:@"Frame Rate:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.fpsPopup = [self addPopUpButton:contentView atX:controlLeft y:y width:controlWidth];
    [self.fpsPopup addItemWithTitle:@"30 fps"];
    [self.fpsPopup addItemWithTitle:@"60 fps"];
    [self.fpsPopup addItemWithTitle:@"120 fps"];
    NSInteger savedFPS = [[NSUserDefaults standardUserDefaults] integerForKey:kSettingFPS];
    NSInteger fpsIdx = 1; // default: 60
    if (savedFPS == 30) fpsIdx = 0;
    else if (savedFPS == 120) fpsIdx = 2;
    [self.fpsPopup selectItemAtIndex:fpsIdx];

    // --- Row 2: Bitrate ---
    y -= rowHeight + 4;
    [self addLabel:@"Bitrate:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.bitratePopup = [self addPopUpButton:contentView atX:controlLeft y:y width:230];
    for (int i = 0; i < kBitrateCount; i++) {
        [self.bitratePopup addItemWithTitle:@(kBitrateLabels[i])];
    }
    NSInteger savedBitrate = [[NSUserDefaults standardUserDefaults] integerForKey:kSettingBitrate];
    int bitrateIdx = 2; // default: 15 Mbps
    for (int i = 0; i < kBitrateCount; i++) {
        if (kBitrates[i] == (int)savedBitrate) { bitrateIdx = i; break; }
    }
    [self.bitratePopup selectItemAtIndex:bitrateIdx];

    // Auto-detect bitrate button.
    self.autoDetectButton = [[NSButton alloc] initWithFrame:
        NSMakeRect(controlLeft + 240, y, 65, 26)];
    self.autoDetectButton.title = @"Auto";
    self.autoDetectButton.bezelStyle = NSBezelStyleRounded;
    [self.autoDetectButton setFont:[NSFont systemFontOfSize:11]];
    self.autoDetectButton.target = self;
    self.autoDetectButton.action = @selector(runSpeedTest:);
    [contentView addSubview:self.autoDetectButton];

    // Spinner shown during speed test (hidden by default).
    self.speedTestSpinner = [[NSProgressIndicator alloc] initWithFrame:
        NSMakeRect(controlLeft + 240, y + 3, 60, 20)];
    self.speedTestSpinner.style = NSProgressIndicatorStyleBar;
    self.speedTestSpinner.indeterminate = YES;
    self.speedTestSpinner.hidden = YES;
    [contentView addSubview:self.speedTestSpinner];

    // --- Row 3: Resolution ---
    y -= rowHeight + 4;
    [self addLabel:@"Resolution:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.resolutionPopup = [self addPopUpButton:contentView atX:controlLeft y:y width:controlWidth];
    for (int i = 0; i < kResolutionCount; i++) {
        [self.resolutionPopup addItemWithTitle:@(kResolutions[i].label)];
    }
    NSInteger savedRes = [[NSUserDefaults standardUserDefaults] integerForKey:kSettingResolution];
    if (savedRes >= 0 && savedRes < kResolutionCount) {
        [self.resolutionPopup selectItemAtIndex:savedRes];
    }

    // --- Row 4: Retina (HiDPI) ---
    y -= rowHeight + 4;
    self.scalePopup = nil; // Not used anymore — replaced by retinaCheckbox.
    self.retinaCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(controlLeft, y, controlWidth, 20)];
    [self.retinaCheckbox setButtonType:NSButtonTypeSwitch];
    self.retinaCheckbox.title = @"Retina (HiDPI) — sharper text and UI";
    [self.retinaCheckbox setFont:[NSFont systemFontOfSize:13]];
    self.retinaCheckbox.state = [[NSUserDefaults standardUserDefaults] boolForKey:kSettingScale]
                                 ? NSControlStateValueOn : NSControlStateValueOff;
    [contentView addSubview:self.retinaCheckbox];
    [self addLabel:@"Quality:" toView:contentView atX:leftMargin y:y width:labelWidth];

    // --- Row 5: Port ---
    y -= rowHeight + 8;
    [self addLabel:@"Port:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.portField = [[NSTextField alloc] initWithFrame:NSMakeRect(controlLeft, y, 100, 24)];
    self.portField.stringValue = [NSString stringWithFormat:@"%ld",
        (long)[[NSUserDefaults standardUserDefaults] integerForKey:kSettingPort]];
    [self.portField setFont:[NSFont systemFontOfSize:13]];
    [self.portField setBezelStyle:NSTextFieldRoundedBezel];
    [contentView addSubview:self.portField];

    // --- Apply button ---
    y -= rowHeight + 12;
    NSButton* applyButton = [[NSButton alloc] initWithFrame:NSMakeRect(frame.size.width - 100 - 24, y, 100, 32)];
    applyButton.title = @"Apply";
    applyButton.bezelStyle = NSBezelStyleRounded;
    [applyButton setFont:[NSFont systemFontOfSize:13]];
    applyButton.target = self;
    applyButton.action = @selector(applySettings:);
    applyButton.keyEquivalent = @"\r"; // Enter key.
    [contentView addSubview:applyButton];

    // =========================================================================
    // Section: Stream Deck Shortcuts
    // =========================================================================
    y -= 30;
    NSTextField* deckHeader = [NSTextField labelWithString:@"Stream Deck Shortcuts"];
    deckHeader.frame = NSMakeRect(leftMargin, y, 300, 20);
    deckHeader.font = [NSFont boldSystemFontOfSize:15];
    deckHeader.textColor = [NSColor whiteColor];
    [contentView addSubview:deckHeader];

    y -= 14;
    NSBox* sep2 = [[NSBox alloc] initWithFrame:NSMakeRect(leftMargin, y, frame.size.width - 2 * leftMargin, 1)];
    sep2.boxType = NSBoxSeparator;
    [contentView addSubview:sep2];

    // Scrollable list of configured deck apps.
    y -= 6;
    CGFloat listHeight = y - 50; // Leave room for "Add App..." button at bottom.
    if (listHeight < 60) listHeight = 60;

    self.deckAppListScrollView = [[NSScrollView alloc] initWithFrame:
        NSMakeRect(leftMargin, y - listHeight, frame.size.width - 2 * leftMargin, listHeight)];
    self.deckAppListScrollView.hasVerticalScroller = YES;
    self.deckAppListScrollView.autohidesScrollers = YES;
    self.deckAppListScrollView.borderType = NSBezelBorder;
    self.deckAppListScrollView.drawsBackground = YES;
    self.deckAppListScrollView.backgroundColor =
        [NSColor colorWithCalibratedRed:0.12 green:0.12 blue:0.16 alpha:1.0];
    [contentView addSubview:self.deckAppListScrollView];

    [self rebuildDeckAppList];

    // "Add App..." button.
    y = self.deckAppListScrollView.frame.origin.y - 10;
    NSButton* addAppButton = [[NSButton alloc] initWithFrame:
        NSMakeRect(leftMargin, y - 28, 120, 28)];
    addAppButton.title = @"Add App...";
    addAppButton.bezelStyle = NSBezelStyleRounded;
    [addAppButton setFont:[NSFont systemFontOfSize:12]];
    addAppButton.target = self;
    addAppButton.action = @selector(addDeckApp:);
    [contentView addSubview:addAppButton];
}

- (void)addLabel:(NSString*)text toView:(NSView*)view atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)w {
    NSTextField* label = [NSTextField labelWithString:text];
    label.frame = NSMakeRect(x, y, w, 20);
    label.alignment = NSTextAlignmentRight;
    [label setFont:[NSFont systemFontOfSize:13]];
    [view addSubview:label];
}

- (NSPopUpButton*)addPopUpButton:(NSView*)view atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)w {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(x, y - 2, w, 26)
                                                      pullsDown:NO];
    [popup setFont:[NSFont systemFontOfSize:13]];
    [view addSubview:popup];
    return popup;
}

// ---------------------------------------------------------------------------
// Deck Shortcuts: rebuild the scrollable list of configured apps.
// ---------------------------------------------------------------------------
- (void)rebuildDeckAppList {
    CGFloat rowH = 28;
    CGFloat listW = self.deckAppListScrollView.contentSize.width;
    CGFloat totalH = MAX(rowH * (CGFloat)self.deckApps.count, self.deckAppListScrollView.contentSize.height);

    self.deckAppListContainer = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, listW, totalH)];
    self.deckAppListContainer.wantsLayer = YES;

    for (NSInteger i = 0; i < (NSInteger)self.deckApps.count; i++) {
        NSDictionary* app = self.deckApps[i];
        CGFloat ry = totalH - rowH * (i + 1);

        // App icon (16x16).
        NSString* appPath = app[@"path"];
        NSImage* icon = nil;
        if (appPath && [[NSFileManager defaultManager] fileExistsAtPath:appPath]) {
            icon = [[NSWorkspace sharedWorkspace] iconForFile:appPath];
        }
        if (!icon) {
            icon = [NSImage imageNamed:NSImageNameApplicationIcon];
        }
        NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(8, ry + 6, 16, 16)];
        iconView.image = icon;
        iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
        [self.deckAppListContainer addSubview:iconView];

        // App name label.
        NSTextField* nameLabel = [NSTextField labelWithString:app[@"label"] ?: @"Unknown"];
        nameLabel.frame = NSMakeRect(32, ry + 4, listW - 80, 20);
        nameLabel.font = [NSFont systemFontOfSize:12];
        nameLabel.textColor = [NSColor secondaryLabelColor];
        [self.deckAppListContainer addSubview:nameLabel];

        // Remove (x) button.
        NSButton* removeBtn = [[NSButton alloc] initWithFrame:NSMakeRect(listW - 36, ry + 2, 24, 24)];
        removeBtn.bezelStyle = NSBezelStyleInline;
        removeBtn.title = @"";
        removeBtn.image = [NSImage imageWithSystemSymbolName:@"xmark.circle.fill"
                                    accessibilityDescription:@"Remove"];
        removeBtn.imagePosition = NSImageOnly;
        removeBtn.bordered = NO;
        removeBtn.tag = i;
        removeBtn.target = self;
        removeBtn.action = @selector(removeDeckApp:);
        [self.deckAppListContainer addSubview:removeBtn];
    }

    self.deckAppListScrollView.documentView = self.deckAppListContainer;
}

// ---------------------------------------------------------------------------
// Deck Shortcuts: "Add App..." opens an NSOpenPanel in /Applications.
// ---------------------------------------------------------------------------
- (void)addDeckApp:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Select Applications for Stream Deck";
    panel.allowsMultipleSelection = YES;
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.directoryURL = [NSURL fileURLWithPath:@"/Applications"];
    panel.allowedContentTypes = @[[UTType typeWithIdentifier:@"com.apple.application-bundle"]];
    panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

    [panel beginSheetModalForWindow:self.settingsWindow completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) return;

        for (NSURL* url in panel.URLs) {
            NSString* appPath = url.path;
            NSBundle* appBundle = [NSBundle bundleWithPath:appPath];
            NSString* bundleId = appBundle.bundleIdentifier ?: @"";
            NSString* appName = [[appPath lastPathComponent] stringByDeletingPathExtension];
            NSString* tileId = [NSString stringWithFormat:@"app_%@",
                [appName lowercaseString]];

            // Avoid duplicates.
            BOOL exists = NO;
            for (NSDictionary* existing in self.deckApps) {
                if ([existing[@"path"] isEqualToString:appPath]) {
                    exists = YES;
                    break;
                }
            }
            if (exists) continue;

            NSDictionary* entry = @{
                @"id":        tileId,
                @"label":     appName,
                @"bundle_id": bundleId,
                @"path":      appPath,
            };
            [self.deckApps addObject:entry];
        }

        // Persist and update UI.
        [self saveDeckAppsAndRebuild];
    }];
}

// ---------------------------------------------------------------------------
// Deck Shortcuts: remove an app by index (button tag).
// ---------------------------------------------------------------------------
- (void)removeDeckApp:(id)sender {
    NSInteger idx = [(NSButton*)sender tag];
    if (idx >= 0 && idx < (NSInteger)self.deckApps.count) {
        [self.deckApps removeObjectAtIndex:idx];
        [self saveDeckAppsAndRebuild];
    }
}

// ---------------------------------------------------------------------------
// Deck Shortcuts: persist to NSUserDefaults, rebuild UI, update deck layout.
// ---------------------------------------------------------------------------
- (void)saveDeckAppsAndRebuild {
    [[NSUserDefaults standardUserDefaults] setObject:[self.deckApps copy]
                                              forKey:kSettingDeckApps];
    [[NSUserDefaults standardUserDefaults] synchronize];

    [self rebuildDeckAppList];
    [self rebuildDeckLayout];
}

// ---------------------------------------------------------------------------
// Extract a 64x64 PNG icon from an .app path, returned as base64 string.
// ---------------------------------------------------------------------------
- (NSString*)extractIconBase64ForAppAtPath:(NSString*)appPath {
    if (!appPath || ![[NSFileManager defaultManager] fileExistsAtPath:appPath])
        return @"";

    NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:appPath];
    if (!icon) return @"";

    // Resize to 64x64.
    NSSize targetSize = NSMakeSize(64, 64);
    NSImage* resized = [[NSImage alloc] initWithSize:targetSize];
    [resized lockFocus];
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];
    [icon drawInRect:NSMakeRect(0, 0, 64, 64)
            fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver
            fraction:1.0];
    [resized unlockFocus];

    // Get PNG data.
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithData:[resized TIFFRepresentation]];
    NSData* pngData = [rep representationUsingType:NSBitmapImageFileTypePNG
                                        properties:@{}];
    if (!pngData) return @"";

    return [pngData base64EncodedStringWithOptions:0];
}

// ---------------------------------------------------------------------------
// Rebuild the DeckManager layout from current deckApps and push to Android.
// ---------------------------------------------------------------------------
- (void)rebuildDeckLayout {
    if (!_deckManager) return;

    // Load deck apps from defaults if not yet loaded.
    if (!self.deckApps) {
        NSArray* saved = [[NSUserDefaults standardUserDefaults] arrayForKey:kSettingDeckApps];
        self.deckApps = saved ? [saved mutableCopy] : [NSMutableArray new];
    }

    droidscreen::DeckLayout layout;
    layout.grid_cols = 4;

    // Row 0: Media + Volume (always present).
    droidscreen::DeckTile media;
    media.id       = "media";
    media.type     = "media";
    media.label    = "Now Playing";
    media.row      = 0;
    media.col      = 0;
    media.col_span = 2;
    layout.tiles.push_back(std::move(media));

    droidscreen::DeckTile volume;
    volume.id       = "volume";
    volume.type     = "volume";
    volume.label    = "Volume";
    volume.row      = 0;
    volume.col      = 2;
    volume.col_span = 1;
    layout.tiles.push_back(std::move(volume));

    // Row 1+: App shortcuts from deckApps.
    for (NSInteger i = 0; i < (NSInteger)self.deckApps.count; i++) {
        NSDictionary* app = self.deckApps[i];
        droidscreen::DeckTile tile;
        tile.id        = [app[@"id"] UTF8String] ?: "";
        tile.type      = "app";
        tile.label     = [app[@"label"] UTF8String] ?: "";
        tile.app_path  = [app[@"path"] UTF8String] ?: "";
        tile.bundle_id = [app[@"bundle_id"] UTF8String] ?: "";
        tile.row       = 1 + (int)(i / layout.grid_cols);
        tile.col       = (int)(i % layout.grid_cols);
        tile.col_span  = 1;

        // Extract app icon as base64 PNG for Android.
        NSString* iconB64 = [self extractIconBase64ForAppAtPath:app[@"path"]];
        tile.icon_b64 = [iconB64 UTF8String] ?: "";

        layout.tiles.push_back(std::move(tile));
    }

    // Calculate grid rows needed.
    int appRows = (int)((self.deckApps.count + layout.grid_cols - 1) / layout.grid_cols);
    layout.grid_rows = 1 + (appRows > 1 ? appRows : 1);

    _deckManager->set_layout(layout);

    // If streaming, push the updated config to Android.
    if (_isStreaming && _pipeline && _pipeline->is_running()) {
        _pipeline->send_deck_config();
        NSLog(@"[deck] Pushed updated deck config to Android (%lu app tiles)",
              (unsigned long)self.deckApps.count);
    }
}

// NSWindowDelegate: just hide the settings window rather than destroying it.
- (void)windowWillClose:(NSNotification*)notification {
    // Nothing special -- window is retained, will be shown again via makeKeyAndOrderFront.
}

// -----------------------------------------------------------------------------
#pragma mark - Settings Load / Save
// -----------------------------------------------------------------------------

- (void)applySettings:(id)sender {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

    // FPS.
    NSInteger fpsIdx = self.fpsPopup.indexOfSelectedItem;
    NSInteger fps;
    switch (fpsIdx) {
        case 0: fps = 30; break;
        case 2: fps = 120; break;
        default: fps = 60; break;
    }
    [defaults setInteger:fps forKey:kSettingFPS];

    // Bitrate.
    NSInteger brIdx = self.bitratePopup.indexOfSelectedItem;
    if (brIdx >= 0 && brIdx < kBitrateCount) {
        [defaults setInteger:kBitrates[brIdx] forKey:kSettingBitrate];
    }

    // Resolution.
    [defaults setInteger:self.resolutionPopup.indexOfSelectedItem forKey:kSettingResolution];

    // Retina (HiDPI).
    [defaults setBool:(self.retinaCheckbox.state == NSControlStateValueOn) forKey:kSettingScale];

    // Port.
    NSInteger port = self.portField.integerValue;
    if (port < 1 || port > 65535) port = 38271;
    [defaults setInteger:port forKey:kSettingPort];

    [defaults synchronize];

    NSLog(@"[Settings] Saved: fps=%ld bitrate=%ld res=%ld retina=%d port=%ld",
          (long)fps,
          (long)[defaults integerForKey:kSettingBitrate],
          (long)[defaults integerForKey:kSettingResolution],
          (int)[defaults boolForKey:kSettingScale],
          (long)port);

    // If currently streaming, restart the pipeline with new settings.
    if (_isStreaming) {
        NSLog(@"[Settings] Restarting pipeline with new settings...");
        dispatch_async(_streamQueue, ^{
            [self disconnectSync];
            // Wait for the Android side to finish cleaning up the old
            // connection and re-accept. Without this delay, adb forward
            // fails because the old socket is still in TIME_WAIT.
            usleep(500000);  // 500ms
            [self connectSync];
        });
    }

    [self.settingsWindow close];
}

/// Perform a handshake with the given TCPClient (used by speed test).
static bool speed_test_handshake(droidscreen::TCPClient* client) {
    ds_handshake_req_t req{};
    req.protocol_version = DS_PROTOCOL_VERSION;
    req.width            = 1920;
    req.height           = 1200;
    req.fps              = 60;
    req.codec            = DS_CODEC_H264;
    req.max_bitrate_kbps = 15000;
    req.touch_enabled    = 0;
    req.frame_interval_us = 16667; /* 60 fps */

    uint8_t req_buf[DS_HANDSHAKE_REQ_SIZE];
    ds_handshake_req_serialize(req_buf, &req);
    if (!client->send_message(DS_MSG_HANDSHAKE_REQ, 0,
                              req_buf, DS_HANDSHAKE_REQ_SIZE)) {
        return false;
    }

    ds_header_t hdr;
    if (!client->recv_header(&hdr)) return false;
    if (hdr.type != DS_MSG_HANDSHAKE_RESP || hdr.length != DS_HANDSHAKE_RESP_SIZE)
        return false;

    uint8_t resp_buf[DS_HANDSHAKE_RESP_SIZE];
    return client->recv_exact(resp_buf, DS_HANDSHAKE_RESP_SIZE);
}

- (void)runSpeedTest:(id)sender {
    // Show spinner, hide button.
    self.autoDetectButton.hidden = YES;
    self.speedTestSpinner.hidden = NO;
    [self.speedTestSpinner startAnimation:nil];

    // If currently streaming, disconnect first (the speed test needs
    // its own TCP connection to the Android app).
    BOOL wasStreaming = _isStreaming;

    dispatch_async(_streamQueue, ^{
        @autoreleasepool {
            if (wasStreaming) {
                NSLog(@"[SpeedTest] Pausing stream for speed test...");
                [self disconnectSync];
                usleep(500000);  // 500ms for Android to re-open server socket
            }

            uint16_t port = (uint16_t)[[NSUserDefaults standardUserDefaults]
                                        integerForKey:kSettingPort];

            // Set up ADB forward.
            if (!adb_forward_setup(port)) {
                NSLog(@"[SpeedTest] ADB forward failed");
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            // Connect TCP.
            auto client = std::make_unique<droidscreen::TCPClient>();
            if (!client->connect(port)) {
                NSLog(@"[SpeedTest] TCP connect failed");
                adb_forward_remove(port);
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            // Handshake (so Android enters message loop).
            if (!speed_test_handshake(client.get())) {
                NSLog(@"[SpeedTest] Handshake failed");
                client->close();
                adb_forward_remove(port);
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            // Run speed test (2 seconds).
            uint32_t throughput_kbps = droidscreen::run_speed_test(client.get(), 2000);

            // Disconnect and clean up.
            client->close();
            adb_forward_remove(port);

            [self speedTestFinished:throughput_kbps wasStreaming:wasStreaming];
        }
    });
}

- (void)speedTestFinished:(uint32_t)throughput_kbps wasStreaming:(BOOL)wasStreaming {
    // Select best bitrate at ~70% of measured throughput.
    uint32_t target_kbps = throughput_kbps * 70 / 100;
    int bestIdx = 0;
    if (throughput_kbps > 0) {
        for (int i = kBitrateCount - 1; i >= 0; i--) {
            if (kBitrates[i] <= (int)target_kbps) {
                bestIdx = i;
                break;
            }
        }
    }

    NSLog(@"[SpeedTest] throughput=%u kbps, target=%u kbps, selected=%d kbps",
          throughput_kbps, target_kbps, kBitrates[bestIdx]);

    dispatch_async(dispatch_get_main_queue(), ^{
        // Stop spinner, show button.
        [self.speedTestSpinner stopAnimation:nil];
        self.speedTestSpinner.hidden = YES;
        self.autoDetectButton.hidden = NO;

        if (throughput_kbps == 0) {
            self.autoDetectButton.title = @"Failed";
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                self.autoDetectButton.title = @"Auto";
            });
            return;
        }

        // Show result briefly.
        NSString* resultStr = [NSString stringWithFormat:@"%.0f Mbps",
                               throughput_kbps / 1000.0];
        self.autoDetectButton.title = resultStr;

        // Update the bitrate popup and save to defaults.
        [self.bitratePopup selectItemAtIndex:bestIdx];
        [[NSUserDefaults standardUserDefaults] setInteger:kBitrates[bestIdx]
                                                   forKey:kSettingBitrate];

        NSLog(@"[SpeedTest] Set bitrate to %d kbps (USB throughput: %u kbps)",
              kBitrates[bestIdx], throughput_kbps);

        // Reset button title after 3 seconds.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            self.autoDetectButton.title = @"Auto";
        });
    });

    // If we were streaming before the test, reconnect.
    if (wasStreaming) {
        NSLog(@"[SpeedTest] Resuming stream...");
        dispatch_async(_streamQueue, ^{
            usleep(300000);  // 300ms
            [self connectSync];
        });
    }
}

/// Read current settings into a struct for the streaming code.
struct StreamSettings {
    uint16_t port;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t width;      // Logical (point) resolution
    uint32_t height;
    bool     hidpi;      // Retina (2x pixel density)
};

- (StreamSettings)currentSettings {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

    StreamSettings s;
    s.port = (uint16_t)[defaults integerForKey:kSettingPort];
    s.fps  = (uint32_t)[defaults integerForKey:kSettingFPS];
    s.bitrate_kbps = (uint32_t)[defaults integerForKey:kSettingBitrate];

    NSInteger resIdx = [defaults integerForKey:kSettingResolution];
    if (resIdx < 0 || resIdx >= kResolutionCount) resIdx = 0;
    s.width  = kResolutions[resIdx].width;
    s.height = kResolutions[resIdx].height;

    // HiDPI (Retina): the virtual display uses 2× framebuffer pixels,
    // but the stream is captured at the logical resolution. macOS renders
    // at 2× density so text and UI look much sharper.
    s.hidpi = [defaults boolForKey:kSettingScale];

    return s;
}

// -----------------------------------------------------------------------------
#pragma mark - Menu Actions
// -----------------------------------------------------------------------------

- (void)showSettings:(id)sender {
    NSLog(@"[DEBUG] showSettings: called");

    // Always rebuild the settings window to avoid dangling pointer issues.
    // When a virtual display is destroyed/recreated, any window that was
    // on it can become invalidated — even if the pointer is non-nil.
    [self buildSettingsWindow];

    // Ensure the settings window appears on the main (built-in) display.
    NSScreen* mainScreen = [NSScreen mainScreen];
    if (mainScreen) {
        NSRect screenFrame = mainScreen.visibleFrame;
        NSRect windowFrame = self.settingsWindow.frame;
        CGFloat x = NSMidX(screenFrame) - windowFrame.size.width / 2;
        CGFloat y = NSMidY(screenFrame) - windowFrame.size.height / 2;
        [self.settingsWindow setFrameOrigin:NSMakePoint(x, y)];
    }

    [self.settingsWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSLog(@"[DEBUG] showSettings: done");
}

- (void)toggleConnect:(id)sender {
    NSLog(@"[DEBUG] toggleConnect: isStreaming=%d", _isStreaming);
    if (_isStreaming) {
        // User manually disconnected — suppress auto-connect until device is
        // re-plugged or user clicks Connect again.
        _userDisconnected = YES;
        [self updateConnectMenuTitle:@"Disconnecting..."];
        self.connectMenuItem.enabled = NO;
        dispatch_async(_streamQueue, ^{
            [self disconnectSync];
            dispatch_async(dispatch_get_main_queue(), ^{
                self.connectMenuItem.enabled = YES;
            });
        });
    } else {
        // User manually connecting — clear the suppression flag.
        _userDisconnected = NO;
        [self updateConnectMenuTitle:@"Connecting..."];
        self.connectMenuItem.enabled = NO;
        dispatch_async(_streamQueue, ^{
            [self connectSync];
            dispatch_async(dispatch_get_main_queue(), ^{
                self.connectMenuItem.enabled = YES;
            });
        });
    }
}

- (void)showDeckConfig:(id)sender {
    // Opens the settings window (which contains the Deck Shortcuts section).
    [self showSettings:sender];
}

- (void)quitApp:(id)sender {
    [NSApp terminate:nil];
}

// -----------------------------------------------------------------------------
#pragma mark - Connect / Disconnect (runs on _streamQueue)
// -----------------------------------------------------------------------------

- (void)connectSync {
    @autoreleasepool {
        // Guard: prevent concurrent connect/disconnect operations.
        if (_isBusy) {
            NSLog(@"[Stream] connectSync: busy, skipping");
            return;
        }
        if (_isStreaming) {
            NSLog(@"[Stream] connectSync: already streaming, skipping");
            return;
        }
        _isBusy = YES;

        // Guard: require screen recording permission before anything else.
        if (!CGPreflightScreenCaptureAccess()) {
            NSLog(@"[Stream] Screen recording permission not granted");
            [self updateStatusText:@"Status: Screen Recording permission required"];
            [self updateConnectMenuTitle:@"Connect"];
            [[NSWorkspace sharedWorkspace] openURL:
                [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
            _isBusy = NO;
            return;
        }

        StreamSettings settings = [self currentSettings];

        NSLog(@"[Stream] Connecting: %ux%u@%ufps, %u kbps, port %u, hidpi=%s",
              settings.width, settings.height, settings.fps,
              settings.bitrate_kbps, settings.port,
              settings.hidpi ? "YES" : "NO");

        [self updateStatusText:@"Status: Setting up ADB..."];

        // 1. ADB forward.
        if (!adb_forward_setup(settings.port)) {
            [self updateStatusText:@"Status: ADB forward failed"];
            [self updateConnectMenuTitle:@"Connect"];
            _isBusy = NO;
            return;
        }

        // 1b. Detect Android device's native resolution (landscape).
        //     Used to cap the capture resolution so we never stream
        //     more pixels than the tablet can display.
        CGSize deviceRes = adb_device_resolution();
        uint32_t device_w = (uint32_t)deviceRes.width;
        uint32_t device_h = (uint32_t)deviceRes.height;
        if (device_w == 0 || device_h == 0) {
            // Fallback: assume largest preset.
            device_w = kResolutions[0].width;
            device_h = kResolutions[0].height;
            NSLog(@"[Stream] Could not detect device res, using fallback %ux%u",
                  device_w, device_h);
        }
        NSLog(@"[Stream] Target device: %ux%u", device_w, device_h);

        // 2. Create virtual display.
        //    Logical resolution = settings.width × settings.height (point resolution).
        //    With HiDPI, the framebuffer is 2× but macOS renders at 2× density.
        [self updateStatusText:@"Status: Creating virtual display..."];
        _virtualDisplay = std::make_unique<droidscreen::VirtualDisplay>();

        if (!_virtualDisplay->create(settings.width, settings.height,
                                     settings.fps, settings.hidpi)) {
            NSLog(@"[Stream] Virtual display creation failed");
            [self updateStatusText:@"Status: Virtual display failed"];
            [self updateConnectMenuTitle:@"Connect"];
            adb_forward_remove(settings.port);
            _virtualDisplay.reset();
            _isBusy = NO;
            return;
        }

        NSLog(@"[Stream] Virtual display created (ID=%u, hidpi=%s)",
              _virtualDisplay->display_id(),
              settings.hidpi ? "YES" : "NO");

        // Give macOS a moment to register the new display.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 3. Create capturer.
        //    Capture at the FRAMEBUFFER resolution, capped to the device's native res.
        //    This ensures we never encode more pixels than the tablet can display.
        //
        //    Examples (for a 2560×1600 tablet):
        //      1280×800 + Retina → fb=2560×1600, cap→2560×1600 → big + sharp
        //      1920×1200 + Retina → fb=3840×2400, cap→2560×1600 → medium + sharp
        //      2560×1600 + Retina → fb=5120×3200, cap→2560×1600 → lots of content, sharp
        //      2560×1600 no Retina → fb=2560×1600, cap→2560×1600 → normal
        //      1280×800 no Retina → fb=1280×800, cap→1280×800 → big, lower quality
        //
        uint32_t fb_w = settings.hidpi ? settings.width * 2 : settings.width;
        uint32_t fb_h = settings.hidpi ? settings.height * 2 : settings.height;
        uint32_t capture_w = (fb_w <= device_w) ? fb_w : device_w;
        uint32_t capture_h = (fb_h <= device_h) ? fb_h : device_h;

        NSLog(@"[Stream] Capture: framebuffer=%ux%u, device=%ux%u, capture=%ux%u",
              fb_w, fb_h, device_w, device_h, capture_w, capture_h);

        [self updateStatusText:@"Status: Initializing capture..."];
        _capturer = std::make_unique<droidscreen::SCKCapturer>();

        if (!_capturer->init_with_display_id(_virtualDisplay->display_id(),
                                             capture_w, capture_h,
                                             settings.fps)) {
            NSLog(@"[Stream] Capturer init failed");
            [self updateStatusText:@"Status: Capture init failed"];
            [self updateConnectMenuTitle:@"Connect"];
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
            _capturer.reset();
            adb_forward_remove(settings.port);
            _isBusy = NO;
            return;
        }

        _streamWidth  = _capturer->width();
        _streamHeight = _capturer->height();
        _streamFPS    = settings.fps;

        NSLog(@"[Stream] Capture resolution: %ux%u", _streamWidth, _streamHeight);

        // 4. Connect TCP.
        [self updateStatusText:@"Status: Connecting to device..."];
        _client = std::make_unique<droidscreen::TCPClient>();

        if (!_client->connect(settings.port)) {
            NSLog(@"[Stream] TCP connect failed");
            [self updateStatusText:@"Status: Connection failed (is Android app running?)"];
            [self updateConnectMenuTitle:@"Connect"];
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
            _capturer.reset();
            _client.reset();
            adb_forward_remove(settings.port);
            _isBusy = NO;
            return;
        }

        // 5. Create encoder, touch/mouse injector, deck manager, pipeline.
        _encoder = std::make_unique<droidscreen::VTEncoder>();
        _mouse   = std::make_unique<droidscreen::MacMouseInjector>();
        _touch   = std::make_unique<droidscreen::MacTouchInjector>();

        // Check Accessibility permission for input injection (CGEventPost).
        NSDictionary *axOpts = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        bool accessibilityGranted =
            AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)axOpts);
        if (!accessibilityGranted) {
            NSLog(@"[Stream] Accessibility permission not granted; input injection disabled");
        }

        // Initialize injectors with the virtual display ID.
        // The injectors query CGDisplayBounds at event time so coordinates
        // are always correct regardless of HiDPI, scaling, or resolution
        // changes made by the user in System Settings.
        if (accessibilityGranted) {
            CGDirectDisplayID did = _virtualDisplay->display_id();
            _mouse->set_display_id(did);
            _touch->set_display_id(did);
            if (!_mouse->init(0, 0)) {
                NSLog(@"[Stream] Mouse injector init failed");
            }
            if (!_touch->init(0, 0)) {
                NSLog(@"[Stream] Touch injector init failed");
            }
        }

        _deckManager = std::make_unique<droidscreen::DeckManager>();

        // Build the deck layout from user-configured apps in NSUserDefaults.
        [self rebuildDeckLayout];

        __weak AppDelegate* weakSelf = self;
        // Dynamic action callback: look up app_path from the tile in the layout.
        droidscreen::DeckManager* deckPtr = _deckManager.get();
        _deckManager->set_action_callback([deckPtr, weakSelf](uint8_t action, const std::string& tile_id) {
            fprintf(stderr, "[deck] action callback: action=%u tile=%s\n",
                    action, tile_id.c_str());

            // Find the tile in the current layout.
            const auto& tiles = deckPtr->layout().tiles;
            for (const auto& tile : tiles) {
                if (tile.id != tile_id) continue;

                if (tile.type == "media") {
                    AppDelegate* strongSelf = weakSelf;
                    switch (action) {
                        case 1:
                            post_media_key_event(NX_KEYTYPE_PLAY);
                            NSLog(@"[deck] Sent media key: play/pause");
                            [strongSelf scheduleDeckMediaRefreshBurst];
                            break;
                        case 2:
                            post_media_key_event(NX_KEYTYPE_PREVIOUS);
                            NSLog(@"[deck] Sent media key: previous");
                            [strongSelf scheduleDeckMediaRefreshBurst];
                            break;
                        case 3:
                            post_media_key_event(NX_KEYTYPE_NEXT);
                            NSLog(@"[deck] Sent media key: next");
                            [strongSelf scheduleDeckMediaRefreshBurst];
                            break;
                        default:
                            NSLog(@"[deck] Ignoring unknown media action %u", action);
                            break;
                    }
                    break;
                }

                if (tile.type == "app") {
                    if (action != 0) {
                        NSLog(@"[deck] Ignoring non-tap app action %u for %s",
                              action, tile_id.c_str());
                        break;
                    }

                    // Launch by app_path if available, otherwise by bundle_id.
                    std::string path = tile.app_path;
                    std::string bundleId = tile.bundle_id;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        NSURL* appURL = nil;
                        if (!path.empty()) {
                            appURL = [NSURL fileURLWithPath:
                                [NSString stringWithUTF8String:path.c_str()]];
                        }
                        if (!appURL && !bundleId.empty()) {
                            appURL = [[NSWorkspace sharedWorkspace]
                                URLForApplicationWithBundleIdentifier:
                                    [NSString stringWithUTF8String:bundleId.c_str()]];
                        }
                        if (appURL) {
                            NSWorkspaceOpenConfiguration* config =
                                [NSWorkspaceOpenConfiguration configuration];
                            [[NSWorkspace sharedWorkspace] openApplicationAtURL:appURL
                                                                 configuration:config
                                                             completionHandler:^(NSRunningApplication* app,
                                                                                 NSError* error) {
                                if (error) {
                                    NSLog(@"[deck] Failed to launch %s: %@",
                                          tile_id.c_str(), error);
                                } else {
                                    NSLog(@"[deck] Launched %s", tile_id.c_str());
                                }
                            }];
                        } else {
                            NSLog(@"[deck] App not found for tile: %s", tile_id.c_str());
                        }
                    });
                    break;
                }
            }
        });
        _deckManager->set_volume_callback([weakSelf](uint16_t level, bool muted) {
            fprintf(stderr, "[deck] volume callback: level=%u muted=%d\n",
                    level, muted ? 1 : 0);
            float vol = level / 65535.0f;
            setSystemVolume(vol);
            setSystemMuted(muted);
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            dispatch_async(strongSelf->_deckStateQueue, ^{
                [strongSelf pushDeckVolumeState];
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC),
                           strongSelf->_deckStateQueue, ^{
                [strongSelf pushDeckVolumeState];
            });
        });

        _pipeline = std::make_unique<droidscreen::Pipeline>(
            _capturer.get(), _encoder.get(), _client.get(), _touch.get(), _mouse.get(),
            _deckManager.get());

        if (!_pipeline->start(_streamWidth, _streamHeight,
                              settings.fps, settings.bitrate_kbps, accessibilityGranted)) {
            NSLog(@"[Stream] Pipeline start failed");
            [self updateStatusText:@"Status: Pipeline start failed"];
            [self updateConnectMenuTitle:@"Connect"];
            _pipeline.reset();
            _deckManager.reset();
            _encoder.reset();
            _mouse.reset();
            _touch.reset();
            _client->close();
            _client.reset();
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
            _capturer.reset();
            adb_forward_remove(settings.port);
            _isBusy = NO;
            return;
        }

        // Success.
        _isStreaming = YES;
        NSString* statusStr = [NSString stringWithFormat:@"Status: Streaming %ux%u@%ufps",
                               _streamWidth, _streamHeight, _streamFPS];
        [self updateStatusText:statusStr];
        [self updateConnectMenuTitle:@"Disconnect"];

        NSLog(@"[Stream] Pipeline started: %ux%u@%ufps",
              _streamWidth, _streamHeight, _streamFPS);

        // Start stats logging and health monitoring.
        dispatch_async(dispatch_get_main_queue(), ^{
            [self startStatsTimer];
        });

        // Start a GCD timer to push volume/media state to Android every 2 seconds.
        _lastMediaArtworkTrackKey = nil;
        _forceArtworkPush = YES;
        _lastNowPlayingSnapshot = nil;
        _lastNowPlayingSnapshotAt = nil;
        [self startDeckTimer];

        _isBusy = NO;
    }
}

- (void)disconnectSync {
    @autoreleasepool {
        if (!_isStreaming) {
            NSLog(@"[Stream] disconnectSync: not streaming, skipping");
            return;
        }
        NSLog(@"[Stream] Disconnecting...");

        dispatch_async(dispatch_get_main_queue(), ^{
            [self stopStatsTimer];
        });

        [self stopDeckTimer];
        _lastMediaArtworkTrackKey = nil;
        _forceArtworkPush = YES;
        _lastNowPlayingSnapshot = nil;
        _lastNowPlayingSnapshotAt = nil;

        uint16_t port = (uint16_t)[[NSUserDefaults standardUserDefaults] integerForKey:kSettingPort];

        if (_pipeline) {
            _pipeline->stop();
            _pipeline.reset();
        }
        if (_encoder) {
            _encoder.reset();
        }
        if (_touch) {
            _touch.reset();
        }
        if (_mouse) {
            _mouse.reset();
        }
        if (_deckManager) {
            _deckManager.reset();
        }
        if (_client) {
            _client->close();
            _client.reset();
        }
        if (_capturer) {
            _capturer.reset();
        }
        if (_virtualDisplay) {
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
        }

        adb_forward_remove(port);

        _isStreaming = NO;
        [self updateStatusText:@"Status: Disconnected"];
        [self updateConnectMenuTitle:@"Connect"];

        NSLog(@"[Stream] Disconnected");
    }
}

// -----------------------------------------------------------------------------
#pragma mark - Deck Timer (volume/media state push to Android)
// -----------------------------------------------------------------------------

- (void)pushDeckVolumeState {
    droidscreen::Pipeline* pl = _pipeline.get();
    if (!pl || !pl->is_running()) return;

    float vol = getSystemVolume();
    bool muted = getSystemMuted();
    uint16_t level = (uint16_t)(vol * 65535);
    pl->send_volume_state(level, muted);
}

- (void)pushDeckMediaState {
    droidscreen::Pipeline* pl = _pipeline.get();
    if (!pl || !pl->is_running()) return;

    @try {
        BOOL helperTimedOut = NO;
        NSDictionary* snapshot = fetchNowPlayingSnapshot(&helperTimedOut);
        NSString* title = snapshot[@"title"];
        if (title.length > 0) {
            _lastNowPlayingSnapshot = [snapshot copy];
            _lastNowPlayingSnapshotAt = [NSDate date];
        } else if (helperTimedOut && _lastNowPlayingSnapshot != nil) {
            NSMutableDictionary* fallback = [_lastNowPlayingSnapshot mutableCopy];
            NSNumber* elapsedBase = fallback[@"elapsedTimeNow"] ?: fallback[@"elapsedTime"];
            NSNumber* duration = fallback[@"duration"];
            NSNumber* playing = fallback[@"playing"];
            NSDate* lastAt = _lastNowPlayingSnapshotAt ?: [NSDate date];
            if ([playing boolValue] && [elapsedBase isKindOfClass:[NSNumber class]]) {
                double elapsed = [elapsedBase doubleValue] +
                    [[NSDate date] timeIntervalSinceDate:lastAt];
                if ([duration isKindOfClass:[NSNumber class]] && duration.doubleValue > 0.0) {
                    elapsed = MIN(elapsed, duration.doubleValue);
                }
                fallback[@"elapsedTimeNow"] = @(MAX(0.0, elapsed));
            }
            snapshot = fallback;
        }

        NSString* trackKey = mediaTrackKeyForSnapshot(snapshot);
        bool includeArtwork = _forceArtworkPush;
        if (!includeArtwork && trackKey.length > 0 &&
            ![_lastMediaArtworkTrackKey isEqualToString:trackKey]) {
            includeArtwork = true;
        }

        std::string json = buildMediaStateJSON(snapshot, includeArtwork);
        if (includeArtwork && trackKey.length > 0) {
            _lastMediaArtworkTrackKey = [trackKey copy];
            _forceArtworkPush = NO;
        }

        fprintf(stderr, "[deck] Sending media state: %.80s...\n", json.c_str());
        pl->send_media_state(json);
    } @catch (NSException* e) {
        fprintf(stderr, "[deck] Exception reading media state helper: %s\n",
                [[e description] UTF8String]);
    }
}

- (void)pushDeckState {
    [self pushDeckVolumeState];
    [self pushDeckMediaState];
}

- (void)scheduleDeckMediaRefreshBurst {
    if (!_deckStateQueue) return;
    _forceArtworkPush = YES;
    dispatch_async(_deckStateQueue, ^{
        [self pushDeckMediaState];
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC),
                   _deckStateQueue, ^{
        [self pushDeckMediaState];
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC),
                   _deckStateQueue, ^{
        [self pushDeckMediaState];
    });
}

- (void)startDeckTimer {
    [self stopDeckTimer];
    _deckTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                        _deckStateQueue);
    dispatch_source_set_timer(_deckTimer, DISPATCH_TIME_NOW,
                              1 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(_deckTimer, ^{
        fprintf(stderr, "[deck] Timer tick\n");
        [self pushDeckState];
    });
    dispatch_resume(_deckTimer);
    NSLog(@"[deck] Started deck state timer");
}

- (void)stopDeckTimer {
    if (_deckTimer) {
        dispatch_source_cancel(_deckTimer);
        _deckTimer = nil;
        NSLog(@"[deck] Stopped deck state timer");
    }
}

// -----------------------------------------------------------------------------
#pragma mark - Stats Timer (health monitoring on main thread)
// -----------------------------------------------------------------------------

- (void)startStatsTimer {
    [self stopStatsTimer];
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                   target:self
                                                 selector:@selector(statsTimerFired:)
                                                 userInfo:nil
                                                  repeats:YES];
}

- (void)stopStatsTimer {
    [_statsTimer invalidate];
    _statsTimer = nil;
}

- (void)statsTimerFired:(NSTimer*)timer {
    // Safety: _pipeline may be reset on _streamQueue while this fires on main thread.
    if (!_isStreaming) return;

    droidscreen::Pipeline* pl = _pipeline.get();
    if (!pl) return;

    @try {
        // Check if pipeline is still running (connection may have dropped).
        if (!pl->is_running()) {
            NSLog(@"[Stream] Pipeline stopped unexpectedly, disconnecting...");
            dispatch_async(_streamQueue, ^{
                [self disconnectSync];
            });
            return;
        }

        // Log stats.
        uint64_t enc  = pl->frames_encoded();
        uint64_t byt  = pl->bytes_sent();
        int64_t  rtt  = pl->last_rtt_us();

        NSLog(@"[Stats] encoded=%llu bytes=%llu rtt=%lld us",
              (unsigned long long)enc, (unsigned long long)byt, (long long)rtt);
    } @catch (NSException* e) {
        NSLog(@"[Stats] Exception in statsTimerFired: %@", e);
    }
}

// -----------------------------------------------------------------------------
#pragma mark - ADB Device Detection Timer
// -----------------------------------------------------------------------------

- (void)startDeviceDetectionTimer {
    _deviceTimer = [NSTimer scheduledTimerWithTimeInterval:3.0
                                                    target:self
                                                  selector:@selector(deviceTimerFired:)
                                                  userInfo:nil
                                                   repeats:YES];
}

- (void)stopDeviceDetectionTimer {
    [_deviceTimer invalidate];
    _deviceTimer = nil;
}

- (void)deviceTimerFired:(NSTimer*)timer {
    // Run device check off the main thread.
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        BOOL connected = adb_device_connected();

        if (!connected) {
            // Device physically removed — reset the user-disconnect flag so
            // auto-connect kicks in when the device is plugged back in.
            self->_userDisconnected = NO;

            if (self->_isStreaming) {
                NSLog(@"[AutoConnect] Device disconnected, stopping...");
                dispatch_async(self->_streamQueue, ^{
                    if (self->_isStreaming) {
                        [self disconnectSync];
                    }
                });
            }
        } else if (connected && !self->_isStreaming && !self->_isBusy
                   && !self->_userDisconnected) {
            // Device connected but not streaming — check if DroidScreen app is running.
            if (adb_droidscreen_running()) {
                NSLog(@"[AutoConnect] DroidScreen detected on device, connecting...");
                dispatch_async(self->_streamQueue, ^{
                    if (!self->_isStreaming) {
                        [self connectSync];
                    }
                });
            }
        }
    });
}

@end

// =============================================================================
#pragma mark - Main Entry Point
// =============================================================================

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        // Debug logging: all output goes to ~/Library/Logs/DroidScreen/droidscreen.log
        setup_debug_logging();
        install_crash_handlers();
        NSSetUncaughtExceptionHandler(objc_exception_handler);

        NSApplication* app = [NSApplication sharedApplication];

        // No dock icon -- menu bar only.
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;

        NSLog(@"[DroidScreen] Starting menu bar app...");
        NSLog(@"[DroidScreen] Debug log: %@", g_logFilePath);

        [app run];

        return 0;
    }
}

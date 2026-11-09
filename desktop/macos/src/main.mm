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
#import <QuartzCore/QuartzCore.h>
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
#include "wifi_discovery.h"
#import "ui/DSUIBridge.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
#include <cmath>
#include <signal.h>
#include <spawn.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <execinfo.h>

// =============================================================================
#pragma mark - Validation Scene
// =============================================================================

@interface ValidationSceneView : NSView
@end

@implementation ValidationSceneView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        CALayer* root = [CALayer layer];
        root.frame = self.bounds;
        root.backgroundColor = NSColor.blackColor.CGColor;
        root.needsDisplayOnBoundsChange = YES;
        self.layer = root;
        [self buildValidationLayers];
    }
    return self;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)buildValidationLayers {
    NSRect b = self.bounds;
    CALayer* root = self.layer;
    root.sublayers = @[];

    CAGradientLayer* bg = [CAGradientLayer layer];
    bg.frame = CGRectMake(0, 0, b.size.width, b.size.height);
    bg.colors = @[
        (id)[NSColor colorWithCalibratedRed:0.04 green:0.07 blue:0.10 alpha:1.0].CGColor,
        (id)[NSColor colorWithCalibratedRed:0.10 green:0.18 blue:0.22 alpha:1.0].CGColor
    ];
    bg.startPoint = CGPointMake(0, 0);
    bg.endPoint = CGPointMake(1, 1);
    [root addSublayer:bg];

    NSArray<NSColor*>* colors = @[
        [NSColor colorWithCalibratedRed:0.95 green:0.22 blue:0.18 alpha:1.0],
        [NSColor colorWithCalibratedRed:0.10 green:0.74 blue:0.55 alpha:1.0],
        [NSColor colorWithCalibratedRed:0.20 green:0.43 blue:0.95 alpha:1.0],
        [NSColor colorWithCalibratedRed:0.98 green:0.76 blue:0.20 alpha:1.0]
    ];

    CGFloat stripeW = MAX(44.0, b.size.width / 18.0);
    for (int i = 0; i < 4; i++) {
        CALayer* stripe = [CALayer layer];
        stripe.frame = CGRectMake(-stripeW, b.size.height * (0.16 + i * 0.18),
                                  stripeW, b.size.height * 0.14);
        stripe.backgroundColor = colors[i % colors.count].CGColor;
        stripe.opacity = 0.88;
        stripe.cornerRadius = 10;
        [root addSublayer:stripe];

        CABasicAnimation* move = [CABasicAnimation animationWithKeyPath:@"position.x"];
        move.fromValue = @(-stripeW * 2.0);
        move.toValue = @(b.size.width + stripeW * 2.0);
        move.duration = 0.85 + i * 0.08;
        move.repeatCount = HUGE_VALF;
        move.beginTime = CACurrentMediaTime() + i * 0.16;
        move.timingFunction =
            [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
        [stripe addAnimation:move forKey:@"sweep"];
    }

    CALayer* marker = [CALayer layer];
    marker.frame = CGRectMake(40, b.size.height - 120, 92, 92);
    marker.backgroundColor = NSColor.whiteColor.CGColor;
    marker.cornerRadius = 46;
    [root addSublayer:marker];

    CABasicAnimation* markerMove = [CABasicAnimation animationWithKeyPath:@"position.x"];
    markerMove.fromValue = @(70.0);
    markerMove.toValue = @(b.size.width - 70.0);
    markerMove.duration = 1.15;
    markerMove.autoreverses = YES;
    markerMove.repeatCount = HUGE_VALF;
    markerMove.timingFunction =
        [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
    [marker addAnimation:markerMove forKey:@"markerMove"];

    CATextLayer* text = [CATextLayer layer];
    text.frame = CGRectMake(48, 42, b.size.width - 96, 72);
    text.contentsScale = NSScreen.mainScreen.backingScaleFactor;
    text.string = @"DroidScreen latency validation";
    text.foregroundColor = NSColor.whiteColor.CGColor;
    text.fontSize = 44;
    text.alignmentMode = kCAAlignmentLeft;
    [root addSublayer:text];
}

@end

static int run_validation_scene_helper(uint32_t displayID) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSScreen* targetScreen = nil;
        for (int attempt = 0; attempt < 50 && !targetScreen; attempt++) {
            for (NSScreen* screen in NSScreen.screens) {
                NSNumber* screenID = screen.deviceDescription[@"NSScreenNumber"];
                if (screenID && screenID.unsignedIntValue == displayID) {
                    targetScreen = screen;
                    break;
                }
            }
            if (!targetScreen) {
                usleep(100000);
            }
        }

        if (!targetScreen) {
            fprintf(stderr,
                    "[validation-helper] screen %u not visible to AppKit\n",
                    displayID);
            fflush(stderr);
            return 2;
        }

        NSRect frame = targetScreen.frame;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO
                                                          screen:targetScreen];
        window.releasedWhenClosed = NO;
        window.backgroundColor = NSColor.blackColor;
        window.opaque = YES;
        window.ignoresMouseEvents = YES;
        window.level = NSStatusWindowLevel;
        window.collectionBehavior =
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorFullScreenAuxiliary |
            NSWindowCollectionBehaviorStationary;
        window.contentView =
            [[ValidationSceneView alloc] initWithFrame:NSMakeRect(0, 0,
                                                                  frame.size.width,
                                                                  frame.size.height)];
        [window setFrame:frame display:YES];
        [window.contentView displayIfNeeded];
        [window makeKeyAndOrderFront:nil];
        [window orderFrontRegardless];
        [window displayIfNeeded];
        [app activateIgnoringOtherApps:YES];
        [CATransaction flush];

        fprintf(stderr,
                "[validation-helper] scene visible display=%u frame=%s level=%ld\n",
                displayID, NSStringFromRect(frame).UTF8String, (long)window.level);
        fflush(stderr);

        [app run];
        (void)window;
        return 0;
    }
}

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

// =============================================================================
#pragma mark - Long-lived child processes
// =============================================================================

extern char** environ;

/// A child process kept alive for a long time (the MediaRemote helper, `adb
/// track-devices`). Its stdout is delivered in chunks on `queue`; `onExit`
/// runs on `queue` when the child exits on its own (not after -terminate).
@interface DSChildProcess : NSObject
+ (instancetype)spawn:(NSString*)path
                 args:(NSArray<NSString*>*)args
            withStdin:(BOOL)withStdin
          stderrToLog:(BOOL)stderrToLog
                queue:(dispatch_queue_t)queue
               onData:(void (^)(NSData* data))onData
               onExit:(void (^)(int status))onExit;
/// Writes a line to the child's stdin (only with withStdin). Never raises SIGPIPE.
- (void)writeLine:(NSString*)line;
- (void)terminate;
@end

@implementation DSChildProcess {
    pid_t _pid;
    int _stdinFd;
    dispatch_source_t _readSource;
    std::shared_ptr<std::atomic<bool>> _terminated;
    std::shared_ptr<std::atomic<bool>> _reaped;  // never signal a reaped (reusable) pid
}

+ (instancetype)spawn:(NSString*)path
                 args:(NSArray<NSString*>*)args
            withStdin:(BOOL)withStdin
          stderrToLog:(BOOL)stderrToLog
                queue:(dispatch_queue_t)queue
               onData:(void (^)(NSData* data))onData
               onExit:(void (^)(int status))onExit {
    int outPipe[2] = {-1, -1};
    int inPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0) return nil;
    if (withStdin && pipe(inPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return nil;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (withStdin) {
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    if (stderrToLog) {
        posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Only stdin/stdout/stderr reach the child (no sockets, no log fd copies).
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);

    std::vector<std::string> argStorage;
    argStorage.emplace_back(path.fileSystemRepresentation);
    for (NSString* arg in args) argStorage.emplace_back(arg.UTF8String ?: "");
    std::vector<char*> argv;
    for (auto& arg : argStorage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, argStorage[0].c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(outPipe[1]);
    if (withStdin) close(inPipe[0]);
    if (rc != 0) {
        NSLog(@"[child] posix_spawn %@ failed: %s", path, strerror(rc));
        close(outPipe[0]);
        if (withStdin) close(inPipe[1]);
        return nil;
    }

    DSChildProcess* proc = [[DSChildProcess alloc] init];
    proc->_pid = pid;
    proc->_stdinFd = withStdin ? inPipe[1] : -1;
    if (proc->_stdinFd >= 0) {
        fcntl(proc->_stdinFd, F_SETNOSIGPIPE, 1);
        fcntl(proc->_stdinFd, F_SETFL, fcntl(proc->_stdinFd, F_GETFL) | O_NONBLOCK);
    }
    auto terminated = std::make_shared<std::atomic<bool>>(false);
    proc->_terminated = terminated;
    auto reaped = std::make_shared<std::atomic<bool>>(false);
    proc->_reaped = reaped;

    const int readFd = outPipe[0];
    fcntl(readFd, F_SETFL, fcntl(readFd, F_GETFL) | O_NONBLOCK);
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)readFd, 0, queue);
    __unsafe_unretained dispatch_source_t sourceRef = source;
    dispatch_source_set_event_handler(source, ^{
        char buffer[16384];
        for (;;) {
            ssize_t count = read(readFd, buffer, sizeof(buffer));
            if (count > 0) {
                onData([NSData dataWithBytes:buffer length:(NSUInteger)count]);
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
            dispatch_source_cancel(sourceRef);  // EOF or error: the child is gone
            return;
        }
    });
    dispatch_source_set_cancel_handler(source, ^{
        close(readFd);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            int status = 0;
            waitpid(pid, &status, 0);
            reaped->store(true);
            dispatch_async(queue, ^{
                if (!terminated->load() && onExit) onExit(status);
            });
        });
    });
    proc->_readSource = source;
    dispatch_resume(source);
    return proc;
}

- (void)writeLine:(NSString*)line {
    if (_stdinFd < 0 || _terminated->load()) return;
    std::string data = std::string(line.UTF8String ?: "") + "\n";
    if (write(_stdinFd, data.data(), data.size()) < 0) {
        NSLog(@"[child] write to pid %d failed: %s", (int)_pid, strerror(errno));
    }
}

- (void)terminate {
    if (_terminated->exchange(true)) return;
    if (_stdinFd >= 0) {
        close(_stdinFd);
        _stdinFd = -1;
    }
    if (!_reaped->load()) kill(_pid, SIGTERM);
    dispatch_source_cancel(_readSource);  // the cancel handler reaps the child
}

- (void)dealloc {
    [self terminate];
}

@end

/// Splits `buffer` at newlines, consuming complete lines.
static void drain_lines(NSMutableData* buffer, void (^onLine)(NSData* line)) {
    for (;;) {
        const char* bytes = (const char*)buffer.bytes;
        const char* newline = (const char*)memchr(bytes, '\n', buffer.length);
        if (!newline) return;
        NSUInteger length = (NSUInteger)(newline - bytes);
        NSData* line = [NSData dataWithBytes:bytes length:length];
        [buffer replaceBytesInRange:NSMakeRange(0, length + 1) withBytes:NULL length:0];
        if (length > 0) onLine(line);
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
static NSString* const kSettingMinIdleFPS   = @"DroidScreenMinIdleFPS";
static NSString* const kSettingBitrate      = @"DroidScreenBitrate";      // kbps
static NSString* const kSettingResolution   = @"DroidScreenResolution";   // index
static NSString* const kSettingScale        = @"DroidScreenScale";        // index
static NSString* const kSettingPort         = @"DroidScreenPort";
static NSString* const kSettingDeckApps     = @"DroidScreenDeckApps";     // array of dicts
static NSString* const kSettingDesktopID    = @"DroidScreenDesktopID";    // hex, identifies this Mac to tablets
static NSString* const kSettingWifiPairings = @"DroidScreenWifiPairings"; // {tablet id: token hex}
// Per-tablet stream settings: {device key: {name, fps, min_idle_fps, bitrate, resolution, hidpi}}.
// Device key = Wi-Fi tablet id, or "usb:<adb serial>". Missing values fall back
// to the global keys above, which act as the defaults for new tablets.
static NSString* const kSettingDeviceSettings = @"DroidScreenDeviceSettings";

// -----------------------------------------------------------------------------
#pragma mark - Wi-Fi identity and pairing storage
// -----------------------------------------------------------------------------

static NSString* hex_from_bytes(const uint8_t* bytes, size_t len) {
    NSMutableString* out = [NSMutableString stringWithCapacity:len * 2];
    for (size_t i = 0; i < len; ++i) [out appendFormat:@"%02x", bytes[i]];
    return out;
}

static bool bytes_from_hex(NSString* hex, uint8_t* out, size_t len) {
    if (hex.length != len * 2) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned int byte = 0;
        NSString* pair = [hex substringWithRange:NSMakeRange(i * 2, 2)];
        if (![[NSScanner scannerWithString:pair] scanHexInt:&byte]) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

/// Stable random id for this Mac, created on first use.
static void desktop_id(uint8_t out[DS_HELLO_ID_SIZE]) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* stored = [defaults stringForKey:kSettingDesktopID];
    if (stored && bytes_from_hex(stored, out, DS_HELLO_ID_SIZE)) return;
    arc4random_buf(out, DS_HELLO_ID_SIZE);  // CSPRNG on macOS
    [defaults setObject:hex_from_bytes(out, DS_HELLO_ID_SIZE) forKey:kSettingDesktopID];
}

static NSString* desktop_display_name() {
    NSString* name = [[NSHost currentHost] localizedName];
    return name.length ? name : @"Mac";
}

static NSString* stored_pairing_token(const std::string& tablet_id) {
    NSDictionary* pairings = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kSettingWifiPairings];
    return pairings[[NSString stringWithUTF8String:tablet_id.c_str()]];
}

static void store_pairing_token(const std::string& tablet_id, NSString* token) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary* pairings =
        [([defaults dictionaryForKey:kSettingWifiPairings] ?: @{}) mutableCopy];
    pairings[[NSString stringWithUTF8String:tablet_id.c_str()]] = token;
    [defaults setObject:pairings forKey:kSettingWifiPairings];
}

/// Set by `--wifi [name|id]` for automated tests: stream over Wi-Fi only (even
/// with a cable attached), to the paired tablet matching the value ("" = any).
static bool g_cliForceWifi = false;
static std::string g_cliWifiMatch;

/// A tablet we may auto-connect to over Wi-Fi: paired, compatible, and
/// matching `--wifi <name|id>` when given.
static bool wifi_tablet_eligible(const droidscreen::WifiTablet& tablet) {
    bool matches = g_cliWifiMatch.empty() || tablet.id == g_cliWifiMatch ||
                   tablet.service_name == g_cliWifiMatch;
    return matches && stored_pairing_token(tablet.id) &&
           tablet.protocol_version == DS_PROTOCOL_VERSION;
}

/// Where a stream goes: through the adb tunnel or straight to a Wi-Fi tablet.
struct StreamTarget {
    bool wifi = false;
    droidscreen::WifiTablet tablet;  // valid when wifi
};

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
// Bitrate cap presets (kbps), 5 to 80 Mbps. The speed test picks one of these.
static const int kBitrates[]     = { 5000, 10000, 15000, 20000, 25000, 30000, 35000, 40000,
                                     45000, 50000, 55000, 60000, 65000, 70000, 75000, 80000 };
static const int kBitrateCount = sizeof(kBitrates) / sizeof(kBitrates[0]);
static const uint32_t kMinIdleFPSOptions[] = { 1, 10, 15, 30 };
static NSString* const kMinIdleFPSLabels[] = {
    @"1 fps", @"10 fps", @"15 fps", @"30 fps"
};
static const int kMinIdleFPSCount =
    sizeof(kMinIdleFPSOptions) / sizeof(kMinIdleFPSOptions[0]);

static uint32_t sanitize_min_idle_fps(uint32_t fps) {
    switch (fps) {
        case 1:
        case 10:
        case 15:
        case 30:
            return fps;
        default:
            return 10;
    }
}

static NSInteger min_idle_fps_index(uint32_t fps) {
    fps = sanitize_min_idle_fps(fps);
    for (NSInteger i = 0; i < kMinIdleFPSCount; i++) {
        if (kMinIdleFPSOptions[i] == fps) return i;
    }
    return kMinIdleFPSCount - 1;
}

// =============================================================================
#pragma mark - ADB Helpers
// =============================================================================

/// Finds the full path to the `adb` binary. Caches the result.
/// Searches common Homebrew/Android SDK locations + user's shell PATH.
static NSString* adb_find_path() {
    static NSString* cached = nil;
    if (cached) return cached;

    // 1. Bundled ADB inside the .app (injected by CI/installer).
    //    Path: <App>.app/Contents/Resources/adb/adb
    NSString* bundleAdb = [[[NSBundle mainBundle] resourcePath]
                            stringByAppendingPathComponent:@"adb/adb"];
    if ([[NSFileManager defaultManager] isExecutableFileAtPath:bundleAdb]) {
        cached = bundleAdb;
        NSLog(@"[ADB] Found adb in bundle: %@", cached);
        return cached;
    }

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

/// adb also lists wireless-debugging devices ("ip:port", "adb-…._adb-tls-connect._tcp").
/// Those are not a cable: the stream should use the Wi-Fi path for them.
static BOOL adb_serial_is_usb(NSString* serial) {
    if (serial.length == 0) return NO;
    if ([serial containsString:@"._adb-tls-connect."] || [serial containsString:@"._adb._tcp"]) return NO;
    NSRange colon = [serial rangeOfString:@":" options:NSBackwardsSearch];
    if (colon.location != NSNotFound) return NO;   // host:port
    return YES;
}

/// Serial of the adb-attached device, or nil.
static NSString* adb_device_serial() {
    NSString* serial = [adb_run_output(@[@"get-serialno"])
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (serial.length == 0 || [serial hasPrefix:@"error"] || [serial isEqualToString:@"unknown"]) return nil;
    return serial;
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
// =============================================================================
#pragma mark - AppDelegate
// =============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, DSUIHost>

// Deck shortcuts (mirrors kSettingDeckApps).
@property (nonatomic, strong) NSMutableArray<NSDictionary*>* deckApps;

// State.
@property (nonatomic, assign) BOOL isStreaming;
@property (nonatomic, assign) BOOL isBusy;  // Prevents concurrent connect/disconnect ops.
@property (nonatomic, assign) BOOL userDisconnected;  // Suppresses auto-connect until device is re-plugged or user clicks Connect.

- (void)pushDeckVolumeStateForce:(BOOL)force;
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

    // USB device detection: one long-lived `adb track-devices` process, its
    // queue (owns every _adbTrack* field) and the idle auto-connect retry.
    DSChildProcess* _adbTracker;
    NSMutableData* _adbTrackBuffer;
    dispatch_queue_t _adbTrackQueue;
    BOOL _adbTrackActive;
    NSString* _adbTrackUsbSerial;
    NSTimer* _autoConnectTimer;

    // Wi-Fi: Bonjour discovery, the transport of the active stream, and the
    // tablet the user picked from the menu (empty = automatic).
    std::unique_ptr<droidscreen::WifiDiscovery> _wifiDiscovery;
    BOOL _streamingOverWifi;
    std::string _wifiTargetId;
    std::string _activeWifiTabletId;

    // Interface (SwiftUI) and what it shows.
    id<DSUIController> _ui;
    NSString* _statusDetail;
    NSString* _activeTabletName;
    NSString* _activeAddress;
    NSString* _activeCodec;
    NSString* _activeDeviceKey;
    NSString* _usbDeviceKey;
    NSString* _usbDeviceName;
    BOOL _uiPending;
    BOOL _activeHiDPI;
    uint32_t _activeBitrateKbps;
    BOOL _usbDevicePresent;
    unsigned _statsTick;

    // Stats timer.
    NSTimer* _statsTimer;
    uint64_t _lastStatsCaptured;
    uint64_t _lastStatsEncoded;
    uint64_t _lastStatsDropped;
    uint64_t _lastStatsBytes;
    uint64_t _lastStatsEncSum;
    uint64_t _lastStatsEncN;
    CFAbsoluteTime _lastStatsTime;

    // Validation-only animated window. Enabled by the validation script via
    // DROIDSCREEN_VALIDATION_SCENE=1 and pinned to the virtual display ID.
    NSWindow* _validationWindow;
    NSTask* _validationTask;

    // Deck volume/media feed to Android. Everything below runs on
    // _deckStateQueue: a CoreAudio listener for volume, one persistent
    // MediaRemote helper for Now Playing, and a 1 s progress tick while
    // something plays (the tablet does not advance the clock on its own).
    dispatch_queue_t _deckStateQueue;
    BOOL _deckActive;
    dispatch_source_t _deckProgressTimer;
    DSChildProcess* _nowPlayingHelper;
    NSMutableData* _nowPlayingBuffer;
    NSDictionary* _nowPlayingSnapshot;
    CFAbsoluteTime _nowPlayingSnapshotAt;
    std::string _lastSentMediaJSON;
    BOOL _lastLoggedPlaying;
    AudioObjectPropertyListenerBlock _defaultOutputListener;
    AudioObjectPropertyListenerBlock _outputVolumeListener;
    AudioDeviceID _observedOutputDevice;
    int32_t _lastSentVolumeLevel;  // -1 = nothing sent yet
    int _lastSentVolumeMuted;

    // Current streaming parameters (for status display).
    uint32_t _streamWidth;
    uint32_t _streamHeight;
    uint32_t _streamFPS;

    NSString* _lastMediaArtworkTrackKey;
    BOOL _forceArtworkPush;
    NSString* _lastObservedTrackKey;
    // Bumped on every tablet media action; helper snapshots stamped with an
    // older generation (taken before the action) are dropped.
    std::atomic<uint64_t> _mediaActionGen;
}

// -----------------------------------------------------------------------------
#pragma mark - App Lifecycle
// -----------------------------------------------------------------------------

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // Register default settings.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kSettingFPS:         @60,
        kSettingMinIdleFPS:  @10,  // measured: same wake latency as 30, ~1/3 less tablet CPU
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

    [self loadDeckApps];
    _ui = DSUICreateController(self);
    [self pushUIState];
    [self startDeviceDetection];
    [self startWifiDiscovery];

    // Request screen capture permission once at launch.
    if (!CGPreflightScreenCaptureAccess()) {
        NSLog(@"[DroidScreen] Screen recording permission not granted, requesting...");
        CGRequestScreenCaptureAccess();
    }

    if (g_cliForceWifi) {
        NSLog(@"[DroidScreen] --wifi: streaming over Wi-Fi only (match='%s')",
              g_cliWifiMatch.c_str());
    }
    NSLog(@"[DroidScreen] Menu bar app launched");
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    [self stopDeviceDetection];
    if (_wifiDiscovery) _wifiDiscovery->stop();
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
#pragma mark - Interface (SwiftUI menu bar popover + settings, see src/ui/)
// -----------------------------------------------------------------------------

static NSString* strip_status_prefix(NSString* text) {
    return [text hasPrefix:@"Status: "] ? [text substringFromIndex:8] : text;
}

static NSString* codec_name(ds_codec_t codec) {
    switch (codec) {
        case DS_CODEC_HEVC: return @"HEVC";
        default:            return @"H.264";
    }
}

/// Remote address of a connected socket, or nil.
static NSString* peer_address(int fd) {
    if (fd < 0) return nil;
    struct sockaddr_storage addr {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) return nil;
    char host[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr, host, sizeof(host));
    } else if (addr.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_addr, host, sizeof(host));
    } else {
        return nil;
    }
    return [NSString stringWithUTF8String:host];
}

/// Friendly name of the adb-attached tablet ("POCO Pad"), falling back to the model.
static NSString* adb_device_name() {
    NSString* name = [adb_run_output(@[@"shell", @"settings", @"get", @"global", @"device_name"])
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (name.length == 0 || [name isEqualToString:@"null"]) {
        name = [adb_run_output(@[@"shell", @"getprop", @"ro.product.model"])
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    }
    return name.length ? name : @"USB tablet";
}

/// Status line from the connection code. Safe from any thread.
- (void)updateStatusText:(NSString*)text {
    NSString* detail = strip_status_prefix(text);
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_statusDetail = detail;
        [self pushUIState];
    });
}

/// Kept for the connection code: the title itself now comes from the phase.
- (void)updateConnectMenuTitle:(NSString*)title {
    BOOL pending = [title hasSuffix:@"..."];
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_uiPending = pending;
        [self pushUIState];
    });
}

/// Publish the connection state to the interface. Main thread.
- (void)pushUIState {
    if (!_ui) return;
    NSString* phase = _isStreaming ? @"streaming" : ((_isBusy || _uiPending) ? @"connecting" : @"idle");
    NSMutableDictionary* state = [@{
        @"phase":     phase,
        @"detail":    _statusDetail ?: @"Not connected",
        @"usbDevice": @(_usbDevicePresent),
    } mutableCopy];
    if (_usbDevicePresent && _usbDeviceKey) {
        state[@"usbDeviceKey"] = _usbDeviceKey;
        state[@"usbDeviceName"] = _usbDeviceName ?: @"USB tablet";
    }
    if (_isStreaming || _isBusy) {
        state[@"transport"] = _streamingOverWifi ? @"wifi" : @"usb";
        if (_activeTabletName) state[@"tabletName"] = _activeTabletName;
        if (_activeDeviceKey) state[@"deviceKey"] = _activeDeviceKey;
        if (_activeAddress) state[@"address"] = _activeAddress;
    }
    if (_isStreaming) {
        state[@"width"] = @(_streamWidth);
        state[@"height"] = @(_streamHeight);
        state[@"fps"] = @(_streamFPS);
        state[@"hidpi"] = @(_activeHiDPI);
        state[@"codec"] = _activeCodec ?: @"HEVC";
    }
    [_ui updateState:state];
}

/// Publish the Wi-Fi tablets seen on the network. Main thread.
- (void)rebuildWifiMenu {
    if (!_ui) return;
    NSMutableArray* list = [NSMutableArray array];
    auto tablets = _wifiDiscovery ? _wifiDiscovery->tablets() : std::vector<droidscreen::WifiTablet>{};
    for (const auto& tablet : tablets) {
        bool current = _isStreaming && _streamingOverWifi && tablet.id == _activeWifiTabletId;
        [list addObject:@{
            @"id":         [NSString stringWithUTF8String:tablet.id.c_str()],
            @"name":       [NSString stringWithUTF8String:tablet.service_name.c_str()],
            @"paired":     @(stored_pairing_token(tablet.id) != nil),
            @"compatible": @(tablet.protocol_version == DS_PROTOCOL_VERSION),
            @"current":    @(current),
        }];
    }
    [_ui updateTablets:list];
}

- (void)loadDeckApps {
    NSArray* saved = [[NSUserDefaults standardUserDefaults] arrayForKey:kSettingDeckApps];
    self.deckApps = saved ? [saved mutableCopy] : [NSMutableArray new];
}

// ---------------------------------------------------------------------------
// Extract a 64x64 PNG icon from an .app path, returned as base64 string.
// ---------------------------------------------------------------------------
- (NSString*)extractIconBase64ForAppAtPath:(NSString*)appPath {
    if (!appPath || ![[NSFileManager defaultManager] fileExistsAtPath:appPath])
        return @"";

    NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:appPath];
    if (!icon) return @"";

    NSSize targetSize = NSMakeSize(64, 64);
    NSImage* resized = [[NSImage alloc] initWithSize:targetSize];
    [resized lockFocus];
    [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];
    [icon drawInRect:NSMakeRect(0, 0, 64, 64)
            fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver
            fraction:1.0];
    [resized unlockFocus];

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

    if (!self.deckApps) [self loadDeckApps];

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

        NSString* iconB64 = [self extractIconBase64ForAppAtPath:app[@"path"]];
        tile.icon_b64 = [iconB64 UTF8String] ?: "";

        layout.tiles.push_back(std::move(tile));
    }

    int appRows = (int)((self.deckApps.count + layout.grid_cols - 1) / layout.grid_cols);
    layout.grid_rows = 1 + (appRows > 1 ? appRows : 1);

    _deckManager->set_layout(layout);

    if (_isStreaming && _pipeline && _pipeline->is_running()) {
        _pipeline->send_deck_config();
        NSLog(@"[deck] Pushed updated deck config to Android (%lu app tiles)",
              (unsigned long)self.deckApps.count);
    }
}

// DSUIHost ----------------------------------------------------------------

- (void)uiToggleConnect {
    [self toggleConnect:nil];
}

- (void)uiConnectWifiTablet:(NSString*)tabletId {
    if (!tabletId.length) return;
    _wifiTargetId = tabletId.UTF8String;
    _userDisconnected = NO;
    [self updateConnectMenuTitle:@"Connecting..."];
    dispatch_async(_streamQueue, ^{
        if (self->_isStreaming) {
            [self disconnectSync];
        }
        [self connectSync];
        [self updateConnectMenuTitle:@""];
        dispatch_async(dispatch_get_main_queue(), ^{ [self rebuildWifiMenu]; });
    });
}

- (void)uiSettingsChanged:(BOOL)reconnect {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSLog(@"[Settings] Saved: fps=%ld min_idle_fps=%ld bitrate=%ld res=%ld retina=%d port=%ld",
          (long)[defaults integerForKey:kSettingFPS],
          (long)[defaults integerForKey:kSettingMinIdleFPS],
          (long)[defaults integerForKey:kSettingBitrate],
          (long)[defaults integerForKey:kSettingResolution],
          (int)[defaults boolForKey:kSettingScale],
          (long)[defaults integerForKey:kSettingPort]);

    if (reconnect && _isStreaming) {
        NSLog(@"[Settings] Restarting pipeline with new settings...");
        [self updateConnectMenuTitle:@"Connecting..."];
        dispatch_async(_streamQueue, ^{
            [self disconnectSync];
            // Wait for the Android side to finish cleaning up the old
            // connection and re-accept. Without this delay, adb forward
            // fails because the old socket is still in TIME_WAIT.
            usleep(500000);  // 500ms
            [self connectSync];
            [self updateConnectMenuTitle:@""];
        });
    }
}

- (void)uiDeckAppsChanged {
    [self loadDeckApps];
    [self rebuildDeckLayout];
}

- (void)uiForgetTablet:(NSString*)tabletId {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary* pairings = [[defaults dictionaryForKey:kSettingWifiPairings] mutableCopy];
    if (pairings[tabletId]) {
        [pairings removeObjectForKey:tabletId];
        [defaults setObject:pairings forKey:kSettingWifiPairings];
        NSLog(@"[Wi-Fi] Forgot pairing with tablet %@", tabletId);
    }
    [self rebuildWifiMenu];
}

- (void)uiRunSpeedTest {
    [self runSpeedTest:nil];
}

- (void)uiQuit {
    [self quitApp:nil];
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
    req.reserved[0] = DS_CODEC_CAP_H264;

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

            StreamTarget target;
            NSString* targetError = nil;
            if (![self chooseTarget:&target error:&targetError]) {
                NSLog(@"[SpeedTest] %@", targetError);
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            std::unique_ptr<droidscreen::TCPClient> client;
            NSString* openError = nil;
            if (![self openTransport:target port:port client:client error:&openError]) {
                NSLog(@"[SpeedTest] %@", openError);
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            // Handshake (so Android enters message loop).
            if (!speed_test_handshake(client.get())) {
                NSLog(@"[SpeedTest] Handshake failed");
                client->close();
                if (!target.wifi) adb_forward_remove(port);
                [self speedTestFinished:0 wasStreaming:wasStreaming];
                return;
            }

            // Run speed test (2 seconds).
            uint32_t throughput_kbps = droidscreen::run_speed_test(client.get(), 2000);

            // Disconnect and clean up.
            client->close();
            if (!target.wifi) adb_forward_remove(port);

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

    if (throughput_kbps > 0) {
        [[NSUserDefaults standardUserDefaults] setInteger:kBitrates[bestIdx]
                                                   forKey:kSettingBitrate];
        NSLog(@"[SpeedTest] Set bitrate to %d kbps (throughput: %u kbps)",
              kBitrates[bestIdx], throughput_kbps);
    }
    NSInteger selected = throughput_kbps > 0 ? kBitrates[bestIdx] : 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_ui speedTestFinishedWithThroughput:throughput_kbps selectedKbps:selected];
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
    uint32_t min_idle_fps;
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
    s.min_idle_fps = sanitize_min_idle_fps(
        (uint32_t)[defaults integerForKey:kSettingMinIdleFPS]);
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

/// Global settings overlaid with what the user chose for this tablet.
/// Command-line overrides (-DroidScreenFPS 60, ...) still win, for tests.
/// 120 fps only where the encoder keeps up. Measured on Apple silicon: a
/// 2560x1600 frame takes ~11 ms in VideoToolbox (> the 8.3 ms budget), so
/// ~10% of frames were dropped and motion stuttered; 1920x1200 takes ~7 ms
/// with no drops. Retina doubles the encoded size, so it rules 120 out.
/// An explicit -DroidScreenFPS on the command line is left alone (tests).
static void clamp_high_fps(StreamSettings& s) {
    if (s.fps <= 60) return;
    NSDictionary* args = [[NSUserDefaults standardUserDefaults] volatileDomainForName:NSArgumentDomain];
    if (args[kSettingFPS] != nil) return;
    const uint64_t pixels = (uint64_t)s.width * s.height * (s.hidpi ? 4 : 1);
    if (s.hidpi || pixels > 1920ull * 1200ull) {
        NSLog(@"[Stream] %u fps needs Retina off and <= 1920x1200 (encoder can't keep up); using 60 fps",
              s.fps);
        s.fps = 60;
    }
}

- (StreamSettings)settingsForDeviceKey:(NSString*)key name:(NSString*)name {
    StreamSettings s = [self currentSettings];
    if (!key.length) { clamp_high_fps(s); return s; }

    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary* all = [[defaults dictionaryForKey:kSettingDeviceSettings] mutableCopy]
        ?: [NSMutableDictionary dictionary];
    NSMutableDictionary* dev = [all[key] mutableCopy] ?: [NSMutableDictionary dictionary];

    // Remember the name so the settings window can list it while offline.
    if (name.length && ![dev[@"name"] isEqual:name]) {
        dev[@"name"] = name;
        all[key] = dev;
        [defaults setObject:all forKey:kSettingDeviceSettings];
    }

    NSDictionary* args = [defaults volatileDomainForName:NSArgumentDomain];
    auto pick = ^BOOL(NSString* devKey, NSString* globalKey) {
        return dev[devKey] != nil && args[globalKey] == nil;
    };
    if (pick(@"fps", kSettingFPS)) s.fps = [dev[@"fps"] unsignedIntValue];
    if (pick(@"min_idle_fps", kSettingMinIdleFPS))
        s.min_idle_fps = sanitize_min_idle_fps([dev[@"min_idle_fps"] unsignedIntValue]);
    if (pick(@"bitrate", kSettingBitrate)) s.bitrate_kbps = [dev[@"bitrate"] unsignedIntValue];
    if (pick(@"resolution", kSettingResolution)) {
        NSInteger idx = [dev[@"resolution"] integerValue];
        if (idx >= 0 && idx < kResolutionCount) {
            s.width = kResolutions[idx].width;
            s.height = kResolutions[idx].height;
        }
    }
    if (pick(@"hidpi", kSettingScale)) s.hidpi = [dev[@"hidpi"] boolValue];
    if (s.fps != 30 && s.fps != 60 && s.fps != 120) s.fps = 60;
    if (s.bitrate_kbps < 1000) s.bitrate_kbps = 15000;
    clamp_high_fps(s);
    return s;
}

// -----------------------------------------------------------------------------
#pragma mark - Validation Scene
// -----------------------------------------------------------------------------

- (BOOL)validationSceneEnabled {
    const char* value = getenv("DROIDSCREEN_VALIDATION_SCENE");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

- (void)showValidationSceneOnDisplay:(CGDirectDisplayID)displayID {
    if (![self validationSceneEnabled]) return;

    if (!NSThread.isMainThread) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            [self showValidationSceneOnDisplay:displayID];
        });
        return;
    }

    NSScreen* targetScreen = nil;
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* screenID = screen.deviceDescription[@"NSScreenNumber"];
        if (screenID && screenID.unsignedIntValue == displayID) {
            targetScreen = screen;
            break;
        }
    }

    if (!targetScreen) {
        NSLog(@"[Validation] Virtual display screen %u not visible to AppKit; skipping scene",
              displayID);
        fprintf(stderr, "[validation] screen %u not visible to AppKit\n", displayID);
        fflush(stderr);
        return;
    }

    if (_validationTask) {
        if (_validationTask.isRunning) {
            [_validationTask terminate];
            [_validationTask waitUntilExit];
        }
        _validationTask = nil;
    }

    NSDictionary<NSString*, NSString*>* processEnv =
        NSProcessInfo.processInfo.environment;
    NSString* executablePath = processEnv[@"DROIDSCREEN_VALIDATION_HELPER_PATH"];
    BOOL useExternalHelper = executablePath.length > 0;
    if (!useExternalHelper) {
        executablePath = NSBundle.mainBundle.executablePath;
    }
    if (executablePath.length == 0) {
        fprintf(stderr, "[validation] missing executable path\n");
        fflush(stderr);
        return;
    }

    _validationTask = [[NSTask alloc] init];
    _validationTask.executableURL = [NSURL fileURLWithPath:executablePath];
    NSString* displayArg = [NSString stringWithFormat:@"%u", displayID];
    NSString* parentArg = [NSString stringWithFormat:@"%d", getpid()];
    _validationTask.arguments = useExternalHelper
        ? @[displayArg, parentArg]
        : @[@"--validation-scene-helper", displayArg];

    NSMutableDictionary<NSString*, NSString*>* env =
        [processEnv mutableCopy];
    env[@"DROIDSCREEN_VALIDATION_HELPER"] = @"1";
    _validationTask.environment = env;

    NSError* taskError = nil;
    if (![_validationTask launchAndReturnError:&taskError]) {
        fprintf(stderr, "[validation] helper launch failed: %s\n",
                taskError.localizedDescription.UTF8String ?: "unknown");
        fflush(stderr);
        _validationTask = nil;
        return;
    }

    fprintf(stderr, "[validation] helper launched pid=%d display=%u external=%d\n",
            (int)_validationTask.processIdentifier, displayID,
            useExternalHelper ? 1 : 0);
    fflush(stderr);
}

- (void)stopValidationScene {
    if (!NSThread.isMainThread) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            [self stopValidationScene];
        });
        return;
    }

    if (_validationWindow) {
        [_validationWindow close];
        _validationWindow = nil;
        NSLog(@"[Validation] Scene stopped");
        fprintf(stderr, "[validation] scene stopped\n");
        fflush(stderr);
    }
    if (_validationTask) {
        pid_t pid = _validationTask.processIdentifier;
        if (_validationTask.isRunning) {
            [_validationTask terminate];
            [_validationTask waitUntilExit];
        }
        _validationTask = nil;
        fprintf(stderr, "[validation] helper stopped pid=%d\n", (int)pid);
        fflush(stderr);
    }
}

// -----------------------------------------------------------------------------
#pragma mark - Menu Actions
// -----------------------------------------------------------------------------

- (void)showSettings:(id)sender {
    [_ui showSettings];
}

- (void)toggleConnect:(id)sender {
    NSLog(@"[DEBUG] toggleConnect: isStreaming=%d", _isStreaming);
    if (_isStreaming) {
        // User manually disconnected — suppress auto-connect until device is
        // re-plugged or user clicks Connect again.
        _userDisconnected = YES;
        [self updateConnectMenuTitle:@"Disconnecting..."];
        dispatch_async(_streamQueue, ^{
            [self disconnectSync];
            [self updateConnectMenuTitle:@""];
        });
    } else {
        // User manually connecting — clear the suppression flag.
        _userDisconnected = NO;
        [self updateConnectMenuTitle:@"Connecting..."];
        dispatch_async(_streamQueue, ^{
            [self connectSync];
            [self updateConnectMenuTitle:@""];
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

        NSLog(@"[Stream] Connecting: %ux%u@%ufps, min_idle=%ufps, %u kbps, port %u, hidpi=%s",
              settings.width, settings.height, settings.fps,
              settings.min_idle_fps,
              settings.bitrate_kbps, settings.port,
              settings.hidpi ? "YES" : "NO");

        // 1. Pick the tablet and open the connection (adb tunnel or Wi-Fi).
        //    Done before creating the virtual display so a Wi-Fi approval
        //    prompt on the tablet doesn't leave an empty display around.
        StreamTarget target;
        NSString* targetError = nil;
        if (![self chooseTarget:&target error:&targetError]) {
            NSLog(@"[Stream] %@", targetError);
            [self updateStatusText:[@"Status: " stringByAppendingString:targetError]];
            [self updateConnectMenuTitle:@"Connect"];
            _isBusy = NO;
            return;
        }
        NSString* openError = nil;
        if (![self openTransport:target port:settings.port client:_client error:&openError]) {
            NSLog(@"[Stream] %@", openError);
            [self updateStatusText:[@"Status: " stringByAppendingString:openError]];
            [self updateConnectMenuTitle:@"Connect"];
            _client.reset();
            _isBusy = NO;
            return;
        }
        _streamingOverWifi = target.wifi;
        _activeWifiTabletId = target.wifi ? target.tablet.id : std::string();
        {
            NSString* name = target.wifi
                ? [NSString stringWithUTF8String:target.tablet.service_name.c_str()]
                : adb_device_name();
            NSString* serial = target.wifi ? nil : adb_device_serial();
            NSString* deviceKey = target.wifi
                ? [NSString stringWithUTF8String:target.tablet.id.c_str()]
                : (serial ? [@"usb:" stringByAppendingString:serial] : nil);
            NSString* address = target.wifi ? peer_address((int)_client->fd()) : nil;

            // Per-tablet settings (port stays global: it was needed to connect).
            uint16_t port = settings.port;
            settings = [self settingsForDeviceKey:deviceKey name:name];
            settings.port = port;
            NSLog(@"[Stream] Settings for %@ (%@): %ux%u@%ufps, min_idle=%ufps, %u kbps, hidpi=%s",
                  name, deviceKey ?: @"no key", settings.width, settings.height, settings.fps,
                  settings.min_idle_fps, settings.bitrate_kbps, settings.hidpi ? "YES" : "NO");

            dispatch_async(dispatch_get_main_queue(), ^{
                self->_activeTabletName = name;
                self->_activeAddress = address;
                self->_activeDeviceKey = deviceKey;
                [self pushUIState];
            });
        }

        // 1b. The tablet's native resolution (landscape) caps the capture
        //     resolution so we never stream more pixels than it can display.
        //     Wi-Fi tablets publish it in their Bonjour record.
        CGSize deviceRes = target.wifi
            ? CGSizeMake(target.tablet.width, target.tablet.height)
            : adb_device_resolution();
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
            [self closeTransportForPort:settings.port];
            _virtualDisplay.reset();
            _isBusy = NO;
            return;
        }

        NSLog(@"[Stream] Virtual display created (ID=%u, hidpi=%s)",
              _virtualDisplay->display_id(),
              settings.hidpi ? "YES" : "NO");

        // Give macOS a moment to register the new display.
        std::this_thread::sleep_for(std::chrono::seconds(2));
        [self showValidationSceneOnDisplay:_virtualDisplay->display_id()];
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

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
            [self stopValidationScene];
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
            _capturer.reset();
            [self closeTransportForPort:settings.port];
            _isBusy = NO;
            return;
        }

        _streamWidth  = _capturer->width();
        _streamHeight = _capturer->height();
        _streamFPS    = settings.fps;
        _activeHiDPI  = settings.hidpi;
        _activeBitrateKbps = settings.bitrate_kbps;

        NSLog(@"[Stream] Capture resolution: %ux%u", _streamWidth, _streamHeight);

        // 4. TCP connection was opened in step 1.

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
            // Forced: echo the Mac's real value even if it did not change
            // (clamped, or a device without volume control).
            dispatch_async(strongSelf->_deckStateQueue, ^{
                [strongSelf pushDeckVolumeStateForce:YES];
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC),
                           strongSelf->_deckStateQueue, ^{
                [strongSelf pushDeckVolumeStateForce:YES];
            });
        });

        _pipeline = std::make_unique<droidscreen::Pipeline>(
            _capturer.get(), _encoder.get(), _client.get(), _touch.get(), _mouse.get(),
            _deckManager.get());

        if (!_pipeline->start(_streamWidth, _streamHeight,
                              settings.fps, settings.bitrate_kbps,
                              settings.min_idle_fps, accessibilityGranted,
                              DS_CODEC_HEVC,
                              DS_CODEC_CAP_H264 | DS_CODEC_CAP_HEVC)) {
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
            [self stopValidationScene];
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
            _capturer.reset();
            [self closeTransportForPort:settings.port];
            _isBusy = NO;
            return;
        }

        // Success.
        _isStreaming = YES;
        {
            NSString* codec = codec_name(_pipeline->accepted_codec());
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_activeCodec = codec;
                [self rebuildWifiMenu];
            });
        }
        NSString* statusStr = [NSString stringWithFormat:@"Status: Streaming %ux%u@%ufps (%@)",
                               _streamWidth, _streamHeight, _streamFPS,
                               _streamingOverWifi ? @"Wi-Fi" : @"USB"];
        [self updateStatusText:statusStr];
        [self updateConnectMenuTitle:@"Disconnect"];

        NSLog(@"[Stream] Pipeline started: %ux%u@%ufps",
              _streamWidth, _streamHeight, _streamFPS);

        // Start stats logging and health monitoring.
        dispatch_async(dispatch_get_main_queue(), ^{
            [self startStatsTimer];
        });

        // Feed volume/media state to the tablet Deck while streaming.
        _lastMediaArtworkTrackKey = nil;
        _forceArtworkPush = YES;
        [self startDeckFeed];

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

        [self stopDeckFeed];
        _lastMediaArtworkTrackKey = nil;
        _forceArtworkPush = YES;

        uint16_t port = (uint16_t)[[NSUserDefaults standardUserDefaults] integerForKey:kSettingPort];

        // The deck-state queue and the stats timer (main thread) use _pipeline
        // without locks, and a deck push can be mid-flight (the Now Playing
        // query is slow) when the connection drops. Detach the pipeline from
        // both before destroying it, or they touch a freed object.
        __block droidscreen::Pipeline* detached = nullptr;
        dispatch_sync(_deckStateQueue, ^{
            detached = self->_pipeline.release();
        });
        if (NSThread.isMainThread) {
            [self stopStatsTimer];
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                [self stopStatsTimer];
            });
        }
        std::unique_ptr<droidscreen::Pipeline> pipeline(detached);
        if (pipeline) {
            pipeline->stop();
            pipeline.reset();
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
        [self stopValidationScene];
        if (_virtualDisplay) {
            _virtualDisplay->destroy();
            _virtualDisplay.reset();
        }

        [self closeTransportForPort:port];

        _isStreaming = NO;
        [self updateStatusText:@"Status: Disconnected"];
        [self updateConnectMenuTitle:@"Connect"];
        dispatch_async(dispatch_get_main_queue(), ^{ [self rebuildWifiMenu]; });

        NSLog(@"[Stream] Disconnected");
    }
}

// -----------------------------------------------------------------------------
#pragma mark - Transport selection (USB / Wi-Fi)
// -----------------------------------------------------------------------------

/// Decide where to stream. Order: tablet picked in the menu, then a USB
/// device, then any Wi-Fi tablet this Mac is already paired with.
- (BOOL)chooseTarget:(StreamTarget*)target error:(NSString**)error {
    if (!_wifiTargetId.empty()) {
        droidscreen::WifiTablet tablet;
        if (_wifiDiscovery && _wifiDiscovery->find(_wifiTargetId, &tablet)) {
            target->wifi = true;
            target->tablet = tablet;
            return YES;
        }
        // The tablet picked in the menu left the network: back to automatic.
        _wifiTargetId.clear();
    }
    if (!g_cliForceWifi && _usbDevicePresent) {
        target->wifi = false;
        return YES;
    }
    if (_wifiDiscovery) {
        for (const auto& tablet : _wifiDiscovery->tablets()) {
            if (wifi_tablet_eligible(tablet)) {
                target->wifi = true;
                target->tablet = tablet;
                return YES;
            }
        }
    }
    *error = @"No tablet found (USB or paired Wi-Fi)";
    return NO;
}

/// Open a protocol-ready connection to the target. Over Wi-Fi this includes
/// the hello exchange, which may wait for the user to approve on the tablet.
- (BOOL)openTransport:(const StreamTarget&)target
                 port:(uint16_t)port
               client:(std::unique_ptr<droidscreen::TCPClient>&)client
                error:(NSString**)error {
    client = std::make_unique<droidscreen::TCPClient>();

    if (!target.wifi) {
        [self updateStatusText:@"Status: Setting up ADB..."];
        if (!adb_forward_setup(port)) {
            *error = @"ADB forward failed";
            return NO;
        }
        [self updateStatusText:@"Status: Connecting to device..."];
        if (!client->connect(port)) {
            adb_forward_remove(port);
            *error = @"Connection failed (is Android app running?)";
            return NO;
        }
        return YES;
    }

    const auto& tablet = target.tablet;
    if (tablet.protocol_version != DS_PROTOCOL_VERSION) {
        *error = [NSString stringWithFormat:@"Update DroidScreen on %s (protocol v%u, need v%u)",
                  tablet.service_name.c_str(), tablet.protocol_version, DS_PROTOCOL_VERSION];
        return NO;
    }
    [self updateStatusText:[NSString stringWithFormat:@"Status: Connecting to %s over Wi-Fi...",
                            tablet.service_name.c_str()]];
    if (!client->connect(tablet.host.c_str(), tablet.port, 3000)) {
        *error = [NSString stringWithFormat:@"Could not reach %s over Wi-Fi",
                  tablet.service_name.c_str()];
        return NO;
    }

    uint8_t my_id[DS_HELLO_ID_SIZE];
    desktop_id(my_id);
    uint8_t token[DS_HELLO_TOKEN_SIZE] = {0};
    NSString* stored = stored_pairing_token(tablet.id);
    bool paired = stored && bytes_from_hex(stored, token, DS_HELLO_TOKEN_SIZE);
    if (!paired) {
        [self updateStatusText:[NSString stringWithFormat:@"Status: Tap \"Allow\" on %s...",
                                tablet.service_name.c_str()]];
    }

    auto result = droidscreen::perform_hello(client.get(), my_id, token,
                                             desktop_display_name().UTF8String);
    if (result == droidscreen::HelloResult::Rejected && paired) {
        // The tablet no longer trusts us (user denied, or it forgot the
        // pairing). Drop the stale token so auto-connect stops retrying.
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        NSMutableDictionary* pairings =
            [([defaults dictionaryForKey:kSettingWifiPairings] ?: @{}) mutableCopy];
        [pairings removeObjectForKey:[NSString stringWithUTF8String:tablet.id.c_str()]];
        [defaults setObject:pairings forKey:kSettingWifiPairings];
    }
    if (result != droidscreen::HelloResult::Accepted) {
        client->close();
        *error = result == droidscreen::HelloResult::Rejected
            ? [NSString stringWithFormat:@"%s did not allow this Mac", tablet.service_name.c_str()]
            : [NSString stringWithFormat:@"Wi-Fi pairing with %s failed", tablet.service_name.c_str()];
        return NO;
    }
    store_pairing_token(tablet.id, hex_from_bytes(token, DS_HELLO_TOKEN_SIZE));
    return YES;
}

- (void)closeTransportForPort:(uint16_t)port {
    if (_client) {
        _client->close();
        _client.reset();
    }
    if (!_streamingOverWifi) {
        adb_forward_remove(port);
    }
    _streamingOverWifi = NO;
    _activeWifiTabletId.clear();
}

// -----------------------------------------------------------------------------
#pragma mark - Wi-Fi discovery and menu
// -----------------------------------------------------------------------------

- (void)startWifiDiscovery {
    _wifiDiscovery = std::make_unique<droidscreen::WifiDiscovery>();
    __weak AppDelegate* weakSelf = self;
    if (!_wifiDiscovery->start([weakSelf]() {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf rebuildWifiMenu];
            [strongSelf wifiTabletsChanged];
        })) {
        NSLog(@"[Wi-Fi] Bonjour browsing unavailable");
    }
}

/// Runs on the main queue whenever tablets appear or disappear.
- (void)wifiTabletsChanged {
    if (!_wifiDiscovery) return;
    bool any_paired = false;
    for (const auto& tablet : _wifiDiscovery->tablets()) {
        if (stored_pairing_token(tablet.id)) any_paired = true;
    }
    // The paired tablet left the network (app closed, tablet asleep): like
    // unplugging the cable, re-arm auto-connect for when it comes back.
    if (!any_paired && !_isStreaming) {
        _userDisconnected = NO;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [self autoConnectWifiIfPaired];
    });
}

/// Auto-connect to a paired Wi-Fi tablet when nothing is streaming and no
/// USB device is attached (USB keeps priority). Call off the main thread.
- (void)autoConnectWifiIfPaired {
    if (_isStreaming || _isBusy || _userDisconnected || !_wifiDiscovery) return;
    if (!g_cliForceWifi && _usbDevicePresent) return;
    bool candidate = false;
    for (const auto& tablet : _wifiDiscovery->tablets()) {
        if (wifi_tablet_eligible(tablet)) candidate = true;
    }
    if (!candidate) return;
    NSLog(@"[AutoConnect] Paired Wi-Fi tablet available, connecting...");
    dispatch_async(_streamQueue, ^{
        if (!self->_isStreaming) {
            [self connectSync];
        }
    });
}

// -----------------------------------------------------------------------------
#pragma mark - Deck feed (volume/media state push to Android)
// -----------------------------------------------------------------------------

static const AudioObjectPropertyAddress kDefaultOutputAddress = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress kOutputVolumeAddress = {
    kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
    kAudioObjectPropertyScopeOutput,
    kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress kOutputMuteAddress = {
    kAudioDevicePropertyMute,
    kAudioObjectPropertyScopeOutput,
    kAudioObjectPropertyElementMain
};

/// Sends volume/mute to the tablet when they differ from what it was last
/// sent (or always, with force). Deck queue.
- (void)pushDeckVolumeStateForce:(BOOL)force {
    droidscreen::Pipeline* pl = _pipeline.get();
    if (!pl || !pl->is_running()) return;

    float vol = getSystemVolume();
    bool muted = getSystemMuted();
    uint16_t level = (uint16_t)(vol * 65535);
    if (!force && (int32_t)level == _lastSentVolumeLevel &&
        (muted ? 1 : 0) == _lastSentVolumeMuted) {
        return;
    }
    _lastSentVolumeLevel = level;
    _lastSentVolumeMuted = muted ? 1 : 0;
    pl->send_volume_state(level, muted);
}

/// Moves the volume/mute listeners to the current default output device. Deck queue.
- (void)bindOutputVolumeListener {
    if (_observedOutputDevice != kAudioObjectUnknown) {
        AudioObjectRemovePropertyListenerBlock(_observedOutputDevice, &kOutputVolumeAddress,
                                               _deckStateQueue, _outputVolumeListener);
        AudioObjectRemovePropertyListenerBlock(_observedOutputDevice, &kOutputMuteAddress,
                                               _deckStateQueue, _outputVolumeListener);
        _observedOutputDevice = kAudioObjectUnknown;
    }
    AudioDeviceID device = getDefaultOutputDevice();
    if (device == kAudioObjectUnknown) return;
    _observedOutputDevice = device;
    // Not every device has volume or mute (e.g. some HDMI outputs).
    if (AudioObjectHasProperty(device, &kOutputVolumeAddress)) {
        AudioObjectAddPropertyListenerBlock(device, &kOutputVolumeAddress,
                                            _deckStateQueue, _outputVolumeListener);
    }
    if (AudioObjectHasProperty(device, &kOutputMuteAddress)) {
        AudioObjectAddPropertyListenerBlock(device, &kOutputMuteAddress,
                                            _deckStateQueue, _outputVolumeListener);
    }
}

- (void)startVolumeObserver {
    __weak AppDelegate* weakSelf = self;
    _outputVolumeListener = ^(UInt32, const AudioObjectPropertyAddress*) {
        [weakSelf pushDeckVolumeStateForce:NO];
    };
    _defaultOutputListener = ^(UInt32, const AudioObjectPropertyAddress*) {
        AppDelegate* strongSelf = weakSelf;
        [strongSelf bindOutputVolumeListener];
        [strongSelf pushDeckVolumeStateForce:NO];
    };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &kDefaultOutputAddress,
                                        _deckStateQueue, _defaultOutputListener);
    _observedOutputDevice = kAudioObjectUnknown;
    [self bindOutputVolumeListener];
}

- (void)stopVolumeObserver {
    if (!_defaultOutputListener) return;
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &kDefaultOutputAddress,
                                           _deckStateQueue, _defaultOutputListener);
    if (_observedOutputDevice != kAudioObjectUnknown) {
        AudioObjectRemovePropertyListenerBlock(_observedOutputDevice, &kOutputVolumeAddress,
                                               _deckStateQueue, _outputVolumeListener);
        AudioObjectRemovePropertyListenerBlock(_observedOutputDevice, &kOutputMuteAddress,
                                               _deckStateQueue, _outputVolumeListener);
        _observedOutputDevice = kAudioObjectUnknown;
    }
    _defaultOutputListener = nil;
    _outputVolumeListener = nil;
}

/// Starts the persistent MediaRemote helper (perl + MediaRemoteMini.dylib,
/// adapter_stream): it writes one JSON line per Now Playing change. Deck queue.
- (void)startNowPlayingHelper {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* scriptPath = [bundle pathForResource:@"mediaremote-mini" ofType:@"pl"];
    NSString* dylibPath = [bundle pathForResource:@"MediaRemoteMini" ofType:@"dylib"];
    if (scriptPath.length == 0 || dylibPath.length == 0) {
        NSLog(@"[deck] MediaRemote helper resources are missing");
        return;
    }

    __weak AppDelegate* weakSelf = self;
    _nowPlayingBuffer = [NSMutableData data];
    _nowPlayingHelper = [DSChildProcess spawn:@"/usr/bin/perl"
                                         args:@[scriptPath, dylibPath, @"adapter_stream"]
                                    withStdin:YES
                                  stderrToLog:YES
                                        queue:_deckStateQueue
                                       onData:^(NSData* data) {
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf->_nowPlayingBuffer appendData:data];
        drain_lines(strongSelf->_nowPlayingBuffer, ^(NSData* line) {
            [strongSelf handleNowPlayingLine:line];
        });
    }
                                       onExit:^(int status) {
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_deckActive) return;
        NSLog(@"[deck] MediaRemote helper exited (status %d), restarting in 2 s", status);
        strongSelf->_nowPlayingHelper = nil;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       strongSelf->_deckStateQueue, ^{
            AppDelegate* again = weakSelf;
            if (again && again->_deckActive && !again->_nowPlayingHelper) {
                [again startNowPlayingHelper];
            }
        });
    }];
    if (!_nowPlayingHelper) {
        NSLog(@"[deck] Failed to start MediaRemote helper");
        return;
    }
    // A (re)started helper begins at generation 0: hand it the current one so
    // its snapshots are not dropped as stale. This also asks for a snapshot.
    [_nowPlayingHelper writeLine:[NSString stringWithFormat:@"%llu",
                                  (unsigned long long)_mediaActionGen.load()]];
    NSLog(@"[deck] MediaRemote helper started");
}

/// One JSON line from the helper: the current Now Playing snapshot. Deck queue.
- (void)handleNowPlayingLine:(NSData*)line {
    NSError* error = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:line options:0 error:&error];
    if (![obj isKindOfClass:[NSDictionary class]]) {
        NSLog(@"[deck] Failed to parse helper JSON: %@", error);
        return;
    }
    NSMutableDictionary* snapshot = [(NSDictionary*)obj mutableCopy];
    // A snapshot taken before a tablet media action reflects the old state
    // (e.g. still playing) and would undo the tablet's toggle.
    uint64_t gen = [snapshot[@"gen"] unsignedLongLongValue];
    [snapshot removeObjectForKey:@"gen"];
    if (gen < _mediaActionGen.load()) {
        fprintf(stderr, "[deck] Dropping media snapshot taken before a media action\n");
        return;
    }
    _nowPlayingSnapshot = snapshot;
    _nowPlayingSnapshotAt = CFAbsoluteTimeGetCurrent();
    [self pushDeckMediaState];
}

/// Sends the latest Now Playing snapshot to the tablet, with the elapsed time
/// advanced to now, when it differs from what was sent last. Deck queue.
- (void)pushDeckMediaState {
    droidscreen::Pipeline* pl = _pipeline.get();
    if (!pl || !pl->is_running()) return;

    NSDictionary* snapshot = _nowPlayingSnapshot;
    if ([snapshot[@"playing"] boolValue]) {
        NSNumber* elapsedBase = snapshot[@"elapsedTimeNow"] ?: snapshot[@"elapsedTime"];
        if ([elapsedBase isKindOfClass:[NSNumber class]]) {
            NSNumber* rate = snapshot[@"playbackRate"];
            double speed = [rate isKindOfClass:[NSNumber class]] && rate.doubleValue > 0.0
                ? rate.doubleValue : 1.0;
            double elapsed = elapsedBase.doubleValue +
                (CFAbsoluteTimeGetCurrent() - _nowPlayingSnapshotAt) * speed;
            NSNumber* duration = snapshot[@"duration"];
            if ([duration isKindOfClass:[NSNumber class]] && duration.doubleValue > 0.0) {
                elapsed = MIN(elapsed, duration.doubleValue);
            }
            NSMutableDictionary* advanced = [snapshot mutableCopy];
            advanced[@"elapsedTimeNow"] = @(MAX(0.0, elapsed));
            snapshot = advanced;
        }
    }

    NSString* trackKey = mediaTrackKeyForSnapshot(snapshot);
    // The tablet drops its artwork whenever the track changes, so forget what
    // was delivered on every change (A -> artless B -> A must resend A's art).
    // An empty snapshot (nothing playing) also clears the tablet's art, so
    // it counts as a change too.
    NSString* observedKey = trackKey.length > 0 ? trackKey : @"";
    BOOL trackChanged = NO;
    if (!_lastObservedTrackKey || ![_lastObservedTrackKey isEqualToString:observedKey]) {
        _lastObservedTrackKey = [observedKey copy];
        _lastMediaArtworkTrackKey = nil;
        trackChanged = YES;
    }
    bool includeArtwork = _forceArtworkPush;
    if (!includeArtwork && trackKey.length > 0 &&
        ![_lastMediaArtworkTrackKey isEqualToString:trackKey]) {
        includeArtwork = true;
    }

    std::string json = buildMediaStateJSON(snapshot, includeArtwork);
    // Only treat the track's artwork as delivered once it actually went out.
    // MediaRemote often reports a new track before its artwork; marking it
    // sent then suppressed the artwork until the next forced push.
    if (includeArtwork && trackKey.length > 0 &&
        json.find("\"album_art_b64\":") != std::string::npos) {
        _lastMediaArtworkTrackKey = [trackKey copy];
        _forceArtworkPush = NO;
    }

    if (json == _lastSentMediaJSON) return;
    _lastSentMediaJSON = json;

    BOOL playing = [snapshot[@"playing"] boolValue];
    if (trackChanged || includeArtwork || playing != _lastLoggedPlaying) {
        _lastLoggedPlaying = playing;
        fprintf(stderr, "[deck] Sending media state: %.80s...\n", json.c_str());
    }
    pl->send_media_state(json);
}

- (void)scheduleDeckMediaRefreshBurst {
    if (!_deckStateQueue) return;
    const uint64_t gen = _mediaActionGen.fetch_add(1) + 1;
    _forceArtworkPush = YES;
    // The helper answers each request with a fresh snapshot stamped with gen;
    // the later ones catch the player settling after the media key.
    NSString* request = [NSString stringWithFormat:@"%llu", (unsigned long long)gen];
    for (int64_t delayMs : {0, 150, 400}) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayMs * (int64_t)NSEC_PER_MSEC),
                       _deckStateQueue, ^{
            [self->_nowPlayingHelper writeLine:request];
        });
    }
}

/// Called from the stream queue once the pipeline runs.
- (void)startDeckFeed {
    [self stopDeckFeed];
    dispatch_async(_deckStateQueue, ^{
        self->_deckActive = YES;
        self->_lastSentVolumeLevel = -1;
        self->_lastSentVolumeMuted = -1;
        self->_lastSentMediaJSON.clear();
        self->_lastLoggedPlaying = NO;
        self->_nowPlayingSnapshot = nil;

        [self startVolumeObserver];
        [self pushDeckVolumeStateForce:YES];
        [self startNowPlayingHelper];

        // The tablet shows elapsed/total but does not advance it: tick the
        // progress while playing (no process spawn; paused sends nothing).
        self->_deckProgressTimer =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->_deckStateQueue);
        dispatch_source_set_timer(self->_deckProgressTimer,
                                  dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                                  NSEC_PER_SEC, 250 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(self->_deckProgressTimer, ^{
            if ([self->_nowPlayingSnapshot[@"playing"] boolValue]) {
                [self pushDeckMediaState];
            }
        });
        dispatch_resume(self->_deckProgressTimer);
        NSLog(@"[deck] Started deck feed");
    });
}

/// Never call it on the deck queue.
- (void)stopDeckFeed {
    dispatch_sync(_deckStateQueue, ^{
        if (!self->_deckActive) return;
        self->_deckActive = NO;
        if (self->_deckProgressTimer) {
            dispatch_source_cancel(self->_deckProgressTimer);
            self->_deckProgressTimer = nil;
        }
        [self stopVolumeObserver];
        [self->_nowPlayingHelper terminate];
        self->_nowPlayingHelper = nil;
        self->_nowPlayingSnapshot = nil;
        NSLog(@"[deck] Stopped deck feed");
    });
}

// -----------------------------------------------------------------------------
#pragma mark - Stats Timer (health monitoring on main thread)
// -----------------------------------------------------------------------------

- (void)startStatsTimer {
    [self stopStatsTimer];
    _lastStatsCaptured = 0;
    _lastStatsEncoded = 0;
    _lastStatsDropped = 0;
    _lastStatsBytes = 0;
    _lastStatsTime = CFAbsoluteTimeGetCurrent();
    _statsTick = 0;
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
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
        uint64_t cap = pl->frames_captured();
        uint64_t enc = pl->frames_encoded();
        uint64_t drop = pl->frames_dropped();
        uint64_t byt = pl->bytes_sent();
        int64_t rtt = pl->last_rtt_us();
        uint64_t encSum = pl->encode_us_sum();
        uint64_t encN = pl->encode_samples();
        double encMs = (encN > _lastStatsEncN)
            ? (double)(encSum - _lastStatsEncSum) / (double)(encN - _lastStatsEncN) / 1000.0
            : 0.0;
        _lastStatsEncSum = encSum;
        _lastStatsEncN = encN;
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        double elapsed = now - _lastStatsTime;
        double capFps = elapsed > 0 ? (cap - _lastStatsCaptured) / elapsed : 0.0;
        double encFps = elapsed > 0 ? (enc - _lastStatsEncoded) / elapsed : 0.0;
        double mbps = elapsed > 0 ? ((byt - _lastStatsBytes) * 8.0 / elapsed / 1000000.0) : 0.0;

        // Interface: estimated Mac-to-tablet latency = capture-to-send on
        // this side plus half the measured round trip.
        double rttMs = rtt > 0 ? rtt / 1000.0 : 0.0;
        double c2sMs = pl->last_capture_to_send_us() > 0 ? pl->last_capture_to_send_us() / 1000.0 : encMs;
        [_ui updateStats:@{
            @"latencyMs": @(c2sMs + rttMs / 2.0),
            @"fps":       @(encFps),
            @"targetFps": @(_streamFPS),
            @"mbps":      @(mbps),
            // The cap this stream was started with (per-tablet setting), not the global default.
            @"capMbps":   @(_activeBitrateKbps / 1000.0),
        }];

        if ((_statsTick++ % 2) == 0)
        NSLog(@"[Stats] captured=%llu encoded=%llu dropped=%llu bytes=%llu rtt=%lld us cap_fps=%.1f enc_fps=%.1f mbps=%.1f cap_q=%zu send_q=%zu enc_ms=%.2f",
              (unsigned long long)cap,
              (unsigned long long)enc,
              (unsigned long long)drop,
              (unsigned long long)byt,
              (long long)rtt,
              capFps,
              encFps,
              mbps,
              pl->capture_queue_depth(),
              pl->send_queue_depth(),
              encMs);

        _lastStatsCaptured = cap;
        _lastStatsEncoded = enc;
        _lastStatsDropped = drop;
        _lastStatsBytes = byt;
        _lastStatsTime = now;
    } @catch (NSException* e) {
        NSLog(@"[Stats] Exception in statsTimerFired: %@", e);
    }
}

// -----------------------------------------------------------------------------
#pragma mark - USB Device Detection (adb track-devices)
// -----------------------------------------------------------------------------

- (void)startDeviceDetection {
    _adbTrackQueue = dispatch_queue_create("com.droidscreen.adb-track", DISPATCH_QUEUE_SERIAL);
    if (!g_cliForceWifi) {
        dispatch_async(_adbTrackQueue, ^{
            self->_adbTrackActive = YES;
            [self startAdbTracker];
        });
    }
    // Idle-only retry: the Android app being opened on a plugged-in tablet
    // and a failed Wi-Fi auto-connect produce no event to react to.
    _autoConnectTimer = [NSTimer scheduledTimerWithTimeInterval:3.0
                                                         target:self
                                                       selector:@selector(autoConnectTimerFired:)
                                                       userInfo:nil
                                                        repeats:YES];
    _autoConnectTimer.tolerance = 1.0;
}

- (void)stopDeviceDetection {
    [_autoConnectTimer invalidate];
    _autoConnectTimer = nil;
    if (!_adbTrackQueue) return;
    dispatch_sync(_adbTrackQueue, ^{
        self->_adbTrackActive = NO;
        [self->_adbTracker terminate];
        self->_adbTracker = nil;
    });
}

/// Starts one `adb track-devices`: adb pushes the full device list on start
/// and on every change. Restarted if it exits (adb server killed or
/// restarted). adb track queue.
- (void)startAdbTracker {
    if (!_adbTrackActive || _adbTracker) return;
    __weak AppDelegate* weakSelf = self;
    auto retryLater = ^(int64_t seconds) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, seconds * (int64_t)NSEC_PER_SEC),
                       self->_adbTrackQueue, ^{
            [weakSelf startAdbTracker];
        });
    };

    NSString* adbPath = adb_find_path();
    if (!adbPath) {
        retryLater(5);
        return;
    }
    _adbTrackBuffer = [NSMutableData data];
    _adbTracker = [DSChildProcess spawn:adbPath
                                   args:@[@"track-devices"]
                              withStdin:NO
                            stderrToLog:NO
                                  queue:_adbTrackQueue
                                 onData:^(NSData* data) {
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf->_adbTrackBuffer appendData:data];
        [strongSelf drainAdbTrackBuffer];
    }
                                 onExit:^(int status) {
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_adbTrackActive) return;
        NSLog(@"[ADB] track-devices exited (status %d), restarting in 1 s", status);
        strongSelf->_adbTracker = nil;
        retryLater(1);
    }];
    if (!_adbTracker) {
        retryLater(5);
        return;
    }
    NSLog(@"[ADB] Tracking devices with %@ track-devices", adbPath);
}

/// track-devices output is a sequence of messages: 4 hex digits with the
/// payload length, then "serial\tstate\n" lines. adb track queue.
- (void)drainAdbTrackBuffer {
    for (;;) {
        if (_adbTrackBuffer.length < 4) return;
        char hex[5] = {0};
        memcpy(hex, _adbTrackBuffer.bytes, 4);
        char* end = nullptr;
        unsigned long length = strtoul(hex, &end, 16);
        if (end != hex + 4) {
            NSLog(@"[ADB] Unexpected track-devices output, restarting it");
            [_adbTracker terminate];
            _adbTracker = nil;
            [self startAdbTracker];
            return;
        }
        if (_adbTrackBuffer.length < 4 + length) return;
        NSString* payload = [[NSString alloc]
            initWithBytes:(const char*)_adbTrackBuffer.bytes + 4
                   length:length
                 encoding:NSUTF8StringEncoding] ?: @"";
        [_adbTrackBuffer replaceBytesInRange:NSMakeRange(0, 4 + length) withBytes:NULL length:0];
        [self handleAdbDeviceList:payload];
    }
}

/// A fresh device list from adb. adb track queue.
- (void)handleAdbDeviceList:(NSString*)list {
    NSString* usbSerial = nil;
    for (NSString* line in [list componentsSeparatedByString:@"\n"]) {
        NSArray<NSString*>* fields = [line componentsSeparatedByString:@"\t"];
        if (fields.count >= 2 && [fields[1] isEqualToString:@"device"] &&
            adb_serial_is_usb(fields[0])) {
            usbSerial = fields[0];
            break;
        }
    }
    BOOL connected = usbSerial != nil;

    if (!(_adbTrackUsbSerial == usbSerial || [_adbTrackUsbSerial isEqualToString:usbSerial])) {
        _adbTrackUsbSerial = usbSerial;
        NSString* name = connected ? adb_device_name() : nil;
        NSLog(@"[ADB] USB device %@", connected ? usbSerial : @"removed");
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_usbDevicePresent = connected;
            self->_usbDeviceKey = usbSerial ? [@"usb:" stringByAppendingString:usbSerial] : nil;
            self->_usbDeviceName = name;
            [self pushUIState];
        });
    }

    if (connected) {
        [self autoConnectUsbIfAppRunning];
        return;
    }
    // Wi-Fi streams don't depend on the cable; only offer Wi-Fi auto-connect.
    if (!_isStreaming) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [self autoConnectWifiIfPaired];
        });
        return;
    }
    if (_streamingOverWifi) return;

    // Device physically removed — reset the user-disconnect flag so
    // auto-connect kicks in when the device is plugged back in.
    _userDisconnected = NO;
    NSLog(@"[AutoConnect] Device disconnected, stopping...");
    dispatch_async(_streamQueue, ^{
        if (self->_isStreaming) {
            [self disconnectSync];
        }
    });
}

/// USB device attached but not streaming: connect once the DroidScreen app
/// runs on it. adb track queue.
- (void)autoConnectUsbIfAppRunning {
    if (!_adbTrackUsbSerial || _isStreaming || _isBusy || _userDisconnected) return;
    if (!adb_droidscreen_running()) return;
    NSLog(@"[AutoConnect] DroidScreen detected on device, connecting...");
    dispatch_async(_streamQueue, ^{
        if (!self->_isStreaming) {
            [self connectSync];
        }
    });
}

- (void)autoConnectTimerFired:(NSTimer*)timer {
    if (_isStreaming || _isBusy) return;
    if (g_cliForceWifi || !_usbDevicePresent) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [self autoConnectWifiIfPaired];
        });
        return;
    }
    dispatch_async(_adbTrackQueue, ^{
        [self autoConnectUsbIfAppRunning];
    });
}

@end

// =============================================================================
#pragma mark - Main Entry Point
// =============================================================================

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc >= 3 && strcmp(argv[1], "--validation-scene-helper") == 0) {
            uint32_t displayID = (uint32_t)strtoul(argv[2], nullptr, 10);
            return run_validation_scene_helper(displayID);
        }
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--wifi") == 0) {
                g_cliForceWifi = true;
                // Optional value; anything starting with '-' is another flag
                // (including NSUserDefaults overrides like -DroidScreenFPS).
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    g_cliWifiMatch = argv[++i];
                }
            }
        }

        // ── Single-instance guard ──────────────────────────────────────────────
        // Si hay otra instancia corriendo, terminarla antes de continuar.
        // La nueva instancia "gana" y la vieja es terminada.
        {
            NSString* const kBundleID = @"com.droidscreen.desktop";
            NSArray<NSRunningApplication*>* others =
                [NSRunningApplication runningApplicationsWithBundleIdentifier:kBundleID];
            BOOL foundOther = NO;
            for (NSRunningApplication* other in others) {
                if (other.processIdentifier == getpid()) continue;
                NSLog(@"[DroidScreen] Terminating existing instance PID %d",
                      (int)other.processIdentifier);
                [other terminate];
                foundOther = YES;
            }
            if (foundOther) {
                usleep(800000); // 800ms para shutdown limpio
                for (NSRunningApplication* other in others) {
                    if (other.processIdentifier == getpid()) continue;
                    if (!other.terminated) {
                        NSLog(@"[DroidScreen] Force-terminating PID %d",
                              (int)other.processIdentifier);
                        [other forceTerminate];
                    }
                }
                usleep(200000); // 200ms post force-kill
            }
        }
        // ── End single-instance guard ──────────────────────────────────────────

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

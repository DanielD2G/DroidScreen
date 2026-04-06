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

#include "sck_capturer.h"
#include "vt_encoder.h"
#include "virtual_display.h"
#include "droidscreen/pipeline.h"
#include "droidscreen/mouse_injector.h"
#include "droidscreen/server.h"
#include "droidscreen/touch_injector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <signal.h>
#include <execinfo.h>

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
#pragma mark - Settings Keys (NSUserDefaults)
// =============================================================================

static NSString* const kSettingFPS          = @"DroidScreenFPS";
static NSString* const kSettingBitrate      = @"DroidScreenBitrate";      // kbps
static NSString* const kSettingResolution   = @"DroidScreenResolution";   // index
static NSString* const kSettingScale        = @"DroidScreenScale";        // index
static NSString* const kSettingPort         = @"DroidScreenPort";

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

// State.
@property (nonatomic, assign) BOOL isStreaming;
@property (nonatomic, assign) BOOL isBusy;  // Prevents concurrent connect/disconnect ops.
@property (nonatomic, assign) BOOL userDisconnected;  // Suppresses auto-connect until device is re-plugged or user clicks Connect.

@end

@implementation AppDelegate {
    // Pipeline components (C++ objects, owned here).
    std::unique_ptr<droidscreen::VirtualDisplay>    _virtualDisplay;
    std::unique_ptr<droidscreen::SCKCapturer>       _capturer;
    std::unique_ptr<droidscreen::VTEncoder>         _encoder;
    std::unique_ptr<droidscreen::TCPClient>         _client;
    std::unique_ptr<droidscreen::NullMouseInjector> _mouse;
    std::unique_ptr<droidscreen::NullTouchInjector> _touch;
    std::unique_ptr<droidscreen::Pipeline>          _pipeline;

    // Background queue for streaming operations.
    dispatch_queue_t _streamQueue;

    // ADB device detection timer.
    NSTimer* _deviceTimer;

    // Stats timer.
    NSTimer* _statsTimer;

    // Current streaming parameters (for status display).
    uint32_t _streamWidth;
    uint32_t _streamHeight;
    uint32_t _streamFPS;
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
    }];

    _streamQueue = dispatch_queue_create("com.droidscreen.stream", DISPATCH_QUEUE_SERIAL);
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
    NSRect frame = NSMakeRect(0, 0, 400, 330);
    NSWindowStyleMask style = NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable;

    self.settingsWindow = [[NSWindow alloc] initWithContentRect:frame
                                                      styleMask:style
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
    self.settingsWindow.title = @"DroidScreen Settings";
    self.settingsWindow.delegate = self;
    // Keep settings window on main display — never let it migrate to virtual display.
    self.settingsWindow.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace
                                          | NSWindowCollectionBehaviorTransient;
    self.settingsWindow.level = NSFloatingWindowLevel;
    [self.settingsWindow setReleasedWhenClosed:NO];
    [self.settingsWindow center];

    // Prevent resizing.
    self.settingsWindow.minSize = frame.size;
    self.settingsWindow.maxSize = frame.size;

    NSView* contentView = self.settingsWindow.contentView;
    CGFloat leftMargin  = 20;
    CGFloat labelWidth  = 110;
    CGFloat controlLeft = leftMargin + labelWidth + 10;
    CGFloat controlWidth = 230;
    CGFloat rowHeight   = 32;
    CGFloat startY      = frame.size.height - 50;

    // --- Row 1: FPS ---
    CGFloat y = startY;
    [self addLabel:@"Frame Rate:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.fpsPopup = [self addPopUpButton:contentView atX:controlLeft y:y width:controlWidth];
    [self.fpsPopup addItemWithTitle:@"30 fps"];
    [self.fpsPopup addItemWithTitle:@"60 fps"];
    NSInteger savedFPS = [[NSUserDefaults standardUserDefaults] integerForKey:kSettingFPS];
    [self.fpsPopup selectItemAtIndex:(savedFPS == 30 ? 0 : 1)];

    // --- Row 2: Bitrate ---
    y -= rowHeight + 8;
    [self addLabel:@"Bitrate:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.bitratePopup = [self addPopUpButton:contentView atX:controlLeft y:y width:controlWidth];
    for (int i = 0; i < kBitrateCount; i++) {
        [self.bitratePopup addItemWithTitle:@(kBitrateLabels[i])];
    }
    NSInteger savedBitrate = [[NSUserDefaults standardUserDefaults] integerForKey:kSettingBitrate];
    int bitrateIdx = 2; // default: 15 Mbps
    for (int i = 0; i < kBitrateCount; i++) {
        if (kBitrates[i] == (int)savedBitrate) { bitrateIdx = i; break; }
    }
    [self.bitratePopup selectItemAtIndex:bitrateIdx];

    // --- Row 3: Resolution ---
    y -= rowHeight + 8;
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
    y -= rowHeight + 8;
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
    y -= rowHeight + 12;
    [self addLabel:@"Port:" toView:contentView atX:leftMargin y:y width:labelWidth];
    self.portField = [[NSTextField alloc] initWithFrame:NSMakeRect(controlLeft, y, 100, 24)];
    self.portField.stringValue = [NSString stringWithFormat:@"%ld",
        (long)[[NSUserDefaults standardUserDefaults] integerForKey:kSettingPort]];
    [self.portField setFont:[NSFont systemFontOfSize:13]];
    [self.portField setBezelStyle:NSTextFieldRoundedBezel];
    [contentView addSubview:self.portField];

    // --- Apply button ---
    y -= rowHeight + 20;
    NSButton* applyButton = [[NSButton alloc] initWithFrame:NSMakeRect(frame.size.width - 100 - 20, y, 100, 32)];
    applyButton.title = @"Apply";
    applyButton.bezelStyle = NSBezelStyleRounded;
    [applyButton setFont:[NSFont systemFontOfSize:13]];
    applyButton.target = self;
    applyButton.action = @selector(applySettings:);
    applyButton.keyEquivalent = @"\r"; // Enter key.
    [contentView addSubview:applyButton];
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
    NSInteger fps = (fpsIdx == 0) ? 30 : 60;
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
            [self connectSync];
        });
    }

    [self.settingsWindow close];
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
                                             capture_w, capture_h)) {
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

        // 5. Create encoder, touch injector, pipeline.
        _encoder = std::make_unique<droidscreen::VTEncoder>();
        _mouse   = std::make_unique<droidscreen::NullMouseInjector>();
        _touch   = std::make_unique<droidscreen::NullTouchInjector>();
        _pipeline = std::make_unique<droidscreen::Pipeline>(
            _capturer.get(), _encoder.get(), _client.get(), _touch.get(), _mouse.get());

        if (!_pipeline->start(_streamWidth, _streamHeight,
                              settings.fps, settings.bitrate_kbps, false)) {
            NSLog(@"[Stream] Pipeline start failed");
            [self updateStatusText:@"Status: Pipeline start failed"];
            [self updateConnectMenuTitle:@"Connect"];
            _pipeline.reset();
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

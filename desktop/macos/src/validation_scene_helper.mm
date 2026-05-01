#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdlib>
#include <unistd.h>

@interface ValidationSceneView : NSView
@property(nonatomic, strong) NSArray<CALayer*>* stripes;
@property(nonatomic, strong) CALayer* marker;
@property(nonatomic, strong) CALayer* pulse;
@property(nonatomic) dispatch_source_t animationTimer;
@property(nonatomic, assign) CFTimeInterval animationStart;
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
        [self startValidationAnimation];
    }
    return self;
}

- (void)dealloc {
    if (_animationTimer) {
        dispatch_source_cancel(_animationTimer);
        _animationTimer = nil;
    }
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
    NSMutableArray<CALayer*>* stripes = [NSMutableArray array];
    for (int i = 0; i < 4; i++) {
        CALayer* stripe = [CALayer layer];
        stripe.frame = CGRectMake(-stripeW, b.size.height * (0.16 + i * 0.18),
                                  stripeW, b.size.height * 0.14);
        stripe.backgroundColor = colors[i % colors.count].CGColor;
        stripe.opacity = 0.88;
        stripe.cornerRadius = 10;
        [root addSublayer:stripe];
        [stripes addObject:stripe];
    }
    self.stripes = stripes;

    CALayer* marker = [CALayer layer];
    marker.frame = CGRectMake(40, b.size.height - 120, 92, 92);
    marker.backgroundColor = NSColor.whiteColor.CGColor;
    marker.cornerRadius = 46;
    [root addSublayer:marker];
    self.marker = marker;

    CALayer* pulse = [CALayer layer];
    pulse.frame = CGRectMake(b.size.width - 132, 44, 84, 84);
    pulse.backgroundColor = [NSColor colorWithCalibratedRed:0.12
                                                      green:0.84
                                                       blue:1.00
                                                      alpha:1.0].CGColor;
    pulse.cornerRadius = 6;
    [root addSublayer:pulse];
    self.pulse = pulse;

    CATextLayer* text = [CATextLayer layer];
    text.frame = CGRectMake(48, 42, b.size.width - 96, 72);
    text.contentsScale = NSScreen.mainScreen.backingScaleFactor;
    text.string = @"DroidScreen latency validation";
    text.foregroundColor = NSColor.whiteColor.CGColor;
    text.fontSize = 44;
    text.alignmentMode = kCAAlignmentLeft;
    [root addSublayer:text];
}

- (void)startValidationAnimation {
    self.animationStart = CACurrentMediaTime();
    dispatch_queue_t queue = dispatch_get_main_queue();
    self.animationTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
                                                 0, 0, queue);
    uint64_t interval = NSEC_PER_SEC / 120;
    dispatch_source_set_timer(self.animationTimer,
                              dispatch_time(DISPATCH_TIME_NOW, interval),
                              interval,
                              NSEC_PER_MSEC);

    __weak ValidationSceneView* weakSelf = self;
    dispatch_source_set_event_handler(self.animationTimer, ^{
        [weakSelf tickValidationAnimation];
    });
    dispatch_resume(self.animationTimer);
}

- (void)tickValidationAnimation {
    NSRect b = self.bounds;
    if (b.size.width <= 0 || b.size.height <= 0) return;

    CFTimeInterval t = CACurrentMediaTime() - self.animationStart;
    CGFloat stripeW = MAX(44.0, b.size.width / 18.0);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    for (NSUInteger i = 0; i < self.stripes.count; i++) {
        CALayer* stripe = self.stripes[i];
        CGFloat duration = 0.62 + (CGFloat)i * 0.05;
        CGFloat phase = fmod((CGFloat)t / duration + (CGFloat)i * 0.19, 1.0);
        CGFloat x = -stripeW + phase * (b.size.width + stripeW * 2.0);
        CGRect f = stripe.frame;
        f.origin.x = x - stripeW * 0.5;
        stripe.frame = f;
    }

    CGFloat markerSpan = MAX(1.0, b.size.width - 140.0);
    CGFloat markerPhase = fmod((CGFloat)t / 0.92, 2.0);
    if (markerPhase > 1.0) markerPhase = 2.0 - markerPhase;
    CGPoint markerPos = self.marker.position;
    markerPos.x = 70.0 + markerPhase * markerSpan;
    self.marker.position = markerPos;

    CGFloat pulsePhase = fmod((CGFloat)t * 7.0, 1.0);
    self.pulse.backgroundColor =
        [NSColor colorWithCalibratedRed:0.12 + 0.70 * pulsePhase
                                  green:0.84 - 0.42 * pulsePhase
                                   blue:1.00 - 0.72 * pulsePhase
                                  alpha:1.0].CGColor;

    [CATransaction commit];
}

@end

@interface HelperDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, assign) pid_t parentPid;
@end

@implementation HelperDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [NSTimer scheduledTimerWithTimeInterval:0.5
                                     target:self
                                   selector:@selector(checkParent:)
                                   userInfo:nil
                                    repeats:YES];
}

- (void)checkParent:(NSTimer*)timer {
    (void)timer;
    if (self.parentPid > 0 && kill(self.parentPid, 0) != 0) {
        [NSApp terminate:nil];
    }
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: droidscreen_validation_scene_helper <display-id> [parent-pid]\n");
            return 2;
        }

        uint32_t displayID = (uint32_t)strtoul(argv[1], nullptr, 10);
        pid_t parentPid = (argc >= 3) ? (pid_t)strtol(argv[2], nullptr, 10) : 0;

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

        HelperDelegate* delegate = [[HelperDelegate alloc] init];
        delegate.window = window;
        delegate.parentPid = parentPid;
        app.delegate = delegate;

        fprintf(stderr,
                "[validation-helper] scene visible display=%u frame=%s level=%ld parent=%d\n",
                displayID, NSStringFromRect(frame).UTF8String, (long)window.level,
                (int)parentPid);
        fflush(stderr);

        [app run];
        return 0;
    }
}

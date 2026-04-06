/*
 * DroidScreen macOS - CGVirtualDisplay implementation
 *
 * CGVirtualDisplay is a private CoreGraphics API. We declare the
 * Objective-C interfaces manually since they are not in public headers.
 * This works on macOS 10.14+ but may break in future releases.
 *
 * Pattern based on: github.com/enfp-dev-studio/node-mac-virtual-display
 */

#import "virtual_display.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#import <cstdio>

// ---------------------------------------------------------------------------
// Private API declarations for CGVirtualDisplay classes.
// These are not in any public SDK header.
// ---------------------------------------------------------------------------

@interface CGVirtualDisplayDescriptor : NSObject
@property (nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(NSUInteger)width
                       height:(NSUInteger)height
                  refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (nonatomic, copy) NSArray *modes;
- (instancetype)init;
@end

@interface CGVirtualDisplay : NSObject
@property (nonatomic, readonly) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace droidscreen {

VirtualDisplay::VirtualDisplay() = default;

VirtualDisplay::~VirtualDisplay() {
    destroy();
}

bool VirtualDisplay::create(uint32_t width, uint32_t height, uint32_t fps) {
    if (active_) {
        fprintf(stderr, "[vdisplay] already active, destroying first\n");
        destroy();
    }

    // Check that the private classes exist at runtime.
    Class descClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class dispClass = NSClassFromString(@"CGVirtualDisplay");
    Class settClass = NSClassFromString(@"CGVirtualDisplaySettings");
    Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");

    if (!descClass || !dispClass || !settClass || !modeClass) {
        fprintf(stderr, "[vdisplay] CGVirtualDisplay private API not available "
                "on this macOS version\n");
        return false;
    }

    // Remember the primary display before we create the virtual one.
    CGDirectDisplayID primaryDisplay = CGMainDisplayID();

    // 1. Create descriptor.
    CGVirtualDisplayDescriptor *desc =
        [[descClass alloc] init];
    desc.name = @"DroidScreen";
    desc.maxPixelsWide = width;
    desc.maxPixelsHigh = height;

    // Physical size in mm (assume ~130 PPI for a tablet-like display).
    double ppi = 130.0;
    desc.sizeInMillimeters = CGSizeMake(
        (double)width / ppi * 25.4,
        (double)height / ppi * 25.4);

    desc.serialNum = 0xD50D;  // "DroidScreen" hash
    desc.productID = 0xD5C0;
    desc.vendorID  = 0xDCDC;

    // 2. Create the virtual display.
    CGVirtualDisplay *display =
        [[dispClass alloc] initWithDescriptor:desc];

    if (!display) {
        fprintf(stderr, "[vdisplay] CGVirtualDisplay creation failed\n");
        return false;
    }

    // 3. Configure display modes.
    CGVirtualDisplayMode *mode =
        [[modeClass alloc] initWithWidth:width
                                  height:height
                             refreshRate:(double)fps];

    CGVirtualDisplaySettings *settings =
        [[settClass alloc] init];
    settings.modes = @[mode];

    BOOL applied = [display applySettings:settings];
    if (!applied) {
        fprintf(stderr, "[vdisplay] applySettings failed\n");
        return false;
    }

    CGDirectDisplayID vdID = display.displayID;

    fprintf(stderr, "[vdisplay] created virtual display: ID=%u, %ux%u@%uHz\n",
            vdID, width, height, fps);

    // 4. POST-PROCESSING: Prevent the virtual display from hijacking
    //    the primary display or enabling unwanted mirroring.
    CGDisplayConfigRef config;
    CGBeginDisplayConfiguration(&config);

    // If the virtual display became the main display, restore the original.
    if (CGDisplayIsMain(vdID)) {
        fprintf(stderr, "[vdisplay] virtual display became main, restoring\n");
        CGConfigureDisplayOrigin(config, primaryDisplay, 0, 0);
    }

    // Disable any unwanted mirroring.
    if (CGDisplayMirrorsDisplay(primaryDisplay) == vdID) {
        fprintf(stderr, "[vdisplay] disabling unwanted mirror\n");
        CGConfigureDisplayMirrorOfDisplay(config, primaryDisplay,
                                          kCGNullDirectDisplay);
    }
    CGConfigureDisplayMirrorOfDisplay(config, vdID, kCGNullDirectDisplay);

    CGCompleteDisplayConfiguration(config, kCGConfigurePermanently);

    // Store references.
    descriptor_ = (__bridge_retained void*)desc;
    display_    = (__bridge_retained void*)display;
    settings_   = (__bridge_retained void*)settings;
    display_id_ = vdID;
    width_      = width;
    height_     = height;
    active_     = true;

    return true;
}

void VirtualDisplay::destroy() {
    if (!active_) return;

    fprintf(stderr, "[vdisplay] destroying virtual display ID=%u\n",
            display_id_);

    // Release in reverse order.
    if (display_) {
        CFRelease(display_);
        display_ = nullptr;
    }
    if (settings_) {
        CFRelease(settings_);
        settings_ = nullptr;
    }
    if (descriptor_) {
        CFRelease(descriptor_);
        descriptor_ = nullptr;
    }

    display_id_ = 0;
    active_ = false;

    fprintf(stderr, "[vdisplay] destroyed\n");
}

} // namespace droidscreen

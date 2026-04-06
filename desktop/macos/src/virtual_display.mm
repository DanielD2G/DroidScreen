/*
 * DroidScreen macOS - CGVirtualDisplay implementation
 *
 * CGVirtualDisplay is a private CoreGraphics API. We declare the
 * Objective-C interfaces manually since they are not in public headers.
 * This works on macOS 10.14+ but may break in future releases.
 *
 * HiDPI/Retina support:
 *   - maxPixelsWide/High = framebuffer size (2× logical for Retina)
 *   - CGVirtualDisplayMode width/height = logical (point) resolution
 *   - settings.hiDPI = 1 tells macOS to use 2× pixel density
 *
 * Pattern based on: github.com/nicnacnic/node-mac-virtual-display
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
@property (nonatomic) unsigned int hiDPI;
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

bool VirtualDisplay::create(uint32_t width, uint32_t height,
                            uint32_t fps, bool hidpi) {
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

    // ---- Compute pixel and physical dimensions ----
    //
    // Logical resolution = what macOS shows in "looks like" (points).
    // Framebuffer = actual backing pixels.
    //
    // For HiDPI (Retina): framebuffer = 2× logical, macOS renders at 2× density.
    // For LoDPI (1x):     framebuffer = logical.
    //
    uint32_t framebuffer_w = hidpi ? width * 2 : width;
    uint32_t framebuffer_h = hidpi ? height * 2 : height;

    // Physical size in mm.
    // Use a PPI that makes sense for the display type:
    // - HiDPI: 220 PPI (typical Retina density, like MacBook Pro)
    // - LoDPI: 110 PPI (typical external monitor density)
    // sizeInMillimeters is computed from the FRAMEBUFFER pixels and PPI.
    double ppi = hidpi ? 220.0 : 110.0;
    CGSize physicalSize = CGSizeMake(
        (double)framebuffer_w / ppi * 25.4,
        (double)framebuffer_h / ppi * 25.4);

    fprintf(stderr, "[vdisplay] creating: logical=%ux%u, framebuffer=%ux%u, "
            "hidpi=%s, ppi=%.0f, physical=%.0fx%.0fmm\n",
            width, height, framebuffer_w, framebuffer_h,
            hidpi ? "YES" : "NO", ppi,
            physicalSize.width, physicalSize.height);

    // 1. Create descriptor.
    CGVirtualDisplayDescriptor *desc =
        [[descClass alloc] init];
    desc.name = @"DroidScreen";
    desc.maxPixelsWide = framebuffer_w;
    desc.maxPixelsHigh = framebuffer_h;
    desc.sizeInMillimeters = physicalSize;
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
    //
    // CGVirtualDisplayMode width/height = LOGICAL (point) resolution.
    // macOS maps this to the framebuffer based on the hiDPI setting.
    //
    // When HiDPI is enabled, we add multiple modes at different logical
    // resolutions so macOS offers scaled options in System Settings:
    //   - "More Space"  = larger logical res (e.g. 2560×1600 @ 2x)
    //   - "Default"     = native logical res
    //   - "Larger Text" = smaller logical res (e.g. 1280×800 @ 2x)
    //
    NSMutableArray *modes = [NSMutableArray array];

    // Primary mode at the requested logical resolution.
    [modes addObject:[[modeClass alloc] initWithWidth:width
                                               height:height
                                          refreshRate:(double)fps]];

    if (hidpi) {
        // Common 16:10 scaled resolutions for HiDPI.
        // Each one becomes a "Retina" option backed by 2× pixels.
        struct { uint32_t w; uint32_t h; } scaled[] = {
            {2560, 1600},
            {1920, 1200},
            {1680, 1050},
            {1440,  900},
            {1280,  800},
            {1024,  640},
        };
        for (auto& s : scaled) {
            // Skip if it matches the primary mode (already added).
            if (s.w == width && s.h == height) continue;
            // Only add if the 2× backing fits within maxPixels.
            if (s.w * 2 <= framebuffer_w && s.h * 2 <= framebuffer_h) {
                [modes addObject:[[modeClass alloc] initWithWidth:s.w
                                                           height:s.h
                                                      refreshRate:(double)fps]];
            }
        }
    }

    CGVirtualDisplaySettings *settings =
        [[settClass alloc] init];
    settings.modes = modes;
    settings.hiDPI = hidpi ? 1 : 0;

    fprintf(stderr, "[vdisplay] registering %lu modes (hiDPI=%s)\n",
            (unsigned long)modes.count, hidpi ? "YES" : "NO");

    BOOL applied = [display applySettings:settings];
    if (!applied) {
        fprintf(stderr, "[vdisplay] applySettings failed\n");
        return false;
    }

    CGDirectDisplayID vdID = display.displayID;

    fprintf(stderr, "[vdisplay] created virtual display: ID=%u, "
            "logical=%ux%u@%uHz, framebuffer=%ux%u, hiDPI=%s\n",
            vdID, width, height, fps, framebuffer_w, framebuffer_h,
            hidpi ? "YES" : "NO");

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
    hidpi_      = hidpi;
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
    hidpi_ = false;

    fprintf(stderr, "[vdisplay] destroyed\n");
}

} // namespace droidscreen

/*
 * DroidScreen macOS - ScreenCaptureKit capturer implementation
 */

#import "sck_capturer.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

static void release_cv_pixel_buffer(void* /*release_ctx*/, void* native_handle) {
    if (!native_handle) return;
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(native_handle));
}

// ---------------------------------------------------------------------------
// Objective-C delegate that bridges SCStreamOutput to the C++ callback
// ---------------------------------------------------------------------------

@interface SCKCapturerDelegate : NSObject <SCStreamOutput>
@property (nonatomic, assign) droidscreen::SCKCapturer* owner;
@property (nonatomic, assign) int sampleCount;
@end

@implementation SCKCapturerDelegate

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {

    if (type != SCStreamOutputTypeScreen) return;
    if (!_owner) return;

    // Check frame status -- skip idle frames.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(
        sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict =
            (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFTypeRef statusRef = nullptr;
        if (dict) {
            statusRef = CFDictionaryGetValue(
                dict, (__bridge CFStringRef)SCStreamFrameInfoStatus);
        }
        if (statusRef) {
            CFNumberRef statusNum = (CFNumberRef)statusRef;
            int64_t status = 0;
            CFNumberGetValue(statusNum, kCFNumberSInt64Type, &status);
            // SCFrameStatus: 0=Complete, 1=Idle, 2=Blank, 3=Suspended, 4=Started
            if (status == 1 /* Idle */) {
                // Notify with is_idle=true so the pipeline can skip encoding.
                CVPixelBufferRef pixelBuf =
                    CMSampleBufferGetImageBuffer(sampleBuffer);
                if (pixelBuf) {
                    droidscreen::CapturedFrame frame;
                    frame.native_handle = nullptr;
                    frame.release_ctx = nullptr;
                    frame.release_fn = nullptr;
                    frame.width  = (uint32_t)CVPixelBufferGetWidth(pixelBuf);
                    frame.height = (uint32_t)CVPixelBufferGetHeight(pixelBuf);
                    frame.timestamp_us = 0;
                    frame.is_idle = true;
                    _owner->deliver_frame(frame);
                }
                return;
            }
            if (status != 0 /* Complete */) {
                return;  // Skip blank/suspended/started frames.
            }
        }
    }

    CVPixelBufferRef pixelBuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuf) return;

    if (_sampleCount < 5) {
        CVReturn lockStatus =
            CVPixelBufferLockBaseAddress(pixelBuf, kCVPixelBufferLock_ReadOnly);
        if (lockStatus == kCVReturnSuccess && CVPixelBufferGetPlaneCount(pixelBuf) > 0) {
            const uint8_t* yPlane =
                static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuf, 0));
            size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuf, 0);
            size_t yWidth = CVPixelBufferGetWidthOfPlane(pixelBuf, 0);
            size_t yHeight = CVPixelBufferGetHeightOfPlane(pixelBuf, 0);
            size_t stepX = std::max<size_t>(1, yWidth / 96);
            size_t stepY = std::max<size_t>(1, yHeight / 54);
            uint64_t total = 0;
            uint64_t totalSq = 0;
            uint64_t count = 0;
            uint64_t nonBlack = 0;
            for (size_t y = 0; y < yHeight; y += stepY) {
                const uint8_t* row = yPlane + y * yStride;
                for (size_t x = 0; x < yWidth; x += stepX) {
                    uint8_t v = row[x];
                    total += v;
                    totalSq += (uint64_t)v * (uint64_t)v;
                    nonBlack += v > 8 ? 1 : 0;
                    count++;
                }
            }
            double mean = count ? (double)total / (double)count : 0.0;
            double variance = count
                ? (double)totalSq / (double)count - mean * mean
                : 0.0;
            double nonBlackRatio = count ? (double)nonBlack / (double)count : 0.0;
            fprintf(stderr,
                    "[sck] frame sample %d mean=%.2f variance=%.2f non_black=%.4f size=%zux%zu\n",
                    _sampleCount + 1, mean, variance, nonBlackRatio,
                    yWidth, yHeight);
            fflush(stderr);
            CVPixelBufferUnlockBaseAddress(pixelBuf, kCVPixelBufferLock_ReadOnly);
        }
        _sampleCount++;
    }

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t timestamp_us = 0;
    if (CMTIME_IS_VALID(pts)) {
        timestamp_us = (int64_t)(CMTimeGetSeconds(pts) * 1e6);
    }

    // Retain the pixel buffer so it survives beyond this callback.
    // The pipeline must release it after encoding.
    CVPixelBufferRetain(pixelBuf);

    droidscreen::CapturedFrame frame;
    frame.native_handle = pixelBuf;
    frame.release_ctx = nullptr;
    frame.release_fn = release_cv_pixel_buffer;
    frame.width  = (uint32_t)CVPixelBufferGetWidth(pixelBuf);
    frame.height = (uint32_t)CVPixelBufferGetHeight(pixelBuf);
    frame.timestamp_us = timestamp_us;
    frame.is_idle = false;

    _owner->deliver_frame(frame);
}

@end

// ---------------------------------------------------------------------------
// C++ implementation
// ---------------------------------------------------------------------------

namespace droidscreen {

void SCKCapturer::deliver_frame(const CapturedFrame& frame) {
    if (on_frame_) {
        on_frame_(frame);
    }
}

SCKCapturer::SCKCapturer() = default;

SCKCapturer::~SCKCapturer() {
    stop();
}

bool SCKCapturer::init_with_display_id(uint32_t cg_display_id,
                                       uint32_t capture_width,
                                       uint32_t capture_height,
                                       uint32_t target_fps) {
    __block bool success = false;
    __block SCDisplay* chosen_display = nil;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent * _Nullable content, NSError * _Nullable error) {
            if (error || !content) {
                fprintf(stderr, "[sck] getShareableContent failed: %s\n",
                        error ? [[error localizedDescription] UTF8String]
                              : "nil content");
                dispatch_semaphore_signal(sem);
                return;
            }

            for (SCDisplay *d in content.displays) {
                if (d.displayID == cg_display_id) {
                    chosen_display = d;
                    break;
                }
            }

            if (!chosen_display) {
                fprintf(stderr, "[sck] display ID %u not found in SCK\n",
                        cg_display_id);
                dispatch_semaphore_signal(sem);
                return;
            }

            success = true;
            dispatch_semaphore_signal(sem);
        }];

    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sem, timeout) != 0) {
        fprintf(stderr, "[sck] getShareableContent timed out (10s) — permission denied?\n");
        return false;
    }
    if (!success) return false;

    // SCDisplay.width/height returns LOGICAL (point) resolution.
    uint32_t display_logical_w = (uint32_t)chosen_display.width;
    uint32_t display_logical_h = (uint32_t)chosen_display.height;

    fprintf(stderr, "[sck] display ID %u: logical=%ux%u\n",
            cg_display_id, display_logical_w, display_logical_h);

    // Use explicit capture dimensions if provided, otherwise use the display's
    // logical resolution. For HiDPI displays, capturing at the logical resolution
    // makes SCK downsample from the 2x framebuffer — producing sharp output.
    width_  = (capture_width > 0)  ? capture_width  : display_logical_w;
    height_ = (capture_height > 0) ? capture_height : display_logical_h;

    SCContentFilter* filter = nil;
    if (@available(macOS 13.0, *)) {
        filter = [[SCContentFilter alloc] initWithDisplay:chosen_display
                                    excludingApplications:@[]
                                         exceptingWindows:@[]];
    }
    if (!filter) {
        filter = [[SCContentFilter alloc] initWithDisplay:chosen_display
                                         excludingWindows:@[]];
    }

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width  = width_;
    config.height = height_;
    config.minimumFrameInterval = CMTimeMake(1, target_fps > 0 ? target_fps : 60);
    config.queueDepth = 3;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.showsCursor = YES;

    fprintf(stderr, "[sck] initialized for display ID %u (capture=%ux%u, fps=%u)\n",
            cg_display_id, width_, height_, target_fps);

    stream_ = [[SCStream alloc] initWithFilter:filter
                                 configuration:config
                                      delegate:nil];

    delegate_ = [[SCKCapturerDelegate alloc] init];
    delegate_.owner = this;

    NSError* addErr = nil;
    [stream_ addStreamOutput:delegate_
                        type:SCStreamOutputTypeScreen
              sampleHandlerQueue:dispatch_get_global_queue(
                  QOS_CLASS_USER_INTERACTIVE, 0)
                       error:&addErr];
    if (addErr) {
        fprintf(stderr, "[sck] addStreamOutput failed: %s\n",
                [[addErr localizedDescription] UTF8String]);
        return false;
    }

    fprintf(stderr, "[sck] initialized for display ID %u (capture=%ux%u)\n",
            cg_display_id, width_, height_);
    return true;
}

bool SCKCapturer::init(uint32_t display_index) {
    __block bool success = false;
    __block uint32_t cap_w = 0, cap_h = 0;
    __block SCDisplay* chosen_display = nil;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent * _Nullable content, NSError * _Nullable error) {
            if (error || !content) {
                fprintf(stderr, "[sck] getShareableContent failed: %s\n",
                        error ? [[error localizedDescription] UTF8String]
                              : "nil content");
                dispatch_semaphore_signal(sem);
                return;
            }

            NSArray<SCDisplay*>* displays = content.displays;
            if (display_index >= displays.count) {
                fprintf(stderr, "[sck] display index %u out of range "
                        "(have %lu displays)\n",
                        display_index, (unsigned long)displays.count);
                dispatch_semaphore_signal(sem);
                return;
            }

            chosen_display = displays[display_index];
            cap_w = (uint32_t)chosen_display.width;
            cap_h = (uint32_t)chosen_display.height;

            fprintf(stderr, "[sck] display %u: %ux%u\n",
                    display_index, cap_w, cap_h);

            success = true;
            dispatch_semaphore_signal(sem);
        }];

    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sem, timeout) != 0) {
        fprintf(stderr, "[sck] getShareableContent timed out (10s) — permission denied?\n");
        return false;
    }

    if (!success) return false;

    width_  = cap_w;
    height_ = cap_h;

    // Create the filter (capture just this display, no apps excluded).
    SCContentFilter* filter = nil;
    if (@available(macOS 13.0, *)) {
        filter = [[SCContentFilter alloc] initWithDisplay:chosen_display
                                    excludingApplications:@[]
                                         exceptingWindows:@[]];
    }
    if (!filter) {
        filter = [[SCContentFilter alloc] initWithDisplay:chosen_display
                                         excludingWindows:@[]];
    }

    // Configure the stream.
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width  = cap_w;
    config.height = cap_h;
    config.minimumFrameInterval = CMTimeMake(1, 60);  // 60 fps
    config.queueDepth = 3;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.showsCursor = YES;

    // Create the stream.
    stream_ = [[SCStream alloc] initWithFilter:filter
                                 configuration:config
                                      delegate:nil];

    // Create and attach the output delegate.
    delegate_ = [[SCKCapturerDelegate alloc] init];
    delegate_.owner = this;

    NSError* addErr = nil;
    [stream_ addStreamOutput:delegate_
                        type:SCStreamOutputTypeScreen
              sampleHandlerQueue:dispatch_get_global_queue(
                  QOS_CLASS_USER_INTERACTIVE, 0)
                       error:&addErr];
    if (addErr) {
        fprintf(stderr, "[sck] addStreamOutput failed: %s\n",
                [[addErr localizedDescription] UTF8String]);
        return false;
    }

    fprintf(stderr, "[sck] initialized for display %u (%ux%u)\n",
            display_index, cap_w, cap_h);
    return true;
}

bool SCKCapturer::start(std::function<void(const CapturedFrame&)> on_frame) {
    on_frame_ = std::move(on_frame);
    running_.store(true);

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool ok = false;

    [stream_ startCaptureWithCompletionHandler:^(NSError * _Nullable error) {
        if (error) {
            fprintf(stderr, "[sck] startCapture failed: %s\n",
                    [[error localizedDescription] UTF8String]);
        } else {
            ok = true;
        }
        dispatch_semaphore_signal(sem);
    }];

    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sem, timeout) != 0) {
        fprintf(stderr, "[sck] startCapture timed out (10s)\n");
        return false;
    }

    if (ok) {
        fprintf(stderr, "[sck] capture started\n");
    }
    return ok;
}

void SCKCapturer::stop() {
    if (!running_.exchange(false)) return;

    if (stream_) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        [stream_ stopCaptureWithCompletionHandler:^(NSError * _Nullable error) {
            if (error) {
                fprintf(stderr, "[sck] stopCapture error: %s\n",
                        [[error localizedDescription] UTF8String]);
            }
            dispatch_semaphore_signal(sem);
        }];

        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        stream_   = nil;
        delegate_ = nil;

        fprintf(stderr, "[sck] capture stopped\n");
    }
}

} // namespace droidscreen

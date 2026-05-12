/*
 * DroidScreen macOS - ScreenCaptureKit capturer implementation
 */

#import "sck_capturer.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static int64_t steady_now_us() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        tp.time_since_epoch()).count();
}

static void release_cv_pixel_buffer(void* /*release_ctx*/, void* native_handle) {
    if (!native_handle) return;
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(native_handle));
}

// ---------------------------------------------------------------------------
// Objective-C delegate that bridges SCStreamOutput to the C++ callback
// ---------------------------------------------------------------------------

@interface SCKCapturerDelegate : NSObject <SCStreamOutput, SCStreamDelegate> {
    std::vector<uint8_t> _motionSample;
    std::vector<uint8_t> _previousMotionSample;
}
@property (nonatomic, assign) droidscreen::SCKCapturer* owner;
@property (nonatomic, assign) int sampleCount;
@end

@implementation SCKCapturerDelegate

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    (void)stream;
    if (!_owner) return;
    NSString* message = error.localizedDescription ?: @"ScreenCaptureKit stream stopped";
    _owner->handle_stream_error(message.UTF8String);
}

- (float)measureMotionScore:(CVPixelBufferRef)pixelBuf {
    if (!pixelBuf || CVPixelBufferGetPlaneCount(pixelBuf) == 0) {
        return 0.0f;
    }

    CVReturn lockStatus =
        CVPixelBufferLockBaseAddress(pixelBuf, kCVPixelBufferLock_ReadOnly);
    if (lockStatus != kCVReturnSuccess) {
        return 0.0f;
    }

    const uint8_t* yPlane =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuf, 0));
    size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuf, 0);
    size_t yWidth = CVPixelBufferGetWidthOfPlane(pixelBuf, 0);
    size_t yHeight = CVPixelBufferGetHeightOfPlane(pixelBuf, 0);
    if (!yPlane || yWidth == 0 || yHeight == 0 || yStride == 0) {
        CVPixelBufferUnlockBaseAddress(pixelBuf, kCVPixelBufferLock_ReadOnly);
        return 0.0f;
    }

    constexpr size_t kGridW = 64;
    constexpr size_t kGridH = 36;
    _motionSample.resize(kGridW * kGridH);
    for (size_t gy = 0; gy < kGridH; gy++) {
        size_t y = std::min(yHeight - 1, (gy * yHeight) / kGridH);
        const uint8_t* row = yPlane + y * yStride;
        for (size_t gx = 0; gx < kGridW; gx++) {
            size_t x = std::min(yWidth - 1, (gx * yWidth) / kGridW);
            _motionSample[gy * kGridW + gx] = row[x];
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuf, kCVPixelBufferLock_ReadOnly);

    float score = 0.0f;
    if (_previousMotionSample.size() == _motionSample.size()) {
        uint64_t totalDiff = 0;
        for (size_t i = 0; i < _motionSample.size(); i++) {
            int diff = (int)_motionSample[i] - (int)_previousMotionSample[i];
            totalDiff += (uint64_t)(diff < 0 ? -diff : diff);
        }
        score = (float)((double)totalDiff / (double)_motionSample.size());
    }

    _previousMotionSample = _motionSample;
    return score;
}

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
    float motionScore = [self measureMotionScore:pixelBuf];

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

    // Retain the pixel buffer so it survives beyond this callback.
    // The pipeline must release it after encoding.
    CVPixelBufferRetain(pixelBuf);

    droidscreen::CapturedFrame frame;
    frame.native_handle = pixelBuf;
    frame.release_ctx = nullptr;
    frame.release_fn = release_cv_pixel_buffer;
    frame.width  = (uint32_t)CVPixelBufferGetWidth(pixelBuf);
    frame.height = (uint32_t)CVPixelBufferGetHeight(pixelBuf);
    frame.timestamp_us = steady_now_us();
    frame.is_idle = false;
    frame.motion_score = motionScore;

    _owner->deliver_frame(frame);
}

@end

// ---------------------------------------------------------------------------
// C++ implementation
// ---------------------------------------------------------------------------

namespace droidscreen {

void SCKCapturer::deliver_frame(const CapturedFrame& frame) {
    if (running_.load() && on_frame_) {
        on_frame_(frame);
        return;
    }

    if (frame.native_handle && frame.release_fn) {
        CapturedFrame releasable = frame;
        frame.release_fn(releasable.release_ctx, releasable.native_handle);
    }
}

void SCKCapturer::handle_stream_error(const char* message) {
    if (!running_.exchange(false)) {
        return;
    }
    fprintf(stderr, "[sck] stream stopped with error: %s\n",
            message ? message : "unknown");
    fflush(stderr);
    if (on_error_) {
        on_error_(message ? message : "ScreenCaptureKit stream stopped");
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
    has_display_id_ = true;
    display_id_ = cg_display_id;
    configured_capture_width_ = capture_width;
    configured_capture_height_ = capture_height;
    configured_target_fps_ = target_fps;
    has_display_index_ = false;

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
    // The pipeline still keeps only the freshest frame. A deeper SCK queue gives
    // macOS enough recyclable buffers when VT and idle resend briefly retain one.
    config.queueDepth = 8;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.showsCursor = YES;

    fprintf(stderr, "[sck] initialized for display ID %u (capture=%ux%u, fps=%u)\n",
            cg_display_id, width_, height_, target_fps);

    delegate_ = [[SCKCapturerDelegate alloc] init];
    delegate_.owner = this;

    stream_ = [[SCStream alloc] initWithFilter:filter
                                 configuration:config
                                      delegate:delegate_];

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
    has_display_index_ = true;
    display_index_ = display_index;
    has_display_id_ = false;

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
    // The pipeline still keeps only the freshest frame. A deeper SCK queue gives
    // macOS enough recyclable buffers when VT and idle resend briefly retain one.
    config.queueDepth = 8;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.showsCursor = YES;

    // Create and attach the output delegate.
    delegate_ = [[SCKCapturerDelegate alloc] init];
    delegate_.owner = this;

    // Create the stream.
    stream_ = [[SCStream alloc] initWithFilter:filter
                                 configuration:config
                                      delegate:delegate_];

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

bool SCKCapturer::restart(std::function<void(const CapturedFrame&)> on_frame) {
    fprintf(stderr, "[sck] restarting capture\n");
    fflush(stderr);

    stop();

    bool initialized = false;
    if (has_display_id_) {
        initialized = init_with_display_id(display_id_,
                                           configured_capture_width_,
                                           configured_capture_height_,
                                           configured_target_fps_);
    } else if (has_display_index_) {
        initialized = init(display_index_);
    }

    if (!initialized) {
        fprintf(stderr, "[sck] restart failed: capture init failed\n");
        return false;
    }

    if (!start(std::move(on_frame))) {
        fprintf(stderr, "[sck] restart failed: startCapture failed\n");
        return false;
    }

    return true;
}

void SCKCapturer::set_error_callback(std::function<void(const char*)> on_error) {
    on_error_ = std::move(on_error);
}

void SCKCapturer::stop() {
    bool was_running = running_.exchange(false);
    on_frame_ = nullptr;

    if (stream_) {
        if (was_running) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);

            [stream_ stopCaptureWithCompletionHandler:^(NSError * _Nullable error) {
                if (error) {
                    fprintf(stderr, "[sck] stopCapture error: %s\n",
                            [[error localizedDescription] UTF8String]);
                }
                dispatch_semaphore_signal(sem);
            }];

            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        }

        delegate_.owner = nullptr;
        stream_   = nil;
        delegate_ = nil;

        fprintf(stderr, "[sck] capture stopped\n");
    }
}

} // namespace droidscreen

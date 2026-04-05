/*
 * DroidScreen macOS - ScreenCaptureKit capturer implementation
 */

#import "sck_capturer.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <dispatch/dispatch.h>

#include <cstdio>

// ---------------------------------------------------------------------------
// Objective-C delegate that bridges SCStreamOutput to the C++ callback
// ---------------------------------------------------------------------------

@interface SCKCapturerDelegate : NSObject <SCStreamOutput>
@property (nonatomic, assign) droidscreen::SCKCapturer* owner;
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
                    frame.native_handle = pixelBuf;
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

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t timestamp_us = 0;
    if (CMTIME_IS_VALID(pts)) {
        timestamp_us = (int64_t)(CMTimeGetSeconds(pts) * 1e6);
    }

    droidscreen::CapturedFrame frame;
    frame.native_handle = pixelBuf;
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

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (!success) return false;

    width_  = cap_w;
    height_ = cap_h;

    // Create the filter (capture just this display, no apps excluded).
    SCContentFilter* filter =
        [[SCContentFilter alloc] initWithDisplay:chosen_display
                                excludingWindows:@[]];

    // Configure the stream.
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width  = cap_w;
    config.height = cap_h;
    config.minimumFrameInterval = CMTimeMake(1, 60);  // 60 fps
    config.queueDepth = 3;
    config.pixelFormat = kCVPixelFormatType_32BGRA;
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

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

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

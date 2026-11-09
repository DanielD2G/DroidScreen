/*
 * DroidScreen macOS - Lightweight MediaRemote helper
 *
 * This dynamic library is loaded by /usr/bin/perl so the process can query
 * MediaRemote on modern macOS versions. adapter_stream() keeps the process
 * alive, registers for Now Playing notifications and writes one JSON line per
 * change to stdout (artwork downscaled to a small JPEG thumbnail, once per
 * track).
 *
 * stdin protocol: each line is a decimal "media action generation". Reading
 * one forces an immediate snapshot, and every later line written to stdout
 * carries that generation in "gen" so the app can drop snapshots taken before
 * a tablet media action. EOF on stdin (the app quit) ends the process.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <ImageIO/ImageIO.h>

typedef void (^MRNowPlayingInfoCompletion)(NSDictionary* info);
typedef void (^MRNowPlayingPIDCompletion)(int pid);
typedef void (^MRNowPlayingIsPlayingCompletion)(bool isPlaying);

typedef void (*MRMediaRemoteGetNowPlayingInfoFunction)(dispatch_queue_t queue,
                                                       MRNowPlayingInfoCompletion handler);
typedef void (*MRMediaRemoteGetNowPlayingApplicationPIDFunction)(
    dispatch_queue_t queue,
    MRNowPlayingPIDCompletion handler);
typedef void (*MRMediaRemoteGetNowPlayingApplicationIsPlayingFunction)(
    dispatch_queue_t queue,
    MRNowPlayingIsPlayingCompletion handler);
typedef void (*MRMediaRemoteRegisterForNowPlayingNotificationsFunction)(dispatch_queue_t queue);

static MRMediaRemoteGetNowPlayingInfoFunction g_get_now_playing_info = NULL;
static MRMediaRemoteGetNowPlayingApplicationPIDFunction g_get_now_playing_pid = NULL;
static MRMediaRemoteGetNowPlayingApplicationIsPlayingFunction g_get_now_playing_is_playing = NULL;
static MRMediaRemoteRegisterForNowPlayingNotificationsFunction g_register_notifications = NULL;
static dispatch_queue_t g_mediaremote_queue = NULL;

static void print_json_line(NSString* line) {
    fprintf(stdout, "%s\n", [line UTF8String]);
    fflush(stdout);
}

static BOOL load_mediaremote_symbols(void) {
    static dispatch_once_t onceToken;
    static BOOL loaded = NO;
    dispatch_once(&onceToken, ^{
        CFURLRef bundleURL = (__bridge CFURLRef)[NSURL fileURLWithPath:
            @"/System/Library/PrivateFrameworks/MediaRemote.framework"];
        CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);
        if (!bundle) return;

        g_get_now_playing_info =
            (MRMediaRemoteGetNowPlayingInfoFunction)CFBundleGetFunctionPointerForName(
                bundle, CFSTR("MRMediaRemoteGetNowPlayingInfo"));
        g_get_now_playing_pid =
            (MRMediaRemoteGetNowPlayingApplicationPIDFunction)CFBundleGetFunctionPointerForName(
                bundle, CFSTR("MRMediaRemoteGetNowPlayingApplicationPID"));
        g_get_now_playing_is_playing =
            (MRMediaRemoteGetNowPlayingApplicationIsPlayingFunction)
                CFBundleGetFunctionPointerForName(
                    bundle, CFSTR("MRMediaRemoteGetNowPlayingApplicationIsPlaying"));
        g_register_notifications =
            (MRMediaRemoteRegisterForNowPlayingNotificationsFunction)
                CFBundleGetFunctionPointerForName(
                    bundle, CFSTR("MRMediaRemoteRegisterForNowPlayingNotifications"));

        if (g_get_now_playing_info && g_get_now_playing_pid &&
            g_get_now_playing_is_playing && g_register_notifications) {
            g_mediaremote_queue = dispatch_queue_create(
                "com.droidscreen.mediaremote-helper", DISPATCH_QUEUE_SERIAL);
            loaded = YES;
        }
    });
    return loaded;
}

static NSNumber* current_elapsed_time(NSDictionary* info) {
    NSNumber* elapsed = info[@"kMRMediaRemoteNowPlayingInfoElapsedTime"];
    if (![elapsed isKindOfClass:[NSNumber class]]) return nil;

    NSDate* timestamp = info[@"kMRMediaRemoteNowPlayingInfoTimestamp"];
    NSNumber* playbackRate = info[@"kMRMediaRemoteNowPlayingInfoPlaybackRate"];
    if (![timestamp isKindOfClass:[NSDate class]] ||
        ![playbackRate isKindOfClass:[NSNumber class]]) {
        return elapsed;
    }

    NSTimeInterval delta = [[NSDate date] timeIntervalSinceDate:timestamp];
    double value = [elapsed doubleValue] + delta * [playbackRate doubleValue];
    return @(MAX(0.0, value));
}

static NSString* serialize_payload(NSDictionary* payload) {
    NSError* error = nil;
    NSData* json = [NSJSONSerialization dataWithJSONObject:payload options:0 error:&error];
    if (!json) return nil;
    return [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
}

// Downscaled artwork, computed once per track: the tablet tile is small and
// the JSON travels on the same socket as the video.
static const int kArtworkMaxPixels = 256;
static NSString* g_thumb_key = nil;
static NSString* g_thumb_b64 = nil;

static NSString* artwork_thumbnail_b64(NSData* artwork, NSString* trackKey) {
    NSString* key = [NSString stringWithFormat:@"%@\n%lu", trackKey, (unsigned long)artwork.length];
    if ([g_thumb_key isEqualToString:key]) return g_thumb_b64;

    NSString* result = nil;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)artwork, NULL);
    if (src) {
        NSDictionary* opts = @{
            (__bridge id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
            (__bridge id)kCGImageSourceCreateThumbnailWithTransform: @YES,
            (__bridge id)kCGImageSourceThumbnailMaxPixelSize: @(kArtworkMaxPixels),
        };
        CGImageRef thumb = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
        if (thumb) {
            NSMutableData* jpeg = [NSMutableData data];
            CGImageDestinationRef dst = CGImageDestinationCreateWithData(
                (__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, NULL);
            if (dst) {
                NSDictionary* props = @{
                    (__bridge id)kCGImageDestinationLossyCompressionQuality: @0.8,
                };
                CGImageDestinationAddImage(dst, thumb, (__bridge CFDictionaryRef)props);
                if (CGImageDestinationFinalize(dst) && jpeg.length > 0) {
                    result = [jpeg base64EncodedStringWithOptions:0];
                }
                CFRelease(dst);
            }
            CGImageRelease(thumb);
        }
        CFRelease(src);
    }

    g_thumb_key = key;
    g_thumb_b64 = result;
    return result;
}

// Synchronous snapshot. Never call it on g_mediaremote_queue (the MediaRemote
// callbacks run there). Returns nil when the query timed out.
static NSMutableDictionary* collect_snapshot(void) {
    NSMutableDictionary* payload = [NSMutableDictionary dictionary];
    dispatch_group_t group = dispatch_group_create();

    dispatch_group_enter(group);
    g_get_now_playing_pid(g_mediaremote_queue, ^(int pid) {
        if (pid > 0) {
            payload[@"processIdentifier"] = @(pid);
            NSRunningApplication* app =
                [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
            if (app.bundleIdentifier.length > 0) {
                payload[@"bundleIdentifier"] = app.bundleIdentifier;
            }
        }
        dispatch_group_leave(group);
    });

    dispatch_group_enter(group);
    g_get_now_playing_is_playing(g_mediaremote_queue, ^(bool isPlaying) {
        payload[@"playing"] = @(isPlaying);
        dispatch_group_leave(group);
    });

    __block NSData* artworkData = nil;
    dispatch_group_enter(group);
    g_get_now_playing_info(g_mediaremote_queue, ^(NSDictionary* info) {
        if ([info isKindOfClass:[NSDictionary class]]) {
            NSString* title = info[@"kMRMediaRemoteNowPlayingInfoTitle"];
            NSString* artist = info[@"kMRMediaRemoteNowPlayingInfoArtist"];
            NSString* album = info[@"kMRMediaRemoteNowPlayingInfoAlbum"];
            NSNumber* duration = info[@"kMRMediaRemoteNowPlayingInfoDuration"];
            NSNumber* elapsedNow = current_elapsed_time(info);
            NSNumber* elapsed = info[@"kMRMediaRemoteNowPlayingInfoElapsedTime"];
            NSNumber* playbackRate = info[@"kMRMediaRemoteNowPlayingInfoPlaybackRate"];
            NSData* artwork = info[@"kMRMediaRemoteNowPlayingInfoArtworkData"];

            if (title.length > 0) payload[@"title"] = title;
            if (artist.length > 0) payload[@"artist"] = artist;
            if (album.length > 0) payload[@"album"] = album;
            if ([duration isKindOfClass:[NSNumber class]]) payload[@"duration"] = duration;
            if ([elapsed isKindOfClass:[NSNumber class]]) payload[@"elapsedTime"] = elapsed;
            if ([elapsedNow isKindOfClass:[NSNumber class]]) payload[@"elapsedTimeNow"] = elapsedNow;
            if ([playbackRate isKindOfClass:[NSNumber class]]) {
                payload[@"playbackRate"] = playbackRate;
            }
            if ([artwork isKindOfClass:[NSData class]] && artwork.length > 0) {
                artworkData = artwork;
            }
        }
        dispatch_group_leave(group);
    });

    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC);
    if (dispatch_group_wait(group, timeout) != 0) return nil;

    NSString* title = payload[@"title"];
    if (title.length == 0 || payload[@"playing"] == nil) {
        // Nothing playing: an empty snapshot.
        return [NSMutableDictionary dictionary];
    }
    if (artworkData) {
        NSString* trackKey = [NSString stringWithFormat:@"%@\n%@\n%@", title,
                              payload[@"artist"] ?: @"", payload[@"album"] ?: @""];
        NSString* thumb = artwork_thumbnail_b64(artworkData, trackKey);
        if (thumb.length > 0) {
            payload[@"artworkMimeType"] = @"image/jpeg";
            payload[@"artworkData"] = thumb;
        }
    }
    return payload;
}

// Emission state, only touched on g_emit_queue.
static dispatch_queue_t g_emit_queue = NULL;
static unsigned long long g_gen = 0;
static NSString* g_last_emitted = nil;   // last line minus the moving fields
static BOOL g_emit_scheduled = NO;

static void emit_snapshot(BOOL force) {
    @autoreleasepool {
        NSMutableDictionary* payload = collect_snapshot();
        if (!payload) return;  // MediaRemote timed out; the next change retries.

        // elapsedTimeNow moves on every query; compare without it.
        NSMutableDictionary* stable = [payload mutableCopy];
        [stable removeObjectForKey:@"elapsedTimeNow"];
        NSString* stableJson = serialize_payload(stable) ?: @"";
        if (!force && [g_last_emitted isEqualToString:stableJson]) return;
        g_last_emitted = stableJson;

        payload[@"gen"] = @(g_gen);
        NSString* json = serialize_payload(payload);
        if (json) print_json_line(json);
    }
}

static void schedule_emit(void) {
    dispatch_async(g_emit_queue, ^{
        if (g_emit_scheduled) return;
        g_emit_scheduled = YES;
        // Coalesce the bursts MediaRemote posts around a track change.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), g_emit_queue, ^{
            g_emit_scheduled = NO;
            emit_snapshot(NO);
        });
    });
}

static void watch_stdin(void) {
    static dispatch_source_t source;
    static NSMutableData* pending;
    pending = [NSMutableData data];
    source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, g_emit_queue);
    dispatch_source_set_event_handler(source, ^{
        char buf[256];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) exit(0);  // The app went away.
        [pending appendBytes:buf length:(NSUInteger)n];
        BOOL refresh = NO;
        for (;;) {
            const char* bytes = (const char*)pending.bytes;
            const char* nl = memchr(bytes, '\n', pending.length);
            if (!nl) break;
            NSUInteger len = (NSUInteger)(nl - bytes);
            NSString* line = [[NSString alloc] initWithBytes:bytes length:len
                                                    encoding:NSUTF8StringEncoding];
            [pending replaceBytesInRange:NSMakeRange(0, len + 1) withBytes:NULL length:0];
            unsigned long long gen = strtoull(line.UTF8String ?: "0", NULL, 10);
            if (gen > g_gen) g_gen = gen;
            refresh = YES;
        }
        if (refresh) emit_snapshot(YES);
    });
    dispatch_resume(source);
}

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) void adapter_stream(void);
#ifdef __cplusplus
}
#endif

void adapter_stream(void) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_DFL);
        if (!load_mediaremote_symbols()) {
            fprintf(stderr, "MediaRemote symbols unavailable\n");
            exit(1);
        }
        g_emit_queue = dispatch_queue_create("com.droidscreen.mediaremote-emit",
                                             DISPATCH_QUEUE_SERIAL);

        NSArray<NSString*>* names = @[
            @"kMRMediaRemoteNowPlayingInfoDidChangeNotification",
            @"kMRMediaRemoteNowPlayingApplicationIsPlayingDidChangeNotification",
            @"kMRMediaRemoteNowPlayingApplicationDidChangeNotification",
            @"kMRMediaRemoteNowPlayingApplicationClientStateDidChange",
        ];
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (NSString* name in names) {
            [center addObserverForName:name object:nil queue:nil
                            usingBlock:^(NSNotification* note) { schedule_emit(); }];
        }
        g_register_notifications(g_mediaremote_queue);

        watch_stdin();
        dispatch_async(g_emit_queue, ^{ emit_snapshot(YES); });
    }
    dispatch_main();
}

/*
 * DroidScreen macOS - Lightweight MediaRemote helper
 *
 * This dynamic library is loaded by /usr/bin/perl so the process can query
 * MediaRemote on modern macOS versions and return JSON metadata, including
 * artwork data.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

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

static MRMediaRemoteGetNowPlayingInfoFunction g_get_now_playing_info = NULL;
static MRMediaRemoteGetNowPlayingApplicationPIDFunction g_get_now_playing_pid = NULL;
static MRMediaRemoteGetNowPlayingApplicationIsPlayingFunction g_get_now_playing_is_playing = NULL;
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

        if (g_get_now_playing_info && g_get_now_playing_pid &&
            g_get_now_playing_is_playing) {
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

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) void adapter_get_env(void);
#ifdef __cplusplus
}
#endif

void adapter_get_env(void) {
    @autoreleasepool {
        if (!load_mediaremote_symbols()) {
            print_json_line(@"null");
            return;
        }

        __block NSMutableDictionary* payload = [NSMutableDictionary dictionary];
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
                NSString* artworkMimeType = info[@"kMRMediaRemoteNowPlayingInfoArtworkMIMEType"];
                NSData* artworkData = info[@"kMRMediaRemoteNowPlayingInfoArtworkData"];

                if (title.length > 0) payload[@"title"] = title;
                if (artist.length > 0) payload[@"artist"] = artist;
                if (album.length > 0) payload[@"album"] = album;
                if ([duration isKindOfClass:[NSNumber class]]) payload[@"duration"] = duration;
                if ([elapsed isKindOfClass:[NSNumber class]]) payload[@"elapsedTime"] = elapsed;
                if ([elapsedNow isKindOfClass:[NSNumber class]]) payload[@"elapsedTimeNow"] = elapsedNow;
                if ([playbackRate isKindOfClass:[NSNumber class]]) {
                    payload[@"playbackRate"] = playbackRate;
                }
                if (artworkMimeType.length > 0) payload[@"artworkMimeType"] = artworkMimeType;
                if (artworkData.length > 0) {
                    payload[@"artworkData"] = [artworkData base64EncodedStringWithOptions:0];
                }
            }
            dispatch_group_leave(group);
        });

        dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC);
        if (dispatch_group_wait(group, timeout) != 0) {
            print_json_line(@"null");
            return;
        }

        NSString* title = payload[@"title"];
        if (title.length == 0 || payload[@"playing"] == nil) {
            print_json_line(@"null");
            return;
        }

        NSString* json = serialize_payload(payload);
        print_json_line(json ?: @"null");
    }
}

/*
 * DroidScreen macOS - boundary between the streaming core (ObjC++ in main.mm)
 * and the SwiftUI interface (the Swift files in src/ui).
 *
 * Plain Objective-C on purpose: Swift imports it through a bridging header and
 * main.mm implements DSUIHost. Everything crosses as Foundation types so the
 * Swift side never sees C++.
 */

#pragma once

#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Actions the interface asks the streaming core to perform. Called on the main thread.
@protocol DSUIHost <NSObject>
/// Connect with the automatic target choice, or disconnect when streaming.
- (void)uiToggleConnect;
/// Connect to a Wi-Fi tablet by its stable id (pairs it first if needed).
- (void)uiConnectWifiTablet:(NSString*)tabletId;
/// Settings were written to NSUserDefaults; restart the stream if `reconnect`.
- (void)uiSettingsChanged:(BOOL)reconnect;
/// Deck shortcut list changed in NSUserDefaults; rebuild and push the layout.
- (void)uiDeckAppsChanged;
/// Forget the pairing token stored for a Wi-Fi tablet.
- (void)uiForgetTablet:(NSString*)tabletId;
/// Measure link throughput and pick a bitrate cap. Result arrives via speedTestFinished:.
- (void)uiRunSpeedTest;
- (void)uiQuit;
@end

/// The interface. Created by DSUICreateController(); every method must be called on the main thread.
@protocol DSUIController <NSObject>
/// Connection state. Keys:
///   phase       NSString  "idle" | "connecting" | "streaming"
///   detail      NSString  human readable status line
///   transport   NSString  "wifi" | "usb" (when connecting/streaming)
///   tabletName  NSString
///   address     NSString  peer address or host
///   width, height, fps (NSNumber), hidpi (BOOL NSNumber), codec (NSString)
///   deviceKey   NSString  settings key of the tablet being streamed to
///   usbDevice   BOOL NSNumber, a tablet is attached over adb
///   usbDeviceKey, usbDeviceName  NSString, that tablet ("usb:<serial>")
- (void)updateState:(NSDictionary<NSString*, id>*)state;
/// Live stats, about once a second while streaming. Keys:
///   latencyMs, fps, targetFps, mbps, capMbps (NSNumber)
- (void)updateStats:(NSDictionary<NSString*, id>*)stats;
/// Tablets seen on the network. Each: id, name, paired, compatible, current (NSString/BOOL NSNumber).
- (void)updateTablets:(NSArray<NSDictionary<NSString*, id>*>*)tablets;
/// throughputKbps == 0 means the test failed.
- (void)speedTestFinishedWithThroughput:(NSInteger)throughputKbps selectedKbps:(NSInteger)selectedKbps;
- (void)showSettings;
@end

#ifdef __cplusplus
extern "C" {
#endif
/// Implemented in Swift (@_cdecl). Installs the menu bar item and returns the controller.
id<DSUIController> DSUICreateController(id<DSUIHost> host);
#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

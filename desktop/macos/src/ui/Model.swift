// DroidScreen macOS - observable state for the SwiftUI interface.

import AppKit
import SwiftUI

struct TabletInfo: Identifiable, Equatable {
    let id: String
    let name: String
    let paired: Bool
    let compatible: Bool
    let current: Bool
}

enum SpeedTestState: Equatable {
    case idle, running
    case done(throughputKbps: Int, selectedKbps: Int)
    case failed
}

/// Everything the views render. Updated from DSUIController calls on the main thread.
final class UIModel: ObservableObject {
    weak var host: DSUIHost?

    @Published var phase = "idle"            // idle | connecting | streaming
    @Published var detail = "Not connected"
    @Published var transport: String?        // wifi | usb
    @Published var tabletName: String?
    @Published var address: String?
    @Published var width = 0
    @Published var height = 0
    @Published var fps = 0
    @Published var hidpi = false
    @Published var codec = "HEVC"
    @Published var usbDevice = false
    @Published var activeDeviceKey: String?
    @Published var usbDeviceKey: String?
    @Published var usbDeviceName: String?

    @Published var latencyMs: Double?
    @Published var fpsNow: Double = 0
    @Published var targetFps: Double = 60
    @Published var mbps: Double = 0
    @Published var capMbps: Double = 30
    /// One latency sample per second, last 60 s.
    @Published var history: [Double] = []

    @Published var tablets: [TabletInfo] = []
    @Published var speedTest: SpeedTestState = .idle

    var streaming: Bool { phase == "streaming" }
    var connecting: Bool { phase == "connecting" }

    var linkKind: LinkKind {
        guard streaming || connecting else { return .idle }
        return transport == "usb" ? .usb : .wifi
    }

    var transportLabel: String { transport == "usb" ? "USB" : "Wi-Fi" }

    var p95: Double? {
        guard history.count >= 5 else { return nil }
        let sorted = history.sorted()
        return sorted[min(sorted.count - 1, Int(Double(sorted.count) * 0.95))]
    }

    /// "stable" when the last minute stayed inside a narrow band.
    var stability: String {
        guard history.count >= 10, let lo = history.min(), let hi = history.max() else { return "measuring" }
        let spread = hi - lo
        return spread < 10 ? "stable" : (spread < 25 ? "variable" : "unstable")
    }

    func apply(state: [String: Any]) {
        let newPhase = state["phase"] as? String ?? "idle"
        if newPhase != "streaming" && phase == "streaming" {
            history.removeAll()
            latencyMs = nil
        }
        phase = newPhase
        detail = state["detail"] as? String ?? ""
        transport = state["transport"] as? String
        tabletName = state["tabletName"] as? String
        address = state["address"] as? String
        width = (state["width"] as? NSNumber)?.intValue ?? 0
        height = (state["height"] as? NSNumber)?.intValue ?? 0
        fps = (state["fps"] as? NSNumber)?.intValue ?? 0
        hidpi = (state["hidpi"] as? NSNumber)?.boolValue ?? false
        codec = state["codec"] as? String ?? "HEVC"
        usbDevice = (state["usbDevice"] as? NSNumber)?.boolValue ?? false
        activeDeviceKey = state["deviceKey"] as? String
        usbDeviceKey = state["usbDeviceKey"] as? String
        usbDeviceName = state["usbDeviceName"] as? String
    }

    func apply(stats: [String: Any]) {
        let latency = (stats["latencyMs"] as? NSNumber)?.doubleValue ?? 0
        latencyMs = latency
        fpsNow = (stats["fps"] as? NSNumber)?.doubleValue ?? 0
        targetFps = (stats["targetFps"] as? NSNumber)?.doubleValue ?? 60
        mbps = (stats["mbps"] as? NSNumber)?.doubleValue ?? 0
        capMbps = (stats["capMbps"] as? NSNumber)?.doubleValue ?? 30
        history.append(latency)
        if history.count > 60 { history.removeFirst(history.count - 60) }
    }

    func apply(tablets list: [[String: Any]]) {
        tablets = list.map {
            TabletInfo(id: $0["id"] as? String ?? "",
                       name: $0["name"] as? String ?? "Tablet",
                       paired: ($0["paired"] as? NSNumber)?.boolValue ?? false,
                       compatible: ($0["compatible"] as? NSNumber)?.boolValue ?? true,
                       current: ($0["current"] as? NSNumber)?.boolValue ?? false)
        }
    }
}

/// A tablet that has (or can have) its own stream settings.
struct SettingsDevice: Identifiable, Equatable {
    let key: String          // Wi-Fi tablet id or "usb:<serial>"
    let name: String
    let usb: Bool
    let online: Bool
    var id: String { key }
}

/// Stream settings for one tablet, persisted in DroidScreenDeviceSettings[key].
/// `deviceKey == nil` edits the global keys, used as defaults for new tablets.
/// The port is global (it is needed before we know which tablet answers).
final class SettingsStore: ObservableObject {
    static let resolutions: [(Int, String, String)] = [
        (0, "2560 × 1600", "Native"), (1, "1920 × 1200", ""), (2, "1600 × 1000", ""), (3, "1280 × 800", ""),
    ]
    static let idleOptions = [1, 10, 15, 30]
    static let maxBitrateMbps = 80
    static let devicesKey = "DroidScreenDeviceSettings"

    private let defaults = UserDefaults.standard
    private var loading = false

    @Published private(set) var deviceKey: String?
    /// Set when a value of the streaming tablet changed since the last (re)connect.
    @Published var pendingReconnect = false

    @Published var resolution = 0 { didSet { store(resolution, dev: "resolution", global: "DroidScreenResolution") } }
    @Published var retina = false { didSet { store(retina, dev: "hidpi", global: "DroidScreenScale") } }
    @Published var fps = 60 { didSet { store(fps, dev: "fps", global: "DroidScreenFPS") } }
    @Published var idleFps = 10 { didSet { store(idleFps, dev: "min_idle_fps", global: "DroidScreenMinIdleFPS") } }
    /// Mbps, 5...80 in steps of 5 (stored as kbps).
    @Published var bitrateMbps = 30 { didSet { store(bitrateMbps * 1000, dev: "bitrate", global: "DroidScreenBitrate") } }
    @Published var port: Int { didSet { if defaults.integer(forKey: "DroidScreenPort") != port { defaults.set(port, forKey: "DroidScreenPort") } } }

    /// Called with the edited device key after a change.
    var onChange: ((String?) -> Void)?

    init() {
        let p = UserDefaults.standard.integer(forKey: "DroidScreenPort")
        port = (1...65535).contains(p) ? p : 38271
        select(nil)
    }

    /// Tablets with stored settings, by key -> name (for offline ones).
    func storedDevices() -> [String: String] {
        let all = defaults.dictionary(forKey: SettingsStore.devicesKey) as? [String: [String: Any]] ?? [:]
        return all.mapValues { $0["name"] as? String ?? "Tablet" }
    }

    /// Load the values of `key` (falling back to the defaults) into the published fields.
    func select(_ key: String?) {
        loading = true
        defer { loading = false }
        deviceKey = key
        let dev = key.flatMap { (defaults.dictionary(forKey: SettingsStore.devicesKey)?[$0]) as? [String: Any] } ?? [:]
        func int(_ devKey: String, _ global: String) -> Int {
            (dev[devKey] as? NSNumber)?.intValue ?? defaults.integer(forKey: global)
        }
        resolution = min(max(int("resolution", "DroidScreenResolution"), 0), 3)
        retina = (dev["hidpi"] as? NSNumber)?.boolValue ?? defaults.bool(forKey: "DroidScreenScale")
        let f = int("fps", "DroidScreenFPS")
        fps = [30, 60, 120].contains(f) ? f : 60
        let idle = int("min_idle_fps", "DroidScreenMinIdleFPS")
        idleFps = SettingsStore.idleOptions.contains(idle) ? idle : 10
        bitrateMbps = SettingsStore.mbps(fromKbps: int("bitrate", "DroidScreenBitrate"))
    }

    /// Re-read after the core changed a value (the speed test sets the global bitrate).
    func reload() { select(deviceKey) }

    /// Drop the custom settings of a tablet so it uses the defaults again.
    func reset(_ key: String) {
        var all = defaults.dictionary(forKey: SettingsStore.devicesKey) ?? [:]
        if var dev = all[key] as? [String: Any] {
            let name = dev["name"]
            dev = [:]
            if let name { dev["name"] = name }
            all[key] = dev
            defaults.set(all, forKey: SettingsStore.devicesKey)
        }
        if deviceKey == key { select(key) }
        onChange?(key)
    }

    static func mbps(fromKbps kbps: Int) -> Int {
        min(max(Int((Double(kbps) / 5000).rounded()) * 5, 5), SettingsStore.maxBitrateMbps)
    }

    private func store(_ value: Any, dev devKey: String, global: String) {
        guard !loading else { return }
        if let key = deviceKey {
            var all = defaults.dictionary(forKey: SettingsStore.devicesKey) ?? [:]
            var dev = all[key] as? [String: Any] ?? [:]
            dev[devKey] = value
            all[key] = dev
            defaults.set(all, forKey: SettingsStore.devicesKey)
        } else {
            defaults.set(value, forKey: global)
        }
        onChange?(deviceKey)
    }
}

/// Deck shortcut apps (DroidScreenDeckApps): [{id, label, bundle_id, path}].
final class DeckAppsStore: ObservableObject {
    struct App: Identifiable, Equatable {
        var id: String
        var label: String
        var bundleId: String
        var path: String
    }

    @Published private(set) var apps: [App] = []
    var onChange: (() -> Void)?

    init() { load() }

    func load() {
        let raw = UserDefaults.standard.array(forKey: "DroidScreenDeckApps") as? [[String: Any]] ?? []
        apps = raw.map {
            App(id: $0["id"] as? String ?? UUID().uuidString,
                label: $0["label"] as? String ?? "App",
                bundleId: $0["bundle_id"] as? String ?? "",
                path: $0["path"] as? String ?? "")
        }
    }

    private func save() {
        let raw: [[String: String]] = apps.map {
            ["id": $0.id, "label": $0.label, "bundle_id": $0.bundleId, "path": $0.path]
        }
        UserDefaults.standard.set(raw, forKey: "DroidScreenDeckApps")
        onChange?()
    }

    func remove(_ app: App) {
        apps.removeAll { $0 == app }
        save()
    }

    func move(from: IndexSet, to: Int) {
        apps.move(fromOffsets: from, toOffset: to)
        save()
    }

    func add(urls: [URL]) {
        for url in urls {
            let path = url.path
            guard !apps.contains(where: { $0.path == path }) else { continue }
            let name = url.deletingPathExtension().lastPathComponent
            let bundleId = Bundle(path: path)?.bundleIdentifier ?? ""
            apps.append(App(id: "app_\(name.lowercased())", label: name, bundleId: bundleId, path: path))
        }
        save()
    }
}

// DroidScreen macOS - settings window (sidebar + panes).

import SwiftUI
import UniformTypeIdentifiers

enum SettingsPane: String, CaseIterable, Identifiable {
    case general = "General", display = "Display", devices = "Devices", deck = "Deck", about = "About"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .general: return "slider.horizontal.3"
        case .display: return "display"
        case .devices: return "ipad.landscape"
        case .deck: return "square.grid.2x2"
        case .about: return "info.circle"
        }
    }
}

final class SettingsNavigation: ObservableObject {
    @Published var pane: SettingsPane = .display
}

struct SettingsView: View {
    @ObservedObject var model: UIModel
    @ObservedObject var settings: SettingsStore
    @ObservedObject var deck: DeckAppsStore
    @ObservedObject var nav: SettingsNavigation

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Rectangle().fill(DS.line).frame(width: 1)
            VStack(spacing: 0) {
                HStack {
                    Text(nav.pane.rawValue).font(DSFont.ui(18, .semibold)).foregroundColor(DS.text)
                    Spacer()
                    if model.streaming {
                        HStack(spacing: 6) {
                            Dot(color: DS.live, size: 6)
                            Text(verbatim: "\(model.width) × \(model.height) @ \(model.fps)")
                                .font(DSFont.mono(11)).foregroundColor(DS.text2)
                        }
                    }
                }
                .padding(.horizontal, 24)
                .frame(height: 56)
                Rectangle().fill(DS.line).frame(height: 1)
                ScrollView {
                    pane.padding(.horizontal, 24).padding(.vertical, 8)
                }
                if nav.pane == .display || nav.pane == .general {
                    footer
                }
            }
            .frame(maxWidth: .infinity)
        }
        .frame(minWidth: 720, maxWidth: .infinity, minHeight: 520, maxHeight: .infinity)
        .background(DS.ink)
        .ignoresSafeArea()
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 2) {
            Spacer().frame(height: 38) // traffic lights
            ForEach(SettingsPane.allCases) { pane in
                let on = pane == nav.pane
                Button { nav.pane = pane } label: {
                    HStack(spacing: 10) {
                        Image(systemName: pane.icon).font(.system(size: 13)).frame(width: 18)
                            .foregroundColor(on ? DS.cable : DS.text2)
                        Text(pane.rawValue).font(DSFont.ui(14, on ? .medium : .regular))
                            .foregroundColor(on ? DS.text : DS.text2)
                        Spacer()
                    }
                    .padding(.horizontal, 10).frame(height: 32)
                    .background(RoundedRectangle(cornerRadius: 8).fill(on ? DS.raised : Color.clear))
                    .overlay(alignment: .leading) {
                        if on { Capsule().fill(DS.cable).frame(width: 3, height: 16).offset(x: -1) }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            Spacer()
            VStack(alignment: .leading, spacing: 6) {
                LinkMark(kind: model.linkKind, lineWidth: 1.3).frame(width: 34, height: 18)
                Text(model.streaming ? "\(model.tabletName ?? "Tablet") · \(model.transportLabel)" : "Not streaming")
                    .font(DSFont.mono(10)).foregroundColor(DS.text2).lineLimit(1)
            }
            .padding(10)
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 8)
        .frame(width: 188)
        .background(DS.surface)
    }

    @ViewBuilder private var pane: some View {
        switch nav.pane {
        case .display: DisplayPane(model: model, settings: settings)
        case .general: GeneralPane(model: model, settings: settings)
        case .devices: DevicesPane(model: model, settings: settings, nav: nav)
        case .deck: DeckPane(deck: deck)
        case .about: AboutPane()
        }
    }

    /// The settings shown belong to the tablet being streamed to.
    private var editingActive: Bool {
        model.streaming && (settings.deviceKey == nil || settings.deviceKey == model.activeDeviceKey)
    }

    private var footerText: String {
        if nav.pane == .general { return "Saved automatically" }
        return editingActive ? "Changes apply on next connection" : "Saved. Used the next time this tablet connects"
    }

    private var footer: some View {
        VStack(spacing: 0) {
            Rectangle().fill(DS.line).frame(height: 1)
            HStack(spacing: 8) {
                Image(systemName: "info.circle").font(.system(size: 12)).foregroundColor(DS.text3)
                Text(footerText)
                    .font(DSFont.ui(12)).foregroundColor(DS.text2)
                Spacer()
                if editingActive {
                    Button {
                        settings.pendingReconnect = false
                        model.host?.uiSettingsChanged(true)
                    } label: {
                        Label("Reconnect now", systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(DSButtonStyle(kind: settings.pendingReconnect ? .primary : .secondary))
                }
            }
            .padding(.horizontal, 24)
            .frame(height: 52)
        }
    }
}

/// One labelled row: label column on the left, control + helper on the right.
struct FormRow<Control: View>: View {
    var label: String
    var help: String? = nil
    @ViewBuilder var control: Control

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 16) {
                Text(label).font(DSFont.ui(14)).foregroundColor(DS.text)
                    .frame(width: 140, alignment: .leading)
                    .padding(.top, 5)
                VStack(alignment: .leading, spacing: 6) {
                    control
                    if let help {
                        Text(help).font(DSFont.ui(12)).foregroundColor(DS.text2)
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(.vertical, 12)
            Rectangle().fill(DS.line.opacity(0.7)).frame(height: 1)
        }
    }
}

/// Every tablet that can have settings: streaming, plugged in, on the network, or remembered.
func settingsDevices(model: UIModel, settings: SettingsStore) -> [SettingsDevice] {
    var out: [SettingsDevice] = []
    func add(_ key: String?, _ name: String?, usb: Bool, online: Bool) {
        guard let key, !out.contains(where: { $0.key == key }) else { return }
        out.append(SettingsDevice(key: key, name: name ?? "Tablet", usb: usb, online: online))
    }
    if model.streaming { add(model.activeDeviceKey, model.tabletName, usb: model.transport == "usb", online: true) }
    if model.usbDevice { add(model.usbDeviceKey, model.usbDeviceName, usb: true, online: true) }
    for t in model.tablets { add(t.id, t.name, usb: false, online: true) }
    for (key, name) in settings.storedDevices().sorted(by: { $0.value < $1.value }) {
        add(key, name, usb: key.hasPrefix("usb:"), online: false)
    }
    return out
}

struct DevicePicker: View {
    @ObservedObject var model: UIModel
    @ObservedObject var settings: SettingsStore

    var body: some View {
        let devices = settingsDevices(model: model, settings: settings)
        VStack(alignment: .leading, spacing: 8) {
            Eyebrow(text: "Settings for")
            HStack(spacing: 10) {
                Button {
                    var items: [(String, String?)] = devices.map { (menuTitle($0), $0.key) }
                    items.append(("", nil))   // separator
                    items.append(("Defaults for new tablets", "__defaults__"))
                    PopupMenu.show(items: items, selected: settings.deviceKey ?? "__defaults__") { key in
                        settings.select(key == "__defaults__" ? nil : key)
                    }
                } label: {
                    current(devices)
                }
                .buttonStyle(.plain)
                .fixedSize()
                Spacer()
                if let key = settings.deviceKey {
                    Button("Use defaults") { settings.reset(key) }
                        .buttonStyle(DSButtonStyle(kind: .quiet, height: 26))
                }
            }
            Text(caption(devices)).font(DSFont.ui(12)).foregroundColor(DS.text2)
        }
        .padding(.top, 14).padding(.bottom, 4)
    }

    private func caption(_ devices: [SettingsDevice]) -> String {
        guard let key = settings.deviceKey, let d = devices.first(where: { $0.key == key }) else {
            return "Used by tablets you haven't customized yet."
        }
        return "Used whenever \(d.name) connects over \(d.usb ? "USB" : "Wi-Fi")."
    }

    private func menuTitle(_ d: SettingsDevice) -> String {
        var t = "\(d.name) · \(d.usb ? "USB" : "Wi-Fi")"
        if model.streaming && d.key == model.activeDeviceKey { t += " · streaming" } else if !d.online { t += " · offline" }
        return t
    }

    /// The menu's face: icon, name, live dot, chevron, in a yellow-edged pill.
    private func current(_ devices: [SettingsDevice]) -> some View {
        let d = devices.first(where: { $0.key == settings.deviceKey })
        let live = d != nil && model.streaming && d?.key == model.activeDeviceKey
        return HStack(spacing: 8) {
            Image(systemName: d == nil ? "square.stack" : (d!.usb ? "cable.connector" : "wifi"))
                .font(.system(size: 12, weight: .medium)).foregroundColor(DS.cable)
            Text(verbatim: d?.name ?? "Defaults for new tablets")
                .font(DSFont.ui(14, .medium)).foregroundColor(DS.text)
            if live { Dot(color: DS.live, size: 6) }
            Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold)).foregroundColor(DS.text2)
        }
        .padding(.horizontal, 12).frame(height: 32)
        .background(Capsule().fill(DS.surface))
        .overlay(Capsule().stroke(DS.cable.opacity(0.45), lineWidth: 1))
    }
}

struct DisplayPane: View {
    @ObservedObject var model: UIModel
    @ObservedObject var settings: SettingsStore

    var body: some View {
        VStack(spacing: 0) {
            DevicePicker(model: model, settings: settings)
            FormRow(label: "Resolution") {
                ResolutionPicker(selection: $settings.resolution)
            }
            FormRow(label: "Retina (HiDPI)", help: "Sharper text, same workspace size") {
                CableToggle(isOn: $settings.retina)
            }
            FormRow(label: "Frame rate", help: fpsHelp) {
                Segmented(options: [(30, "30"), (60, "60"), (120, "120")], selection: $settings.fps)
            }
            FormRow(label: "Still-screen refresh", help: "Keeps the tablet responsive while saving battery") {
                Segmented(options: SettingsStore.idleOptions.map { ($0, "\($0)") }, selection: $settings.idleFps)
            }
            FormRow(label: "Bitrate cap", help: "Adapts down automatically on busy networks") {
                TickSlider(value: $settings.bitrateMbps, range: 5...SettingsStore.maxBitrateMbps, step: 5,
                           labels: [5, 20, 40, 60, 80], unit: "Mbps")
            }
        }
    }

    private var fpsHelp: String {
        switch settings.fps {
        case 120: return "120 fps works best over USB"
        case 30: return "Smoothest on crowded networks"
        default: return "60 fps recommended over Wi-Fi"
        }
    }
}

struct ResolutionPicker: View {
    @Binding var selection: Int
    var body: some View {
        HStack(spacing: 2) {
            ForEach(SettingsStore.resolutions, id: \.0) { item in
                let on = item.0 == selection
                Button { selection = item.0 } label: {
                    VStack(spacing: 0) {
                        Text(verbatim: item.1.replacingOccurrences(of: " × ", with: "×"))
                            .font(DSFont.mono(11, on ? .medium : .regular))
                            .lineLimit(1).fixedSize()
                        Text(item.2.isEmpty ? " " : item.2.uppercased())
                            .font(DSFont.mono(8, .medium)).opacity(0.7)
                    }
                    .foregroundColor(on ? DS.ink : DS.text2)
                    .padding(.horizontal, 8).frame(height: 34)
                    .background(RoundedRectangle(cornerRadius: 8).fill(on ? DS.cable : Color.clear))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(RoundedRectangle(cornerRadius: DS.radiusControl + 1).fill(DS.ink))
        .overlay(RoundedRectangle(cornerRadius: DS.radiusControl + 1).stroke(DS.line, lineWidth: 1))
    }
}

/// Snapping slider with tick labels; the fill and knob mark use the cable color.
struct TickSlider: View {
    @Binding var value: Int
    var range: ClosedRange<Int>
    var step: Int
    /// Values printed under the track (all stops when empty).
    var labels: [Int] = []
    var unit: String

    private var stops: [Int] {
        labels.isEmpty ? Array(stride(from: range.lowerBound, through: range.upperBound, by: step)) : labels
    }

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            VStack(spacing: 4) {
                GeometryReader { geo in
                    let w = geo.size.width
                    let t = CGFloat(value - range.lowerBound) / CGFloat(range.upperBound - range.lowerBound)
                    ZStack(alignment: .leading) {
                        Capsule().fill(DS.raised).frame(height: 4)
                        Capsule().fill(DS.cable).frame(width: max(4, w * t), height: 4)
                        Circle().fill(DS.cable).frame(width: 16, height: 16)
                            .overlay(Circle().stroke(DS.ink, lineWidth: 3))
                            .offset(x: w * t - 8)
                    }
                    .frame(height: 20)
                    .contentShape(Rectangle())
                    .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                        let f = min(max(g.location.x / w, 0), 1)
                        let raw = Double(range.lowerBound) + Double(f) * Double(range.upperBound - range.lowerBound)
                        let snapped = Int((raw / Double(step)).rounded()) * step
                        let v = min(max(snapped, range.lowerBound), range.upperBound)
                        if v != value { value = v }
                    })
                }
                .frame(height: 20)
                GeometryReader { geo in
                    ForEach(stops, id: \.self) { s in
                        let t = CGFloat(s - range.lowerBound) / CGFloat(range.upperBound - range.lowerBound)
                        Text(verbatim: "\(s)").font(DSFont.mono(10))
                            .foregroundColor(s == value ? DS.cable : DS.text3)
                            .fixedSize()
                            .position(x: min(max(geo.size.width * t, 6), geo.size.width - 8), y: 7)
                    }
                }
                .frame(height: 14)
            }
            .frame(width: 240)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(verbatim: "\(value)").font(DSFont.mono(16, .medium)).foregroundColor(DS.text)
                Text(unit.uppercased()).font(DSFont.mono(9)).foregroundColor(DS.text3)
            }
        }
    }
}

struct GeneralPane: View {
    @ObservedObject var model: UIModel
    @ObservedObject var settings: SettingsStore
    @State private var portText = ""

    var body: some View {
        VStack(spacing: 0) {
            FormRow(label: "Measure link", help: speedHelp) {
                HStack(spacing: 10) {
                    Button {
                        model.speedTest = .running
                        model.host?.uiRunSpeedTest()
                    } label: {
                        Label(model.speedTest == .running ? "Measuring…" : "Run speed test", systemImage: "gauge.with.dots.needle.50percent")
                    }
                    .buttonStyle(DSButtonStyle())
                    .disabled(model.speedTest == .running)
                    if case .done(let t, let s) = model.speedTest {
                        Text(String(format: "%.0f Mbps → cap %d", Double(t) / 1000, s / 1000))
                            .font(DSFont.mono(12)).foregroundColor(DS.text)
                    } else if model.speedTest == .failed {
                        Text("Couldn't reach the tablet").font(DSFont.ui(12)).foregroundColor(DS.error)
                    }
                }
            }
            FormRow(label: "Port", help: "TCP port the tablet listens on (USB and Wi-Fi)") {
                TextField("38271", text: $portText, onCommit: commitPort)
                    .textFieldStyle(.plain)
                    .font(DSFont.mono(13))
                    .foregroundColor(DS.text)
                    .padding(.horizontal, 10).frame(width: 100, height: 28)
                    .background(RoundedRectangle(cornerRadius: DS.radiusControl).fill(DS.surface))
                    .overlay(RoundedRectangle(cornerRadius: DS.radiusControl).stroke(DS.line, lineWidth: 1))
                    .onAppear { portText = "\(settings.port)" }
            }
            FormRow(label: "Priority", help: "A plugged-in tablet always wins; paired Wi-Fi tablets connect on their own when no cable is attached.") {
                HStack(spacing: 8) {
                    Label("USB", systemImage: "cable.connector").font(DSFont.ui(13)).foregroundColor(DS.text)
                    Image(systemName: "chevron.right").font(.system(size: 10)).foregroundColor(DS.text3)
                    Label("Paired Wi-Fi", systemImage: "wifi").font(DSFont.ui(13)).foregroundColor(DS.text)
                }
                .padding(.top, 4)
            }
        }
    }

    private var speedHelp: String {
        "Streams test data for 2 s and sets the bitrate cap to about 70% of what the link sustains. Pauses the stream while it runs."
    }

    private func commitPort() {
        if let p = Int(portText), (1...65535).contains(p) {
            settings.port = p
        } else {
            portText = "\(settings.port)"
        }
    }
}

struct DevicesPane: View {
    @ObservedObject var model: UIModel
    @ObservedObject var settings: SettingsStore
    @ObservedObject var nav: SettingsNavigation

    private func openSettings(_ key: String?) -> some View {
        Button {
            settings.select(key)
            nav.pane = .display
        } label: {
            Image(systemName: "slider.horizontal.3")
        }
        .buttonStyle(DSButtonStyle(kind: .secondary, height: 28))
        .help("Stream settings for this tablet")
        .disabled(key == nil)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Tablets running DroidScreen on this network. Pairing happens once: approve the Mac on the tablet and it's remembered on both sides.")
                .font(DSFont.ui(12)).foregroundColor(DS.text2)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.vertical, 10)
            if model.tablets.isEmpty && !model.usbDevice {
                Card(padding: 16) {
                    HStack(spacing: 12) {
                        LinkMark(kind: .idle).frame(width: 40, height: 22)
                        Text("No tablets found yet. Open DroidScreen on the tablet, on the same Wi-Fi.")
                            .font(DSFont.ui(13)).foregroundColor(DS.text2)
                    }
                }
            }
            if model.usbDevice {
                deviceCard(name: model.usbDeviceName ?? "USB tablet",
                           detail: "USB · no pairing needed", icon: "cable.connector",
                           current: model.streaming && model.transport == "usb") {
                    openSettings(model.usbDeviceKey)
                }
            }
            ForEach(model.tablets) { t in
                deviceCard(name: t.name,
                           detail: !t.compatible ? "Wi-Fi · update the Android app" : (t.paired ? "Wi-Fi · paired" : "Wi-Fi · not paired"),
                           icon: "wifi", current: t.current) {
                    openSettings(t.id)
                    if t.paired {
                        Button("Forget") { model.host?.uiForgetTablet(t.id) }
                            .buttonStyle(DSButtonStyle(kind: .quiet, height: 28))
                    }
                    if !t.current && t.compatible {
                        Button(t.paired ? "Connect" : "Pair") { model.host?.uiConnectWifiTablet(t.id) }
                            .buttonStyle(DSButtonStyle(kind: t.paired ? .secondary : .primary, height: 28))
                    }
                }
            }
        }
    }

    private func deviceCard<Actions: View>(name: String, detail: String, icon: String, current: Bool,
                                           @ViewBuilder actions: () -> Actions) -> some View {
        Card(padding: 12) {
            HStack(spacing: 12) {
                Image(systemName: icon).font(.system(size: 13, weight: .medium))
                    .foregroundColor(current ? DS.ink : DS.text2)
                    .frame(width: 32, height: 32)
                    .background(RoundedRectangle(cornerRadius: 9).fill(current ? DS.cable : DS.raised))
                VStack(alignment: .leading, spacing: 2) {
                    Text(name).font(DSFont.ui(14, .medium)).foregroundColor(DS.text)
                    Text(current ? detail.replacingOccurrences(of: "paired", with: "streaming") : detail)
                        .font(DSFont.ui(12)).foregroundColor(DS.text2)
                }
                Spacer()
                actions()
            }
        }
    }
}

struct DeckPane: View {
    @ObservedObject var deck: DeckAppsStore

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("App shortcuts shown on the tablet's Deck, next to Now Playing and Volume.")
                .font(DSFont.ui(12)).foregroundColor(DS.text2)
                .padding(.top, 10)
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 4), spacing: 10) {
                ForEach(deck.apps) { app in
                    DeckAppTile(app: app) { deck.remove(app) }
                }
                Button(action: addApps) {
                    VStack(spacing: 6) {
                        Image(systemName: "plus").font(.system(size: 16, weight: .medium))
                        Text("Add app").font(DSFont.ui(12))
                    }
                    .foregroundColor(DS.text2)
                    .frame(maxWidth: .infinity).frame(height: 92)
                    .background(RoundedRectangle(cornerRadius: DS.radiusCard).stroke(DS.line, style: StrokeStyle(lineWidth: 1, dash: [4, 3])))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func addApps() {
        let panel = NSOpenPanel()
        panel.title = "Choose apps for the Deck"
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.directoryURL = URL(fileURLWithPath: "/Applications")
        panel.allowedContentTypes = [UTType.applicationBundle]
        panel.appearance = NSAppearance(named: .darkAqua)
        if panel.runModal() == .OK {
            deck.add(urls: panel.urls)
        }
    }
}

struct DeckAppTile: View {
    var app: DeckAppsStore.App
    var remove: () -> Void
    @State private var hover = false

    var body: some View {
        VStack(spacing: 8) {
            Image(nsImage: NSWorkspace.shared.icon(forFile: app.path))
                .resizable().frame(width: 36, height: 36)
            Text(app.label).font(DSFont.ui(12)).foregroundColor(DS.text).lineLimit(1)
        }
        .frame(maxWidth: .infinity).frame(height: 92)
        .background(RoundedRectangle(cornerRadius: DS.radiusCard).fill(DS.surface))
        .overlay(RoundedRectangle(cornerRadius: DS.radiusCard).stroke(hover ? DS.text3 : DS.line, lineWidth: 1))
        .overlay(alignment: .topTrailing) {
            if hover {
                Button(action: remove) {
                    Image(systemName: "xmark").font(.system(size: 9, weight: .bold)).foregroundColor(DS.text)
                        .frame(width: 18, height: 18).background(Circle().fill(DS.raised))
                        .overlay(Circle().stroke(DS.line, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .padding(6)
            }
        }
        .onHover { hover = $0 }
    }
}

struct AboutPane: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            LinkMark(kind: .wifi, lineWidth: 2).frame(width: 120, height: 62).padding(.top, 24)
            Text("DroidScreen").font(DSFont.ui(24, .semibold)).foregroundColor(DS.text)
            Text("Version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "–")")
                .font(DSFont.mono(12)).foregroundColor(DS.text2)
            Text("Turns an Android tablet into a second display for this Mac, over USB or Wi-Fi.")
                .font(DSFont.ui(13)).foregroundColor(DS.text2)
            Text("The Wi-Fi stream is not encrypted. Use it on networks you trust.")
                .font(DSFont.ui(12)).foregroundColor(DS.warn)
        }
    }
}

/// Native NSMenu popped at the mouse, with closure callbacks.
final class PopupMenu: NSObject {
    private static var active: PopupMenu?
    private let onPick: (String) -> Void

    private init(onPick: @escaping (String) -> Void) { self.onPick = onPick }

    /// `items`: (title, key); a nil key inserts a separator.
    static func show(items: [(String, String?)], selected: String?, onPick: @escaping (String) -> Void) {
        let handler = PopupMenu(onPick: onPick)
        let menu = NSMenu()
        menu.appearance = NSAppearance(named: .darkAqua)
        for (title, key) in items {
            guard let key else { menu.addItem(.separator()); continue }
            let item = NSMenuItem(title: title, action: #selector(pick(_:)), keyEquivalent: "")
            item.target = handler
            item.representedObject = key
            item.state = key == selected ? .on : .off
            menu.addItem(item)
        }
        active = handler
        menu.popUp(positioning: nil, at: NSEvent.mouseLocation, in: nil)
        active = nil
    }

    @objc private func pick(_ sender: NSMenuItem) {
        if let key = sender.representedObject as? String { onPick(key) }
    }
}

// DroidScreen macOS - menu bar popover.

import SwiftUI

struct PopoverView: View {
    @ObservedObject var model: UIModel
    var openSettings: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            LinkStrip(model: model)
            if model.streaming {
                latencyCard
                HStack(spacing: 8) {
                    MiniStat(title: "Frame rate", value: String(format: "%.0f", model.fpsNow), unit: "fps",
                             sub: String(format: "target %d", Int(model.targetFps)),
                             fraction: model.targetFps > 0 ? model.fpsNow / model.targetFps : 0)
                    MiniStat(title: "Bitrate", value: String(format: "%.1f", model.mbps), unit: "Mbps",
                             sub: String(format: "cap %d", Int(model.capMbps)),
                             fraction: model.capMbps > 0 ? model.mbps / model.capMbps : 0)
                }
                HStack {
                    Eyebrow(text: "Display")
                    Spacer()
                    Text(displayLine).font(DSFont.mono(11)).foregroundColor(DS.text)
                }
                .padding(.horizontal, 2)
            } else {
                idleCard
            }
            tabletsSection
            actions
        }
        .padding(14)
        .frame(width: 340)
        .fixedSize(horizontal: false, vertical: true)
        .background(DS.ink)
    }

    private var displayLine: String {
        var parts = ["\(model.width) × \(model.height)"]
        if model.hidpi { parts.append("Retina") }
        parts.append(model.codec)
        return parts.joined(separator: " · ")
    }

    private var header: some View {
        HStack(spacing: 10) {
            LinkMark(kind: model.linkKind, lineWidth: 1.4)
                .frame(width: 30, height: 16)
            VStack(alignment: .leading, spacing: 1) {
                Text("DroidScreen").font(DSFont.ui(14, .semibold)).foregroundColor(DS.text)
                Text(statusLine).font(DSFont.ui(12)).foregroundColor(DS.text2).lineLimit(1)
            }
            Spacer()
            StatusPill(model: model)
        }
    }

    private var statusLine: String {
        if model.streaming, let name = model.tabletName { return "Streaming to \(name)" }
        return model.detail
    }

    private var latencyCard: some View {
        Card {
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .bottom) {
                    VStack(alignment: .leading, spacing: 2) {
                        Eyebrow(text: "Latency")
                        HStack(alignment: .firstTextBaseline, spacing: 4) {
                            Text(model.latencyMs.map { String(format: "%.0f", $0) } ?? "–")
                                .font(DSFont.mono(40, .medium))
                                .foregroundColor(DS.text)
                            Text("ms").font(DSFont.mono(12)).foregroundColor(DS.text3)
                        }
                    }
                    Spacer()
                    VStack(alignment: .trailing, spacing: 3) {
                        HStack(spacing: 4) {
                            Text("p95").foregroundColor(DS.text2)
                            Text(model.p95.map { String(format: "%.0f", $0) } ?? "–").foregroundColor(DS.text)
                            Text("ms").foregroundColor(DS.text2)
                        }
                        HStack(spacing: 5) {
                            Dot(color: DS.latencyColor(model.p95 ?? model.latencyMs ?? 0), size: 5)
                            Text("60 s · \(model.stability)").foregroundColor(DS.text2)
                        }
                    }
                    .font(DSFont.mono(11))
                    .padding(.bottom, 6)
                }
                LatencyMeter(ms: model.latencyMs)
                Sparkline(values: model.history)
                    .frame(height: 26)
            }
        }
    }

    private var idleCard: some View {
        Card(padding: 14) {
            VStack(alignment: .leading, spacing: 10) {
                Text(model.connecting ? "Connecting…" : "Not streaming")
                    .font(DSFont.ui(15, .semibold)).foregroundColor(DS.text)
                Text(idleHint).font(DSFont.ui(12)).foregroundColor(DS.text2)
                    .fixedSize(horizontal: false, vertical: true)
                if !model.connecting {
                    Button("Connect") { model.host?.uiToggleConnect() }
                        .buttonStyle(DSButtonStyle(kind: .primary, height: 32))
                }
            }
        }
    }

    private var idleHint: String {
        if model.connecting { return model.detail }
        if model.usbDevice { return "A tablet is plugged in over USB. Open DroidScreen on it, then connect." }
        if model.tablets.isEmpty { return "Open DroidScreen on your tablet. It shows up here on the same Wi-Fi, or plug in a USB cable." }
        return model.detail == "Disconnected" || model.detail.isEmpty ? "Pick a tablet below to start." : model.detail
    }

    private var tabletsSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Eyebrow(text: "Tablets")
                Rectangle().fill(DS.line).frame(height: 1)
            }
            .padding(.bottom, 2)
            if model.usbDevice {
                TabletRow(icon: "cable.connector", name: model.usbDeviceName ?? "USB tablet",
                          sub: model.streaming && model.transport == "usb" ? "USB · connected" : "USB · plugged in",
                          current: model.streaming && model.transport == "usb",
                          action: usbAction)
            }
            ForEach(model.tablets) { tablet in
                TabletRow(icon: "wifi", name: tablet.name, sub: sub(for: tablet), current: tablet.current,
                          action: action(for: tablet))
            }
            if !model.usbDevice && model.tablets.isEmpty {
                Text("Searching on Wi-Fi…").font(DSFont.ui(12)).foregroundColor(DS.text3).padding(.vertical, 4)
            }
        }
    }

    private var usbAction: RowAction? {
        if model.streaming && model.transport == "usb" { return nil }
        let host = model.host
        return RowAction(title: "Connect") { host?.uiToggleConnect() }
    }

    private func sub(for tablet: TabletInfo) -> String {
        if !tablet.compatible { return "Wi-Fi · update the Android app" }
        if tablet.current { return "Wi-Fi · connected" }
        return tablet.paired ? "Wi-Fi · paired" : "Wi-Fi · not paired"
    }

    private func action(for tablet: TabletInfo) -> RowAction? {
        guard tablet.compatible, !tablet.current else { return nil }
        let id = tablet.id
        let host = model.host
        return RowAction(title: tablet.paired ? "Connect" : "Pair") { host?.uiConnectWifiTablet(id) }
    }

    private var actions: some View {
        VStack(spacing: 0) {
            Rectangle().fill(DS.line).frame(height: 1).padding(.bottom, 4)
            if model.streaming || model.connecting {
                ActionRow(title: model.connecting ? "Cancel" : "Disconnect", icon: "xmark.circle", destructive: true) {
                    model.host?.uiToggleConnect()
                }
            }
            ActionRow(title: "Settings…", icon: "gearshape", shortcut: "⌘,") { openSettings() }
            ActionRow(title: "Quit DroidScreen", icon: "power", shortcut: "⌘Q") { model.host?.uiQuit() }
        }
    }
}

/// The signature: Mac — cable — tablet, with the transport and latency on the line.
struct LinkStrip: View {
    @ObservedObject var model: UIModel

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "laptopcomputer").font(.system(size: 15, weight: .regular)).foregroundColor(DS.text)
            ZStack {
                CableLine(kind: model.linkKind).frame(height: 2)
                Text(label)
                    .font(DSFont.mono(11, .medium))
                    .foregroundColor(model.linkKind == .idle ? DS.text3 : DS.ink)
                    .padding(.horizontal, 8).padding(.vertical, 2)
                    .background(Capsule().fill(model.linkKind == .idle ? DS.surface : DS.cable))
                    .overlay(Capsule().stroke(model.linkKind == .idle ? DS.line : Color.clear, lineWidth: 1))
            }
            HStack(spacing: 5) {
                Image(systemName: "ipad.landscape").font(.system(size: 14)).foregroundColor(DS.text)
                Text(model.tabletName ?? "Tablet")
                    .font(DSFont.ui(12, .medium))
                    .foregroundColor(model.linkKind == .idle ? DS.text3 : DS.text)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 12)
        .frame(height: 44)
        .background(RoundedRectangle(cornerRadius: DS.radiusCard).fill(DS.surface))
        .overlay(RoundedRectangle(cornerRadius: DS.radiusCard).stroke(DS.line, lineWidth: 1))
    }

    private var label: String {
        if model.connecting { return "\(model.transportLabel) · …" }
        guard model.streaming else { return "not linked" }
        let ms = model.latencyMs.map { String(format: " · %.0f ms", $0) } ?? ""
        return model.transportLabel + ms
    }
}

struct CableLine: View {
    var kind: LinkKind
    var body: some View {
        Canvas { ctx, size in
            var p = Path()
            p.move(to: CGPoint(x: 0, y: size.height / 2))
            p.addLine(to: CGPoint(x: size.width, y: size.height / 2))
            var style = StrokeStyle(lineWidth: size.height, lineCap: .round)
            switch kind {
            case .usb: break
            case .wifi: style.dash = [7, 4]
            case .idle: style.dash = [0.1, 5]
            }
            ctx.stroke(p, with: .color(kind == .idle ? DS.text3 : DS.cable), style: style)
        }
    }
}

struct StatusPill: View {
    @ObservedObject var model: UIModel
    var body: some View {
        let (text, color): (String, Color) = model.streaming ? ("Live", DS.live)
            : (model.connecting ? ("Linking", DS.warn) : ("Idle", DS.text3))
        HStack(spacing: 5) {
            Dot(color: color, size: 6)
            Text(text).font(DSFont.mono(10, .medium)).foregroundColor(DS.text2)
        }
        .padding(.horizontal, 8).frame(height: 22)
        .background(Capsule().fill(DS.surface))
        .overlay(Capsule().stroke(DS.line, lineWidth: 1))
    }
}

struct MiniStat: View {
    var title: String
    var value: String
    var unit: String
    var sub: String
    var fraction: Double

    var body: some View {
        Card(padding: 10) {
            VStack(alignment: .leading, spacing: 4) {
                Eyebrow(text: title)
                HStack(alignment: .firstTextBaseline, spacing: 3) {
                    Text(value).font(DSFont.mono(20, .medium)).foregroundColor(DS.text)
                    Text(unit).font(DSFont.mono(10)).foregroundColor(DS.text3)
                }
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule().fill(DS.raised)
                        Capsule().fill(DS.text2).frame(width: geo.size.width * CGFloat(min(max(fraction, 0), 1)))
                    }
                }
                .frame(height: 3)
                Text(sub).font(DSFont.mono(10)).foregroundColor(DS.text3)
            }
        }
    }
}

struct RowAction {
    var title: String
    var perform: () -> Void
}

struct TabletRow: View {
    var icon: String
    var name: String
    var sub: String
    var current: Bool
    var action: RowAction?
    @State private var hover = false

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(current ? DS.ink : DS.text2)
                .frame(width: 26, height: 26)
                .background(RoundedRectangle(cornerRadius: 8).fill(current ? DS.cable : DS.raised))
            VStack(alignment: .leading, spacing: 1) {
                Text(name).font(DSFont.ui(13, .medium)).foregroundColor(DS.text).lineLimit(1)
                Text(sub).font(DSFont.ui(11)).foregroundColor(DS.text2).lineLimit(1)
            }
            Spacer()
            if current {
                Text("CURRENT").font(DSFont.mono(9, .medium)).foregroundColor(DS.cable)
                    .padding(.horizontal, 6).padding(.vertical, 3)
                    .overlay(Capsule().stroke(DS.cable.opacity(0.5), lineWidth: 1))
            } else if let action {
                Button(action.title, action: action.perform).buttonStyle(DSButtonStyle(kind: .secondary, height: 26))
            }
        }
        .padding(.horizontal, 6).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: DS.radiusControl).fill(hover ? DS.surface : Color.clear))
        .onHover { hover = $0 }
    }
}

struct ActionRow: View {
    var title: String
    var icon: String
    var shortcut: String? = nil
    var destructive = false
    var perform: () -> Void
    @State private var hover = false

    var body: some View {
        Button(action: perform) {
            HStack(spacing: 10) {
                Image(systemName: icon).font(.system(size: 13)).frame(width: 18)
                    .foregroundColor(destructive ? DS.error : DS.text2)
                Text(title).font(DSFont.ui(13)).foregroundColor(destructive ? DS.error : DS.text)
                Spacer()
                if let shortcut {
                    Text(shortcut).font(DSFont.mono(11)).foregroundColor(DS.text3)
                }
            }
            .padding(.horizontal, 8).frame(height: 30)
            .background(RoundedRectangle(cornerRadius: 8).fill(hover ? DS.surface : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
    }
}

// DroidScreen macOS - "Link Line" design tokens, fonts and the link mark.

import AppKit
import CoreText
import SwiftUI

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: opacity)
    }
}

extension NSColor {
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
                  green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255,
                  alpha: alpha)
    }
}

/// Link Line palette. Ink surfaces, warm white text, one signal-yellow "cable".
enum DS {
    static let ink = Color(hex: 0x0F1115)
    static let surface = Color(hex: 0x171A20)
    static let raised = Color(hex: 0x1F232B)
    static let hover = Color(hex: 0x252A33)
    static let line = Color(hex: 0x2A2F39)
    static let text = Color(hex: 0xF3F1EA)
    static let text2 = Color(hex: 0xA8ABB3)
    static let text3 = Color(hex: 0x6C707A)
    static let cable = Color(hex: 0xF2D13A)
    static let cableTint = Color(hex: 0xF2D13A, opacity: 0.12)
    static let live = Color(hex: 0x5BD68A)
    static let warn = Color(hex: 0xF2994A)
    static let error = Color(hex: 0xEB5757)

    static let nsInk = NSColor(hex: 0x0F1115)
    static let nsCable = NSColor(hex: 0xF2D13A)

    static let radiusCard: CGFloat = 14
    static let radiusControl: CGFloat = 10

    /// Latency zones shared by the meter and the numbers: good / fair / poor.
    static func latencyColor(_ ms: Double) -> Color {
        ms <= 40 ? live : (ms <= 70 ? warn : error)
    }
}

enum DSFont {
    private static var registered = false

    /// Registers the bundled Instrument Sans / IBM Plex Mono (Resources/Fonts).
    /// Missing files just fall back to the system fonts.
    static func registerBundled() {
        guard !registered else { return }
        registered = true
        let urls = (Bundle.main.urls(forResourcesWithExtension: "ttf", subdirectory: "Fonts") ?? [])
        for url in urls {
            CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
        }
    }

    static func ui(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        Font.custom("Instrument Sans", size: size).weight(weight)
    }

    static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        Font.custom(weight == .regular ? "IBMPlexMono-Regular" : "IBMPlexMono-Medium", size: size)
            .monospacedDigit()
    }
}

enum LinkKind {
    case wifi, usb, idle
}

/// The brand mark: a laptop and a landscape tablet joined by the cable.
/// Wi-Fi draws the cable dashed, USB solid, idle dotted and grey.
struct LinkMark: View {
    var kind: LinkKind
    var outline: Color = DS.text
    var cable: Color = DS.cable
    var lineWidth: CGFloat = 1.5

    var body: some View {
        Canvas { ctx, size in
            LinkMark.draw(in: &ctx, size: size, kind: kind, outline: outline,
                          cable: kind == .idle ? DS.text3 : cable, lineWidth: lineWidth)
        }
    }

    static func draw(in ctx: inout GraphicsContext, size: CGSize, kind: LinkKind,
                     outline: Color, cable: Color, lineWidth: CGFloat) {
        let w = size.width, h = size.height
        let stroke = StrokeStyle(lineWidth: lineWidth, lineCap: .round, lineJoin: .round)

        // Laptop: screen + base.
        let screen = CGRect(x: w * 0.04, y: h * 0.16, width: w * 0.26, height: h * 0.52)
        ctx.stroke(Path(roundedRect: screen, cornerRadius: max(1.5, h * 0.06)), with: .color(outline), style: stroke)
        var base = Path()
        base.move(to: CGPoint(x: 0, y: h * 0.82))
        base.addLine(to: CGPoint(x: w * 0.34, y: h * 0.82))
        ctx.stroke(base, with: .color(outline), style: stroke)

        // Tablet (landscape).
        let tablet = CGRect(x: w * 0.66, y: h * 0.22, width: w * 0.32, height: h * 0.56)
        ctx.stroke(Path(roundedRect: tablet, cornerRadius: max(1.5, h * 0.08)), with: .color(outline), style: stroke)

        // Cable.
        var cablePath = Path()
        let y = h * 0.47
        cablePath.move(to: CGPoint(x: w * 0.36, y: y))
        cablePath.addLine(to: CGPoint(x: w * 0.64, y: y))
        var cableStroke = StrokeStyle(lineWidth: lineWidth * 1.15, lineCap: .round)
        switch kind {
        case .usb: break
        case .wifi: cableStroke.dash = [max(2, w * 0.05), max(1.6, w * 0.035)]
        case .idle: cableStroke.dash = [0.1, max(2, w * 0.045)]
        }
        ctx.stroke(cablePath, with: .color(cable), style: cableStroke)
    }
}

/// Small round status dot.
struct Dot: View {
    var color: Color
    var size: CGFloat = 6
    var body: some View {
        Circle().fill(color).frame(width: size, height: size)
    }
}

/// Uppercase micro label ("LATENCY").
struct Eyebrow: View {
    var text: String
    var body: some View {
        Text(text.uppercased())
            .font(DSFont.mono(10, .medium))
            .tracking(0.8)
            .foregroundColor(DS.text3)
    }
}

/// Card with a 1px line border instead of shadows.
struct Card<Content: View>: View {
    var padding: CGFloat = 12
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: DS.radiusCard).fill(DS.surface))
            .overlay(RoundedRectangle(cornerRadius: DS.radiusCard).stroke(DS.line, lineWidth: 1))
    }
}

enum DSButtonKind {
    case primary, secondary, quiet
}

struct DSButtonStyle: ButtonStyle {
    var kind: DSButtonKind = .secondary
    var height: CGFloat = 30

    func makeBody(configuration: Configuration) -> some View {
        let pressed = configuration.isPressed
        return configuration.label
            .font(DSFont.ui(13, .medium))
            .padding(.horizontal, 12)
            .frame(minHeight: height)
            .foregroundColor(kind == .primary ? DS.ink : DS.text)
            .background(
                RoundedRectangle(cornerRadius: DS.radiusControl)
                    .fill(kind == .primary ? DS.cable.opacity(pressed ? 0.8 : 1)
                          : (kind == .quiet ? Color.clear : (pressed ? DS.hover : DS.raised)))
            )
            .overlay(
                RoundedRectangle(cornerRadius: DS.radiusControl)
                    .stroke(kind == .secondary ? DS.line : Color.clear, lineWidth: 1)
            )
            .contentShape(Rectangle())
    }
}

/// Segmented control: ink well, the selected segment is a yellow pill with ink text.
struct Segmented<T: Hashable>: View {
    var options: [(T, String)]
    @Binding var selection: T
    var mono = true
    var height: CGFloat = 28
    /// Options shown but not selectable.
    var disabled: Set<T> = []

    var body: some View {
        HStack(spacing: 2) {
            ForEach(options.indices, id: \.self) { i in
                let (value, label) = options[i]
                let on = value == selection
                let off = disabled.contains(value)
                Button { if !off { selection = value } } label: {
                    Text(label)
                        .font(mono ? DSFont.mono(12, on ? .medium : .regular) : DSFont.ui(13, .medium))
                        .foregroundColor(on ? DS.ink : (off ? DS.text3.opacity(0.6) : DS.text2))
                        .strikethrough(off && !on, color: DS.text3.opacity(0.6))
                        .padding(.horizontal, 10)
                        .frame(minHeight: height - 6)
                        .background(Capsule().fill(on ? DS.cable : Color.clear))
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(Capsule().fill(DS.ink))
        .overlay(Capsule().stroke(DS.line, lineWidth: 1))
    }
}

/// On/off switch in the cable color.
struct CableToggle: View {
    @Binding var isOn: Bool
    var body: some View {
        Button { isOn.toggle() } label: {
            ZStack(alignment: isOn ? .trailing : .leading) {
                Capsule().fill(isOn ? DS.cable : DS.raised)
                    .overlay(Capsule().stroke(isOn ? Color.clear : DS.line, lineWidth: 1))
                Circle().fill(isOn ? DS.ink : DS.text2).frame(width: 16, height: 16).padding(3)
            }
            .frame(width: 40, height: 22)
            .animation(.easeOut(duration: 0.15), value: isOn)
        }
        .buttonStyle(.plain)
    }
}

/// 20-segment latency meter, 0-100 ms: green up to 40, amber up to 70, red above.
/// Unlit segments keep a dim tint of their zone so the scale reads at a glance.
struct LatencyMeter: View {
    var ms: Double?
    private let segments = 20
    private let maxMs = 100.0

    private func zone(_ i: Int) -> Color {
        let upper = Double(i + 1) * maxMs / Double(segments)
        return upper <= 40 ? DS.live : (upper <= 70 ? DS.warn : DS.error)
    }

    var body: some View {
        let lit = Int(((ms ?? 0) / maxMs * Double(segments)).rounded(.up))
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 3) {
                ForEach(0..<segments, id: \.self) { i in
                    RoundedRectangle(cornerRadius: 2)
                        .fill(i < lit ? zone(i) : zone(i).opacity(0.14))
                        .frame(height: 9)
                }
            }
            GeometryReader { geo in
                ForEach([0, 40, 70, 100], id: \.self) { mark in
                    let x = geo.size.width * CGFloat(mark) / CGFloat(maxMs)
                    VStack(spacing: 2) {
                        Rectangle().fill(DS.text3).frame(width: 1, height: 3)
                        Text(verbatim: "\(mark)").font(DSFont.mono(9)).foregroundColor(DS.text3)
                    }
                    .fixedSize()
                    .position(x: min(max(x, 6), geo.size.width - 9), y: 8)
                }
            }
            .frame(height: 16)
        }
    }
}

/// Last-minute latency line with the 40 ms threshold dashed.
struct Sparkline: View {
    var values: [Double]
    var body: some View {
        Canvas { ctx, size in
            let lo = 0.0, hi = max(60.0, (values.max() ?? 0) * 1.15)
            func y(_ v: Double) -> CGFloat { size.height - CGFloat((v - lo) / (hi - lo)) * size.height }
            var threshold = Path()
            threshold.move(to: CGPoint(x: 0, y: y(40)))
            threshold.addLine(to: CGPoint(x: size.width, y: y(40)))
            ctx.stroke(threshold, with: .color(DS.text3.opacity(0.6)), style: StrokeStyle(lineWidth: 1, dash: [2, 3]))
            guard values.count > 1 else { return }
            var path = Path()
            for (i, v) in values.enumerated() {
                let p = CGPoint(x: size.width * CGFloat(i) / CGFloat(values.count - 1), y: y(v))
                if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
            }
            ctx.stroke(path, with: .color(DS.cable), style: StrokeStyle(lineWidth: 1.25, lineJoin: .round))
            if let last = values.last {
                let p = CGPoint(x: size.width, y: y(last))
                ctx.fill(Path(ellipseIn: CGRect(x: p.x - 2.5, y: p.y - 2.5, width: 5, height: 5)), with: .color(DS.cable))
            }
        }
    }
}

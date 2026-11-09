// DroidScreen macOS - menu bar item, popover and settings window.
// Created from main.mm through DSUICreateController().

import AppKit
import SwiftUI

final class UIController: NSObject, DSUIController, NSPopoverDelegate, NSWindowDelegate {
    private let model = UIModel()
    private let settings = SettingsStore()
    private let deck = DeckAppsStore()
    private let nav = SettingsNavigation()

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let popover = NSPopover()
    private var settingsWindow: NSWindow?
    private var lastIconKey = ""
    private var hostingController: NSViewController?

    init(host: DSUIHost) {
        super.init()
        DSFont.registerBundled()
        model.host = host
        deck.onChange = { [weak host] in host?.uiDeckAppsChanged() }
        settings.onChange = { [weak self, weak host] key in
            guard let self else { return }
            // Only the tablet being streamed to needs a reconnect.
            if self.model.streaming && (key == nil || key == self.model.activeDeviceKey) {
                self.settings.pendingReconnect = true
            }
            host?.uiSettingsChanged(false)
        }

        popover.behavior = .transient
        popover.animates = true
        popover.delegate = self
        popover.appearance = NSAppearance(named: .darkAqua)
        let hosting = NSHostingController(rootView: PopoverView(model: model) { [weak self] in
            self?.showSettings()
        })
        if #available(macOS 13.0, *) {
            // Popover follows the SwiftUI content height (idle vs streaming).
            hosting.sizingOptions = [.preferredContentSize]
        }
        popover.contentViewController = hosting
        hostingController = hosting

        if let button = statusItem.button {
            button.target = self
            button.action = #selector(togglePopover(_:))
            button.toolTip = "DroidScreen"
        }
        refreshIcon()

        // Automation/screenshots: DROIDSCREEN_OPEN_SETTINGS=display|general|devices|deck|about
        if let raw = ProcessInfo.processInfo.environment["DROIDSCREEN_OPEN_SETTINGS"],
           let pane = SettingsPane.allCases.first(where: { $0.rawValue.lowercased() == raw.lowercased() }) {
            DispatchQueue.main.async { [weak self] in
                self?.nav.pane = pane
                self?.showSettings()
            }
        }
    }

    // MARK: DSUIController

    func updateState(_ state: [String: Any]) {
        let previousActive = model.activeDeviceKey
        model.apply(state: state)
        // A tablet just connected while the window shows the defaults: show its settings.
        if let key = model.activeDeviceKey, key != previousActive, settings.deviceKey == nil {
            settings.select(key)
        }
        if !model.streaming { settings.pendingReconnect = false }
        refreshIcon()
    }

    func updateStats(_ stats: [String: Any]) {
        model.apply(stats: stats)
        refreshIcon()
    }

    func updateTablets(_ tablets: [[String: Any]]) {
        model.apply(tablets: tablets)
    }

    func speedTestFinished(withThroughput throughputKbps: Int, selectedKbps: Int) {
        model.speedTest = throughputKbps > 0
            ? .done(throughputKbps: throughputKbps, selectedKbps: selectedKbps)
            : .failed
        // The core stores the result as the default; the streaming tablet gets it too.
        if throughputKbps > 0, let key = model.activeDeviceKey {
            let previous = settings.deviceKey
            settings.select(key)
            settings.bitrateMbps = SettingsStore.mbps(fromKbps: selectedKbps)
            settings.select(previous)
        } else {
            settings.reload()
        }
        settings.pendingReconnect = false
    }

    func showSettings() {
        popover.performClose(nil)
        if settingsWindow == nil {
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 720, height: 520),
                                  styleMask: [.titled, .closable, .fullSizeContentView],
                                  backing: .buffered, defer: false)
            window.titlebarAppearsTransparent = true
            window.titleVisibility = .hidden
            window.title = "DroidScreen Settings"
            window.appearance = NSAppearance(named: .darkAqua)
            window.backgroundColor = DS.nsInk
            window.isReleasedWhenClosed = false
            // Keep it on the built-in display, never on the virtual one.
            window.collectionBehavior = [.moveToActiveSpace, .transient]
            window.delegate = self
            window.contentView = NSHostingView(rootView: SettingsView(model: model, settings: settings,
                                                                      deck: deck, nav: nav))
            settingsWindow = window
        }
        guard let window = settingsWindow else { return }
        if let screen = NSScreen.main {
            let f = screen.visibleFrame
            window.setFrameOrigin(NSPoint(x: f.midX - window.frame.width / 2, y: f.midY - window.frame.height / 2))
        }
        deck.load()
        if !window.isVisible {
            // Open on the tablet that matters right now.
            settings.select(model.activeDeviceKey ?? model.usbDeviceKey ?? model.tablets.first?.id)
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: Popover

    @objc private func togglePopover(_ sender: Any?) {
        if popover.isShown {
            popover.performClose(sender)
        } else if let button = statusItem.button {
            if let view = hostingController?.view {
                view.layoutSubtreeIfNeeded()
                popover.contentSize = view.fittingSize
            }
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    // MARK: Menu bar icon

    /// The link mark, plus the live latency next to it while streaming.
    private func refreshIcon() {
        let kind = model.linkKind
        let latency = model.streaming ? model.latencyMs.map { Int($0.rounded()) } : nil
        let key = "\(kind)-\(model.connecting)-\(latency ?? -1)"
        guard key != lastIconKey, let button = statusItem.button else { return }
        lastIconKey = key

        button.image = StatusIcon.make(kind: kind, connecting: model.connecting)
        button.imagePosition = .imageLeading
        if let latency {
            button.attributedTitle = NSAttributedString(string: " \(latency)", attributes: [
                .font: NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium),
                .baselineOffset: 0.5,
            ])
        } else {
            button.title = ""
        }
    }
}

enum StatusIcon {
    /// 24x16 link mark. Idle is a template image (follows the menu bar color);
    /// connecting/streaming draw the cable in signal yellow.
    static func make(kind: LinkKind, connecting: Bool) -> NSImage {
        let size = NSSize(width: 24, height: 16)
        let image = NSImage(size: size, flipped: true) { rect in
            let w = rect.width, h = rect.height
            let outline: NSColor = kind == .idle ? .black : .labelColor
            outline.setStroke()

            let screen = NSBezierPath(roundedRect: NSRect(x: 1, y: 3, width: 7.5, height: 6.5), xRadius: 1.2, yRadius: 1.2)
            screen.lineWidth = 1.3
            screen.stroke()
            let base = NSBezierPath()
            base.move(to: NSPoint(x: 0, y: 12))
            base.line(to: NSPoint(x: 9.5, y: 12))
            base.lineWidth = 1.3
            base.lineCapStyle = .round
            base.stroke()

            let tablet = NSBezierPath(roundedRect: NSRect(x: w - 8.5, y: 3.5, width: 7.5, height: 8.5), xRadius: 1.4, yRadius: 1.4)
            tablet.lineWidth = 1.3
            tablet.stroke()

            let cable = NSBezierPath()
            cable.move(to: NSPoint(x: 10.5, y: h / 2))
            cable.line(to: NSPoint(x: w - 10, y: h / 2))
            cable.lineWidth = 1.6
            cable.lineCapStyle = .round
            switch kind {
            case .idle:
                var dash: [CGFloat] = [0.1, 2.4]
                cable.setLineDash(&dash, count: 2, phase: 0)
                NSColor.black.withAlphaComponent(0.55).setStroke()
            case .wifi:
                var dash: [CGFloat] = [1.6, 1.4]
                cable.setLineDash(&dash, count: 2, phase: 0)
                DS.nsCable.withAlphaComponent(connecting ? 0.5 : 1).setStroke()
            case .usb:
                DS.nsCable.withAlphaComponent(connecting ? 0.5 : 1).setStroke()
            }
            cable.stroke()
            return true
        }
        image.isTemplate = kind == .idle
        image.accessibilityDescription = "DroidScreen"
        return image
    }
}

@_cdecl("DSUICreateController")
public func DSUICreateController(_ host: DSUIHost) -> DSUIController {
    UIController(host: host)
}

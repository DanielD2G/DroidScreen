// Moves the mouse cursor in short bursts over a given display so a test can
// measure the latency of the first frame after the screen sat still.
// Usage: burst_cursor <display-id> <every-seconds> <total-seconds>
import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count == 4, let display = UInt32(args[1]),
      let every = Double(args[2]), let total = Double(args[3]) else {
    FileHandle.standardError.write("usage: burst_cursor <display-id> <every-s> <total-s>\n".data(using: .utf8)!)
    exit(2)
}
let bounds = CGDisplayBounds(display)
let start = Date()
var burst = 0
while Date().timeIntervalSince(start) < total {
    Thread.sleep(forTimeInterval: every)
    burst += 1
    // ~150 ms of motion: 10 steps across the middle of the display.
    for step in 0..<10 {
        let x = bounds.midX - 200 + CGFloat(step * 40 + (burst % 2) * 20)
        CGWarpMouseCursorPosition(CGPoint(x: x, y: bounds.midY))
        Thread.sleep(forTimeInterval: 0.015)
    }
}

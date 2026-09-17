// Renders assets/icon-1024.png: a macOS-style rounded tile with a hand-drawn
// mouse and a small "fixed" badge. Run via scripts/build.sh.
import AppKit

let size: CGFloat = 1024
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon-1024.png"

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size),
                           bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

// Tile: Apple's icon grid leaves a margin around an ~824pt rounded square.
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let tilePath = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)

let shadow = NSShadow()
shadow.shadowColor = NSColor.black.withAlphaComponent(0.30)
shadow.shadowBlurRadius = 28
shadow.shadowOffset = NSSize(width: 0, height: -12)
NSGraphicsContext.saveGraphicsState()
shadow.set()
NSColor.black.setFill()
tilePath.fill()
NSGraphicsContext.restoreGraphicsState()

let gradient = NSGradient(colors: [
    NSColor(calibratedRed: 0.16, green: 0.20, blue: 0.32, alpha: 1),
    NSColor(calibratedRed: 0.06, green: 0.08, blue: 0.14, alpha: 1),
])!
gradient.draw(in: tilePath, angle: -90)

// Soft highlight across the top, clipped to the tile.
NSGraphicsContext.saveGraphicsState()
tilePath.addClip()
NSGradient(colors: [NSColor.white.withAlphaComponent(0.16), NSColor.white.withAlphaComponent(0)])!
    .draw(in: NSRect(x: 100, y: 512, width: 824, height: 412), angle: -90)
NSGraphicsContext.restoreGraphicsState()

// The mouse: drawn by hand (no SF Symbols or Apple marks - their licences do
// not allow use in app icons).
let body = NSRect(x: 372, y: 262, width: 280, height: 520)
let mouse = NSBezierPath(roundedRect: body, xRadius: 140, yRadius: 140)
NSGraphicsContext.saveGraphicsState()
let mouseShadow = NSShadow()
mouseShadow.shadowColor = NSColor.black.withAlphaComponent(0.35)
mouseShadow.shadowBlurRadius = 30
mouseShadow.shadowOffset = NSSize(width: 0, height: -14)
mouseShadow.set()
NSColor.white.setFill()
mouse.fill()
NSGraphicsContext.restoreGraphicsState()
NSGradient(colors: [NSColor.white, NSColor(calibratedWhite: 0.86, alpha: 1)])!
    .draw(in: mouse, angle: -90)

// Two swipe arcs on the touch surface, hinting at the multitouch it restores.
NSColor(calibratedRed: 0.16, green: 0.20, blue: 0.32, alpha: 0.55).setStroke()
for (i, y) in [CGFloat(640), CGFloat(590)].enumerated() {
    let arc = NSBezierPath()
    let inset: CGFloat = 78 + CGFloat(i) * 14
    arc.move(to: NSPoint(x: body.minX + inset, y: y))
    arc.curve(to: NSPoint(x: body.maxX - inset, y: y),
              controlPoint1: NSPoint(x: body.midX - 30, y: y + 34),
              controlPoint2: NSPoint(x: body.midX + 30, y: y + 34))
    arc.lineWidth = 16
    arc.lineCapStyle = .round
    arc.stroke()
}

// "Fixed" badge, bottom right.
let badge = NSRect(x: 600, y: 180, width: 250, height: 250)
NSColor(calibratedRed: 0.07, green: 0.09, blue: 0.16, alpha: 1).setFill()
NSBezierPath(ovalIn: badge.insetBy(dx: -18, dy: -18)).fill()
NSColor(calibratedRed: 0.20, green: 0.78, blue: 0.35, alpha: 1).setFill()
NSBezierPath(ovalIn: badge).fill()
let check = NSBezierPath()
check.move(to: NSPoint(x: badge.minX + 66, y: badge.midY - 4))
check.line(to: NSPoint(x: badge.minX + 108, y: badge.midY - 48))
check.line(to: NSPoint(x: badge.maxX - 60, y: badge.midY + 52))
check.lineWidth = 26
check.lineCapStyle = .round
check.lineJoinStyle = .round
NSColor.white.setStroke()
check.stroke()

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("wrote \(out)")

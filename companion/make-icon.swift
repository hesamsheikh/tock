// Renders the app icon (pixel Tock, mustard on a dark tile) to build/AppIcon.icns.
// Run by build.sh when the icon is missing: swift make-icon.swift
import AppKit

let rows = [".....####.....", "...########...", "..##########..", "..##########..", "..##########..",
            "..##o####o##..", "..##o####o##..", "..####ww####..", "...########...", "...#......#...",
            "...#......#...", "..##......##.."]
let size = 1024.0
let image = NSImage(size: NSSize(width: size, height: size))
image.lockFocus()
let tile = NSBezierPath(roundedRect: NSRect(x: 100, y: 100, width: 824, height: 824), xRadius: 184, yRadius: 184)
NSColor(red: 0.043, green: 0.039, blue: 0.035, alpha: 1).setFill()
tile.fill()
NSColor(red: 0.906, green: 0.682, blue: 0.271, alpha: 1).setFill()
let px = 44.0, ox = (size - 14 * px) / 2, oy = (size - 12 * px) / 2 - 10
for (r, row) in rows.enumerated() {
  for (c, ch) in row.enumerated() where ch == "#" {
    NSRect(x: ox + Double(c) * px, y: size - oy - Double(r + 1) * px, width: px, height: px).fill()
  }
}
image.unlockFocus()
let png = NSBitmapImageRep(data: image.tiffRepresentation!)!.representation(using: .png, properties: [:])!
try! FileManager.default.createDirectory(atPath: "build/AppIcon.iconset", withIntermediateDirectories: true)
try! png.write(to: URL(fileURLWithPath: "build/icon-1024.png"))

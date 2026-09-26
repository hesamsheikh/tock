// Tock's look on the Mac: the device palette (dark and light), the 3 x 5 pixel font, and Tock
// himself, all drawn as square pixels like on the FIRE.

import SwiftUI

extension Color {
  init(hex: UInt32) {
    self.init(red: Double((hex >> 16) & 0xff) / 255, green: Double((hex >> 8) & 0xff) / 255, blue: Double(hex & 0xff) / 255)
  }
}

// The same palette as simulator/src/device/palette.ts and firmware/tock/gfx.h.
struct Palette {
  let bg, panel, ink, grey, body, faint, umber, dim, amber, red: Color

  static let dark = Palette(
    bg: Color(hex: 0x0b0a09), panel: Color(hex: 0x161412), ink: Color(hex: 0xf2ede3), grey: Color(hex: 0x6e6a62),
    body: Color(hex: 0xe7ae45), faint: Color(hex: 0x1c1a17), umber: Color(hex: 0x3d2f14), dim: Color(hex: 0x6b5020),
    amber: Color(hex: 0xa67c33), red: Color(hex: 0xe8574a))
  static let light = Palette(
    bg: Color(hex: 0xf3eee3), panel: Color(hex: 0xebe4d6), ink: Color(hex: 0x2b2925), grey: Color(hex: 0xa49d90),
    body: Color(hex: 0xe7ae45), faint: Color(hex: 0xe2dbcd), umber: Color(hex: 0xebdcb8), dim: Color(hex: 0xe3c68c),
    amber: Color(hex: 0xe0b366), red: Color(hex: 0xd9483b))

  static func of(_ scheme: ColorScheme) -> Palette { scheme == .dark ? .dark : .light }

  // Task colors, the same in both themes (firmware/tock/tasks.h, simulator/src/os/tasks.ts).
  // Mustard stays for untagged time.
  static let taskColors: [(name: String, color: Color)] = [
    ("Coral", Color(hex: 0xe9785b)), ("Teal", Color(hex: 0x3fb8a8)), ("Sage", Color(hex: 0x9bbf6a)),
    ("Sky", Color(hex: 0x5fa8e0)), ("Lilac", Color(hex: 0xa98be0)), ("Rose", Color(hex: 0xe07fa8)),
    ("Sand", Color(hex: 0xcdb88f)), ("Slate", Color(hex: 0x8a9bb0)),
  ]
  static func task(_ color: Int) -> Color { taskColors[((color % 8) + 8) % 8].color }

  // Heatmap steps: nothing, under 1 h, 1-2 h, 2-4 h, 4 h and more.
  func heat(_ minutes: Int) -> Color {
    minutes <= 0 ? faint : minutes < 60 ? umber : minutes < 120 ? dim : minutes < 240 ? amber : body
  }
}

// MARK: - pixel font

private let FONT: [Character: [String]] = [
  "0": ["###", "#.#", "#.#", "#.#", "###"], "1": [".#.", "##.", ".#.", ".#.", "###"],
  "2": ["###", "..#", "###", "#..", "###"], "3": ["###", "..#", ".##", "..#", "###"],
  "4": ["#.#", "#.#", "###", "..#", "..#"], "5": ["###", "#..", "###", "..#", "###"],
  "6": ["###", "#..", "###", "#.#", "###"], "7": ["###", "..#", "..#", ".#.", ".#."],
  "8": ["###", "#.#", "###", "#.#", "###"], "9": ["###", "#.#", "###", "..#", "###"],
  "A": [".#.", "#.#", "###", "#.#", "#.#"], "B": ["##.", "#.#", "##.", "#.#", "##."],
  "C": [".##", "#..", "#..", "#..", ".##"], "D": ["##.", "#.#", "#.#", "#.#", "##."],
  "E": ["###", "#..", "##.", "#..", "###"], "F": ["###", "#..", "##.", "#..", "#.."],
  "G": [".##", "#..", "#.#", "#.#", ".##"], "H": ["#.#", "#.#", "###", "#.#", "#.#"],
  "I": ["###", ".#.", ".#.", ".#.", "###"], "J": ["..#", "..#", "..#", "#.#", ".#."],
  "K": ["#.#", "#.#", "##.", "#.#", "#.#"], "L": ["#..", "#..", "#..", "#..", "###"],
  "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#"], "N": ["#..#", "##.#", "#.##", "#..#", "#..#"],
  "O": [".#.", "#.#", "#.#", "#.#", ".#."], "P": ["##.", "#.#", "##.", "#..", "#.."],
  "Q": [".#.", "#.#", "#.#", "##.", ".##"], "R": ["##.", "#.#", "##.", "#.#", "#.#"],
  "S": [".##", "#..", ".#.", "..#", "##."], "T": ["###", ".#.", ".#.", ".#.", ".#."],
  "U": ["#.#", "#.#", "#.#", "#.#", "###"], "V": ["#.#", "#.#", "#.#", "#.#", ".#."],
  "W": ["#...#", "#...#", "#.#.#", "##.##", "#...#"], "X": ["#.#", "#.#", ".#.", "#.#", "#.#"],
  "Y": ["#.#", "#.#", ".#.", ".#.", ".#."], "Z": ["###", "..#", ".#.", "#..", "###"],
  ":": [".", "#", ".", "#", "."], "!": ["#", "#", "#", ".", "#"], ".": [".", ".", ".", ".", "#"],
  "?": ["###", "..#", ".#.", "...", ".#."], "-": ["...", "...", "###", "...", "..."],
  "/": ["..#", "..#", ".#.", "#..", "#.."], " ": ["...", "...", "...", "...", "..."],
  "%": ["#.#", "..#", ".#.", "#..", "#.#"], "+": ["...", ".#.", "###", ".#.", "..."],
  ",": [".", ".", ".", "#", "#"], "'": ["#", "#", ".", ".", "."],
]

private func glyph(_ c: Character) -> [String] { FONT[Character(c.uppercased())] ?? FONT[" "]! }

// Text in Tock's pixel font. `scale` is the size of one font pixel, in points.
struct PixelText: View {
  let text: String
  var scale: CGFloat = 2
  var color: Color

  static func width(_ text: String, _ scale: CGFloat) -> CGFloat {
    let w = text.reduce(0) { $0 + CGFloat(glyph($1)[0].count + 1) }
    return max(0, (w - 1) * scale)
  }

  var body: some View {
    Canvas { ctx, _ in
      var x: CGFloat = 0
      for c in text {
        let g = glyph(c)
        for (r, row) in g.enumerated() {
          for (i, px) in row.enumerated() where px == "#" {
            ctx.fill(Path(CGRect(x: x + CGFloat(i) * scale, y: CGFloat(r) * scale, width: scale, height: scale)), with: .color(color))
          }
        }
        x += CGFloat(g[0].count + 1) * scale
      }
    }
    .frame(width: PixelText.width(text, scale), height: 5 * scale)
    .accessibilityLabel(text)
  }
}

// MARK: - Tock

enum TockPose {
  case stand, blink, cheer, sit, talk, tick, tock  // tick / tock: focusing, the knob swaying each second

  // Same grids as lab/tock.js: '#' body, anything else empty.
  var rows: [String] {
    let top = [".....####.....", "...########...", "..##########..", "..##########..", "..##########.."]
    let eyes = ["..##o####o##..", "..##o####o##.."], closed = ["..##########..", "..##########.."]
    let mouth = ["..####ww####..", "...########..."], open = ["..####ww####..", "...###ww###..."]
    let legs = ["...#......#...", "...#......#...", "..##......##.."]
    let focusEyes = ["..##########..", "..##o####o##.."]  // eyes down, on the work
    switch self {
    case .tick, .tock:
      var r = top + focusEyes + mouth + legs
      r[0] = self == .tick ? "....####......" : "......####...."
      return r
    case .stand: return top + eyes + mouth + legs
    case .blink: return top + closed + mouth + legs
    case .talk: return top + eyes + open + legs
    case .sit: return top + closed + mouth + ["..##......##.."]
    case .cheer:
      var r = top + eyes + open + legs
      r[1] = "#" + String(r[1].dropFirst().dropLast()) + "#"
      r[2] = "#." + String(r[2].dropFirst(2).dropLast(2)) + ".#"
      r[3] = "##" + String(r[3].dropFirst(2).dropLast(2)) + "##"
      return r
    }
  }
}

struct TockView: View {
  var pose: TockPose
  var px: CGFloat = 4
  var color: Color

  var body: some View {
    let rows = pose.rows
    Canvas { ctx, size in
      let top = size.height - CGFloat(rows.count) * px  // stand on the bottom edge
      for (r, row) in rows.enumerated() {
        for (c, ch) in row.enumerated() where ch == "#" {
          ctx.fill(Path(CGRect(x: CGFloat(c) * px, y: top + CGFloat(r) * px, width: px, height: px)), with: .color(color))
        }
      }
    }
    .frame(width: 14 * px, height: 12 * px)
    .accessibilityHidden(true)
  }
}

// Tock idling: a blink every few seconds.
struct IdleTock: View {
  var px: CGFloat = 4
  var mood: TockPose = .stand
  var color: Color

  var body: some View {
    TimelineView(.periodic(from: .now, by: 0.12)) { tl in
      let t = tl.date.timeIntervalSinceReferenceDate
      let blinking = mood == .stand && t.truncatingRemainder(dividingBy: 3.3) < 0.14
      TockView(pose: blinking ? .blink : mood, px: px, color: color)
    }
  }
}

// MARK: - icons

// Any '#' grid, drawn as px x px squares.
struct PixelGrid: View {
  let rows: [String]
  var px: CGFloat = 2
  var color: Color

  var body: some View {
    Canvas { ctx, _ in
      for (r, row) in rows.enumerated() {
        for (c, ch) in row.enumerated() where ch == "#" {
          ctx.fill(Path(CGRect(x: CGFloat(c) * px, y: CGFloat(r) * px, width: px, height: px)), with: .color(color))
        }
      }
    }
    .frame(width: CGFloat(rows.first?.count ?? 0) * px, height: CGFloat(rows.count) * px)
    .accessibilityHidden(true)
  }
}

// The same icons as the FIRE's home screen (firmware/tock/home.h).
enum Icons {
  static let wifi = [".#######.", "#.......#", "..#####..", ".#.....#.", "...###...", "....#...."]
  static let bluetooth = ["..##.", "#.#.#", ".###.", "..#..", ".###.", "#.#.#", "..##."]
  static let chevron = ["#####", ".###.", "..#.."]
}

// A battery like the FIRE's: outline, nub, and a fill in steps.
struct BatteryIcon: View {
  let level: Int
  let charging: Bool
  let p: Palette

  var body: some View {
    let w: CGFloat = 22, h: CGFloat = 12
    Canvas { ctx, _ in
      ctx.fill(Path(CGRect(x: 0, y: 0, width: w, height: h)), with: .color(p.grey))
      ctx.fill(Path(CGRect(x: 2, y: 2, width: w - 4, height: h - 4)), with: .color(p.bg))
      ctx.fill(Path(CGRect(x: w, y: 4, width: 2, height: 4)), with: .color(p.grey))
      let fill = ((w - 8) * CGFloat(min(max(level, 0), 100)) / 100).rounded()
      ctx.fill(Path(CGRect(x: 4, y: 4, width: fill, height: h - 8)),
               with: .color(charging ? p.body : level <= 25 ? p.red : p.ink))
    }
    .frame(width: w + 2, height: h)
    .accessibilityLabel("Battery \(level) percent\(charging ? ", charging" : "")")
  }
}

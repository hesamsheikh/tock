// Tock in the menu bar: standing by, swaying its knob while you focus (as on the FIRE), sitting
// while the Timer is paused, mouth open on a coffee break. A dot by its head asks for today's goal.
// Its panel, on glass: the goal ring filled in the colors of what you worked on, today's goal
// (each new day the menu bar asks for one until it's set, here or on Tock), what Tock's Timer is
// doing right now, and today's tasks.

import AppKit
import SwiftUI

// What today looks like, from the last reads.
struct Today {
  struct Part: Identifiable {
    let id: Int  // task id, 0 = untagged
    let name: String
    let minutes: Int
    let color: Color
  }

  let minutes: Int
  let goalMinutes: Int
  let goalSet: Bool  // a goal was set today (or Tock doesn't know the date yet, so don't nag)
  let parts: [Part]  // tasks with time today, in task order, then untagged

  init?(_ link: TockLink) {
    guard link.phase == .connected, let s = link.stats, s.today >= 0 else { return nil }
    minutes = s.todayMinutes
    goalMinutes = max(15, link.goalMinutes)
    goalSet = link.status?.goalDay == s.today
    var parts: [Part] = []
    var tagged = 0
    for t in link.tasks {
      let m = link.taskStats?.on(s.today, t.id) ?? 0
      if m > 0 { parts.append(Part(id: t.id, name: t.name, minutes: m, color: Palette.task(t.color))) }
      tagged += m
    }
    if minutes - tagged > 0 { parts.append(Part(id: 0, name: "UNTAGGED", minutes: minutes - tagged, color: Palette.dark.body)) }
    self.parts = parts
  }

  var progress: Double { min(1, Double(minutes) / Double(goalMinutes)) }
}

// What the Timer is doing, as Tock shows it.
enum TockMood {
  case idle, focus, onBreak, paused

  init(_ link: TockLink) {
    guard link.phase == .connected, let t = link.status?.timer else { self = .idle; return }
    switch t.state {
    case "running": self = t.onBreak ? .onBreak : .focus
    case "paused": self = .paused
    default: self = .idle
    }
  }

  // Focus sways the knob once a second (as on the FIRE); the rest hold still.
  func pose(second: Int) -> TockPose {
    switch self {
    case .idle: return .stand
    case .focus: return second % 2 == 0 ? .tick : .tock
    case .onBreak: return .talk
    case .paused: return .sit
    }
  }
}

// MARK: - the menu bar item

struct MenuBarLabel: View {
  @ObservedObject var link: TockLink
  @StateObject private var beat = Beat()
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    let today = Today(link)
    let mood = TockMood(link)
    Image(nsImage: MenuBarLabel.tockIcon(mood.pose(second: beat.second), connected: link.phase == .connected,
                                         askGoal: today.map { !$0.goalSet } ?? false))
      .accessibilityLabel(mood == .focus ? "Tock, focusing" : "Tock")
      .onAppear {
        AppDelegate.openSettings = {
          openWindow(id: "settings")
          NSApp.activate(ignoringOtherApps: true)
        }
      }
  }

  // Tock in mustard, 1.5 pt per pixel (crisp at 2x), grey while Tock isn't connected; a small
  // dot by its head while today has no goal.
  static func tockIcon(_ pose: TockPose, connected: Bool, askGoal: Bool) -> NSImage {
    let rows = pose.rows, px: CGFloat = 1.5
    let size = NSSize(width: 14 * px + 3, height: 12 * px)
    let image = NSImage(size: size, flipped: true) { _ in
      let top = CGFloat(12 - rows.count) * px
      (connected ? NSColor(Palette.dark.body) : NSColor.labelColor.withAlphaComponent(0.45)).setFill()
      for (r, row) in rows.enumerated() {
        for (c, ch) in row.enumerated() where ch == "#" {
          NSRect(x: CGFloat(c) * px, y: top + CGFloat(r) * px, width: px, height: px).fill()
        }
      }
      if askGoal {
        NSColor.labelColor.setFill()
        NSBezierPath(ovalIn: NSRect(x: size.width - 4.5, y: 0, width: 4.5, height: 4.5)).fill()
      }
      return true
    }
    image.isTemplate = false
    return image
  }
}

// A once-a-second beat for the menu bar's Tock (it sways while you focus). Runs in every run loop
// mode, so it keeps going while a menu is open.
final class Beat: ObservableObject {
  @Published var second = 0
  private var timer: Timer?

  init() {
    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.second += 1 }
    RunLoop.main.add(t, forMode: .common)
    timer = t
  }

  deinit { timer?.invalidate() }
}

// MARK: - the panel

struct TodayPanel: View {
  @ObservedObject var link: TockLink
  @ObservedObject var updater: Updater
  @Environment(\.colorScheme) private var scheme
  @Environment(\.openWindow) private var openWindow
  @State private var changingGoal = false
  @State private var customGoal = ""
  @FocusState private var typingGoal: Bool

  var body: some View {
    let p = Palette.glass(scheme)
    VStack(alignment: .leading, spacing: 14) {
      HStack(alignment: .bottom, spacing: 8) {
        TimelineView(.periodic(from: .now, by: 1)) { tl in
          TockView(pose: TockMood(link).pose(second: Int(tl.date.timeIntervalSinceReferenceDate)), px: 2,
                   color: link.phase == .connected ? p.body : p.grey)
        }
        PixelText(text: "TODAY", scale: 2, color: p.grey)
        Spacer()
        PixelText(text: dateLabel, scale: 2, color: p.grey)
      }
      if let today = Today(link) {
        HStack {
          Spacer()
          GoalRing(today: today, p: p)
          Spacer()
        }
        goal(today, p)
        if let timer = link.status?.timer { TimerCard(timer: timer, readAt: link.statusAt, tasks: link.tasks, p: p) }
        TaskList(today: today, p: p)
        if let s = link.stats { MiniHeatmap(stats: s, p: p) }
      } else {
        VStack(spacing: 10) {
          TockView(pose: .sit, px: 4, color: link.phase == .connected ? p.body : p.grey)
          Text(link.phase == .connected ? "Tock doesn't know the date yet." : "Tock isn't connected.")
            .font(.system(size: 12)).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
      }
      if updater.busy && updater.step != .checking {
        UpdateProgress(updater: updater, link: link, p: p, width: 268).padding(10).glassCard()
      } else if let r = updater.latest, updater.available {
        HStack {
          PixelText(text: "\(r.version) IS OUT", scale: 2, color: p.body)
          Spacer()
          GlassButton(label: "UPDATE", p: p) { Task { await updater.update() } }
        }
        .padding(.leading, 10).padding(.trailing, 6).padding(.vertical, 6)
        .glassCard()
        .help("Updates \(updater.what)")
        if !updater.message.isEmpty { Text(updater.message).font(.system(size: 11)).foregroundStyle(.secondary) }
      }
      Divider().opacity(0.5)
      HStack {
        GlassButton(label: "SETTINGS", p: p) {
          openWindow(id: "settings")
          NSApp.activate(ignoringOtherApps: true)
        }
        .keyboardShortcut(",", modifiers: .command)
        Spacer()
        GlassButton(label: "QUIT", p: p) { NSApp.terminate(nil) }
      }
    }
    .padding(16)
    .frame(width: 300)
    // no background of its own: the menu bar's glass shows through. And exactly as tall as what's
    // in it, so the window follows when something comes or goes while it's open
    .fixedSize(horizontal: false, vertical: true)
  }

  // "2.5", "2,5", "2:30" or "90m" (minutes), from 15 minutes to 16 hours.
  private var customMinutes: Int? {
    let text = customGoal.trimmingCharacters(in: .whitespaces).lowercased().replacingOccurrences(of: ",", with: ".")
    var minutes: Double?
    if text.hasSuffix("m"), let m = Double(text.dropLast()) { minutes = m }
    else if let colon = text.firstIndex(of: ":"), let h = Double(text[..<colon]), let m = Double(text[text.index(after: colon)...]) { minutes = h * 60 + m }
    else if let h = Double(text.hasSuffix("h") ? String(text.dropLast()) : text) { minutes = h * 60 }
    guard let m = minutes, m >= 15, m <= 16 * 60 else { return nil }
    return Int(m.rounded())
  }

  private func setCustomGoal() {
    guard let m = customMinutes else { return }
    link.setGoal(minutes: m)
    customGoal = ""
    changingGoal = false
  }

  private var dateLabel: String {
    let f = DateFormatter()
    f.dateFormat = "EEE d MMM"
    return f.string(from: Date()).uppercased()
  }

  // No goal yet today: ask, with the hours right there. Otherwise one quiet line to change it.
  @ViewBuilder private func goal(_ t: Today, _ p: Palette) -> some View {
    if !t.goalSet || changingGoal {
      VStack(alignment: .leading, spacing: 8) {
        PixelText(text: t.goalSet ? "CHANGE TODAY'S GOAL" : "WHAT'S TODAY'S GOAL?", scale: 2, color: t.goalSet ? p.grey : p.body)
        HStack(spacing: 5) {
          ForEach(TockLink.goalHours, id: \.self) { h in
            GoalChip(hours: h, current: t.goalSet && h * 60 == t.goalMinutes, p: p) {
              link.setGoal(minutes: h * 60)
              changingGoal = false
            }
          }
        }
        // or any length: hours, with a decimal point or as 2:30
        HStack(spacing: 6) {
          TextField("Other, like 2.5", text: $customGoal)
            .textFieldStyle(.plain)
            .font(.system(size: 12, design: .monospaced))
            .focused($typingGoal)
            .onSubmit { setCustomGoal() }
            .padding(.horizontal, 8)
            .frame(height: 26)
            .background(RoundedRectangle(cornerRadius: 6).fill(Color.primary.opacity(0.1)))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(typingGoal ? p.body : .clear, lineWidth: 1))
          PixelText(text: "HOURS", scale: 2, color: p.grey)
          GlassButton(label: "SET", p: p) { setCustomGoal() }
            .disabled(customMinutes == nil)
            .opacity(customMinutes == nil ? 0.5 : 1)
        }
      }
      .padding(10)
      .glassCard()
    } else {
      HStack {
        PixelText(text: "GOAL \(duration(t.goalMinutes))", scale: 2, color: p.grey)
        Spacer()
        Button("Change") { changingGoal = true }
          .buttonStyle(.plain)
          .font(.system(size: 11))
          .foregroundStyle(.secondary)
      }
    }
  }
}

// The goal as a ring, filled clockwise in the colors of today's tasks (untagged in mustard).
struct GoalRing: View {
  let today: Today
  let p: Palette
  private let size: CGFloat = 128, width: CGFloat = 12

  var body: some View {
    ZStack {
      Circle().stroke(Color.primary.opacity(0.15), lineWidth: width)
      ForEach(Array(arcs.enumerated()), id: \.offset) { _, arc in
        Circle()
          .trim(from: arc.from, to: arc.to)
          .stroke(arc.color, style: StrokeStyle(lineWidth: width, lineCap: .butt))
          .rotationEffect(.degrees(-90))
      }
      VStack(spacing: 8) {
        PixelText(text: duration(today.minutes), scale: 4, color: today.minutes > 0 ? p.ink : p.grey)
        PixelText(text: "\(Int((Double(today.minutes) / Double(today.goalMinutes) * 100).rounded()))% OF \(duration(today.goalMinutes))",
                  scale: 2, color: today.progress >= 1 ? p.body : p.grey)
      }
    }
    .frame(width: size, height: size)
    .padding(width / 2)
    .accessibilityElement()
    .accessibilityLabel("\(duration(today.minutes)) of a \(duration(today.goalMinutes)) goal")
  }

  private var arcs: [(from: CGFloat, to: CGFloat, color: Color)] {
    var out: [(CGFloat, CGFloat, Color)] = []
    var at: CGFloat = 0
    for part in today.parts {
      let next = min(1, at + CGFloat(part.minutes) / CGFloat(today.goalMinutes))
      if next > at { out.append((at, next, part.color)) }
      at = next
    }
    return out
  }
}

struct GoalChip: View {
  let hours: Int
  let current: Bool
  let p: Palette
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      PixelText(text: "\(hours)H", scale: 2, color: current ? Color(hex: 0x0b0a09) : hover ? p.ink : p.grey)
        .frame(width: 30, height: 26)
        .background(RoundedRectangle(cornerRadius: 6).fill(current ? p.body : Color.primary.opacity(hover ? 0.18 : 0.1)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel("\(hours) hours")
  }
}

// What Tock's Timer is doing; the countdown runs on here between reads.
struct TimerCard: View {
  let timer: TimerInfo
  let readAt: Date
  let tasks: [TockTask]
  let p: Palette

  var body: some View {
    let task = tasks.first { $0.id == timer.task }
    let color = timer.onBreak ? Color(hex: 0x3fb8a8) : task.map { Palette.task($0.color) } ?? p.body
    TimelineView(.periodic(from: .now, by: 1)) { tl in
      let running = timer.state == "running"
      let left = max(0, timer.left - (running ? Int(tl.date.timeIntervalSince(readAt)) : 0))
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 8) {
          Circle().fill(timer.state == "ready" ? Color.primary.opacity(0.45) : color).frame(width: 8, height: 8)
          PixelText(text: label(task), scale: 2, color: timer.state == "ready" ? p.grey : p.ink)
          Spacer()
          if timer.state != "ready" && timer.state != "waiting" {
            PixelText(text: String(format: "%d:%02d", left / 60, left % 60), scale: 2, color: running ? p.ink : p.grey)
          }
        }
        if timer.state == "running" || timer.state == "paused" {
          GeometryReader { g in
            ZStack(alignment: .leading) {
              Capsule().fill(Color.primary.opacity(0.15))
              Capsule().fill(color.opacity(timer.state == "paused" ? 0.5 : 1))
                .frame(width: g.size.width * CGFloat(timer.total > 0 ? Double(timer.total - left) / Double(timer.total) : 0))
            }
          }
          .frame(height: 4)
        }
      }
    }
    .padding(10)
    .glassCard()
  }

  private func label(_ task: TockTask?) -> String {
    switch timer.state {
    case "ready": return "TIMER READY"
    case "waiting": return "BREAK IS OVER"
    case "done": return "SESSION DONE"
    case "paused": return timer.onBreak ? "BREAK PAUSED" : "PAUSED"
    default: return timer.onBreak ? "COFFEE BREAK" : task?.name ?? "FOCUS"
    }
  }
}

// What today went to.
struct TaskList: View {
  let today: Today
  let p: Palette

  var body: some View {
    if today.parts.isEmpty {
      Text("Nothing yet today. Tock's ready when you are.").font(.system(size: 12)).foregroundStyle(.secondary)
    } else {
      VStack(spacing: 7) {
        ForEach(today.parts) { part in
          HStack(spacing: 8) {
            RoundedRectangle(cornerRadius: 2).fill(part.color).frame(width: 8, height: 8)
            PixelText(text: part.name, scale: 2, color: part.id == 0 ? p.grey : p.ink)
            Spacer()
            PixelText(text: duration(part.minutes), scale: 2, color: p.grey)
          }
        }
      }
    }
  }
}

// The last 16 weeks, a column per week (Monday on top), like the FIRE's heatmap; today framed.
struct MiniHeatmap: View {
  let stats: TockStats
  let p: Palette
  private let cell: CGFloat = 12, gap: CGFloat = 3, weeks = 16

  var body: some View {
    let pitch = cell + gap
    let first = stats.today - TockStats.weekday(stats.today) - (weeks - 1) * 7
    let startIndex = stats.today - (stats.days.count - 1)
    VStack(alignment: .leading, spacing: 8) {
      PixelText(text: "16 WEEKS", scale: 2, color: p.grey)
      Canvas { ctx, _ in
        for c in 0..<weeks {
          for r in 0..<7 {
            let day = first + c * 7 + r
            if day > stats.today { continue }
            let i = day - startIndex
            let minutes = i >= 0 && i < stats.days.count ? stats.days[i].minutes : 0
            let rect = CGRect(x: CGFloat(c) * pitch + 2, y: CGFloat(r) * pitch + 2, width: cell, height: cell)
            ctx.fill(Path(roundedRect: rect, cornerRadius: 2), with: .color(minutes > 0 ? p.heat(minutes) : Color.primary.opacity(0.13)))
            if day == stats.today {
              ctx.stroke(Path(roundedRect: rect.insetBy(dx: -1.5, dy: -1.5), cornerRadius: 3), with: .color(p.ink.opacity(0.8)), lineWidth: 1.5)
            }
          }
        }
      }
      .frame(width: CGFloat(weeks) * pitch - gap + 4, height: 7 * pitch - gap + 4)
      .frame(maxWidth: .infinity)
    }
  }
}

struct GlassButton: View {
  let label: String
  let p: Palette
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      PixelText(text: label, scale: 2, color: hover ? p.ink : p.grey)
        .padding(.horizontal, 10)
        .frame(height: 26)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.primary.opacity(hover ? 0.16 : 0.09)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel(label.capitalized)
  }
}

extension View {
  // A pane of frosted glass on the glass: a faint fill and a hairline edge.
  func glassCard() -> some View {
    background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.08)))
      .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.primary.opacity(0.14), lineWidth: 1))
  }
}

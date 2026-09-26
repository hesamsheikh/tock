// Tock for Mac: a menu bar companion for the FIRE (no Dock icon). It starts at login and stays
// running, so it keeps a log of your focus (Store.swift) whenever Tock is in range. Its panel
// shows today (MenuBar.swift); the Settings window (this file) is where the one-off things live:
// tasks, the OpenAI key and model, Wi-Fi, the screensaver's lines, and the app itself.
// Light or dark follows the Mac. Settings is one fixed window with tabs (cmd-1 to cmd-5).
//   Tock.app --demo              a made-up Tock, no Bluetooth (for screenshots)
//   Tock.app --demo --searching  the same, before Tock is found
//   Tock.app --demo --panel      also shows the menu bar item and its panel in a window
//   Tock.app --demo --settings   opens the Settings window at launch
//   Tock.app --icons <dir>       writes the menu bar poses as PNGs and quits

import AppKit
import ServiceManagement
import SwiftUI

// Opening Tock again while it runs (Finder, Spotlight, Launchpad) shows Settings: with no Dock
// icon, that's the way back to it besides the menu bar.
final class AppDelegate: NSObject, NSApplicationDelegate {
  static var openSettings: (() -> Void)?  // set by the menu bar item, which is always there

  func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
    AppDelegate.openSettings?()
    return false
  }
}

@main
struct TockCompanionApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
  @StateObject private var link = TockLink(demo: CommandLine.arguments.contains("--demo"))
  private static let arg = CommandLine.arguments.contains

  init() {
    // Tock.app --icons <dir>: write the menu bar poses as PNGs, 8x, and quit (for checking them)
    if let i = CommandLine.arguments.firstIndex(of: "--icons"), i + 1 < CommandLine.arguments.count {
      let dir = URL(fileURLWithPath: CommandLine.arguments[i + 1])
      for (name, pose) in [("idle", TockPose.stand), ("tick", .tick), ("tock", .tock), ("break", .talk), ("paused", .sit)] {
        let icon = MenuBarLabel.tockIcon(pose, connected: true, askGoal: name == "idle")
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(icon.size.width * 8), pixelsHigh: Int(icon.size.height * 8),
                                   bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                   colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current?.imageInterpolation = .none
        icon.draw(in: NSRect(x: 0, y: 0, width: icon.size.width * 8, height: icon.size.height * 8))
        NSGraphicsContext.restoreGraphicsState()
        try? rep.representation(using: .png, properties: [:])?.write(to: dir.appendingPathComponent("\(name).png"))
      }
      exit(0)
    }
    // start at login: asked for once, on the first run (the MAC tab switches it off and on)
    if !TockCompanionApp.arg("--demo") && !UserDefaults.standard.bool(forKey: "loginItemSet") {
      try? SMAppService.mainApp.register()
      UserDefaults.standard.set(true, forKey: "loginItemSet")
    }
  }

  var body: some Scene {
    // today at a glance, in the menu bar (MenuBar.swift)
    MenuBarExtra {
      TodayPanel(link: link)
    } label: {
      MenuBarLabel(link: link)
    }
    .menuBarExtraStyle(.window)

    // opened from the panel (or cmd-comma there); closing it leaves the app running
    Window("Tock Settings", id: "settings") {
      ContentView(link: link)
    }
    .windowResizability(.contentSize)
    .windowStyle(.hiddenTitleBar)
    .defaultLaunchBehavior(TockCompanionApp.arg("--settings") ? .presented : .suppressed)

    // Tock.app --demo --panel: the menu bar's label and panel in a window, for screenshots
    Window("Menu bar preview", id: "panel") {
      VStack(spacing: 12) {
        HStack(spacing: 6) { MenuBarLabel(link: link) }
          .padding(.horizontal, 10).padding(.vertical, 4)
          .background(Color.primary.opacity(0.08), in: Capsule())
        TodayPanel(link: link)
      }
      .padding(12)
    }
    .windowResizability(.contentSize)
    .defaultLaunchBehavior(TockCompanionApp.arg("--panel") ? .presented : .suppressed)
  }
}

enum Tab: String, CaseIterable {
  case tasks = "TASKS", voice = "VOICE", wifi = "WI-FI", saver = "SAVER", mac = "MAC"
}

struct ContentView: View {
  @ObservedObject var link: TockLink
  @Environment(\.colorScheme) private var scheme
  @State private var tab: Tab = .tasks

  static let width: CGFloat = 460, height: CGFloat = 680, margin: CGFloat = 24

  var body: some View {
    let p = Palette.of(scheme)
    VStack(alignment: .leading, spacing: 0) {
      Header(link: link, p: p)
        .padding(.horizontal, Self.margin)
        .padding(.top, 38)  // clear of the window buttons
        .padding(.bottom, 16)
        .fixedSize(horizontal: false, vertical: true)
      if link.phase == .connected {
        TabBar(tab: $tab, p: p)
        Group {
          switch tab {
          case .tasks: TasksPage(link: link, p: p)
          case .voice: VoicePage(link: link, p: p)
          case .wifi: WifiPage(link: link, p: p)
          case .saver: SaverPage(link: link, p: p)
          case .mac: MacPage(link: link, p: p)
          }
        }
        .padding(Self.margin)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else {
        ConnectHelp(phase: link.phase, p: p)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      }
      Footer(link: link, p: p)
    }
    .frame(width: Self.width, height: Self.height)
    .background(p.bg.ignoresSafeArea())
  }
}

// MARK: - frame

struct Header: View {
  @ObservedObject var link: TockLink
  let p: Palette

  var body: some View {
    HStack(alignment: .center, spacing: 14) {
      IdleTock(px: 4, mood: link.phase == .connected ? .stand : .sit, color: p.body)
      VStack(alignment: .leading, spacing: 8) {
        PixelText(text: "TOCK", scale: 5, color: p.body)
        PixelText(text: subtitle, scale: 2, color: p.grey)
      }
      Spacer()
      if link.phase == .connected, let s = link.status {
        HStack(spacing: 12) {
          if s.wifi != "off" { PixelGrid(rows: Icons.wifi, px: 2, color: wifiColor(s.wifi)).help("Wi-Fi: \(s.wifi)") }
          PixelGrid(rows: Icons.bluetooth, px: 2, color: p.body).help("Connected over Bluetooth")
          HStack(spacing: 6) {
            PixelText(text: "\(s.battery)%", scale: 2, color: p.grey)
            BatteryIcon(level: s.battery, charging: s.charging, p: p)
          }
          .help(s.charging ? "Charging" : "Battery")
        }
      }
    }
  }

  private var subtitle: String {
    switch link.phase {
    case .connected: return link.status?.name.uppercased() ?? "CONNECTED"
    default: return "NOT CONNECTED"  // the page below says why
    }
  }

  private func wifiColor(_ state: String) -> Color {
    switch state {
    case "online": return p.body
    case "failed": return p.red
    default: return p.grey
    }
  }
}

struct TabBar: View {
  @Binding var tab: Tab
  let p: Palette
  @State private var hovered: Tab?

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: 24) {
        ForEach(Array(Tab.allCases.enumerated()), id: \.element) { i, t in
          Button { tab = t } label: {
            VStack(alignment: .leading, spacing: 8) {
              PixelText(text: t.rawValue, scale: 2, color: tab == t || hovered == t ? p.ink : p.grey)
              Rectangle().fill(tab == t ? p.body : .clear).frame(width: PixelText.width(t.rawValue, 2), height: 3)
            }
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .keyboardShortcut(KeyEquivalent(Character("\(i + 1)")), modifiers: .command)
          .onHover { hovered = $0 ? t : nil }
          .accessibilityLabel(t.rawValue.capitalized)
        }
      }
      .padding(.horizontal, ContentView.margin)
      Rectangle().fill(p.faint).frame(height: 2)
    }
  }
}

struct Footer: View {
  @ObservedObject var link: TockLink
  let p: Palette

  var body: some View {
    VStack(spacing: 0) {
      Rectangle().fill(p.faint).frame(height: 2)
      HStack(spacing: 8) {
        Rectangle().fill(link.phase == .connected ? p.body : p.grey).frame(width: 6, height: 6)
        Text(message).font(.system(size: 11)).foregroundStyle(p.grey).lineLimit(1)
        Spacer()
      }
      .padding(.horizontal, ContentView.margin)
      .frame(height: 34)
    }
  }

  private var message: String {
    if !link.note.isEmpty { return link.note }
    switch link.phase {
    case .connected: return "Connected over Bluetooth. Tock's clock is set from this Mac."
    case .pairing: return "Waiting for the pairing code."
    default: return "Not connected."
    }
  }
}

struct ConnectHelp: View {
  let phase: TockLink.Phase
  let p: Palette

  var body: some View {
    VStack(spacing: 28) {
      TockView(pose: .sit, px: 8, color: phase == .bluetoothOff ? p.grey : p.body)
      VStack(spacing: 14) {
        PixelText(text: title, scale: 3, color: p.ink)
        Text(help)
          .font(.system(size: 13))
          .foregroundStyle(p.grey)
          .multilineTextAlignment(.center)
          .lineSpacing(3)
          .fixedSize(horizontal: false, vertical: true)
          .frame(width: 320)
      }
      if phase == .searching || phase == .connecting { Dots(p: p) }
    }
    .padding(.bottom, 40)
  }

  private var title: String {
    switch phase {
    case .bluetoothOff: return "BLUETOOTH IS OFF"
    case .pairing: return "TYPE THE CODE"
    case .connecting: return "CONNECTING"
    default: return "LOOKING FOR TOCK"
    }
  }

  private var help: String {
    switch phase {
    case .bluetoothOff: return "Turn on Bluetooth on this Mac, and Tock will show up here."
    case .pairing: return "Tock is showing a six-digit code. Type it into the box macOS just opened."
    default: return "On Tock, open Settings and switch Bluetooth on. This Mac finds it and asks for the code Tock shows, once."
    }
  }
}

// Three squares lighting up in turn.
struct Dots: View {
  let p: Palette

  var body: some View {
    TimelineView(.periodic(from: .now, by: 0.2)) { tl in
      let k = Int(tl.date.timeIntervalSinceReferenceDate / 0.2) % 3
      HStack(spacing: 8) {
        ForEach(0..<3) { i in Rectangle().fill(i == k ? p.body : p.faint).frame(width: 8, height: 8) }
      }
    }
  }
}

// MARK: - controls

func duration(_ minutes: Int) -> String {
  let h = minutes / 60, m = minutes % 60
  return h == 0 ? "\(m)M" : m == 0 ? "\(h)H" : "\(h)H \(m)M"
}

struct PageSection<Content: View>: View {
  let title: String
  let p: Palette
  @ViewBuilder var content: Content

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      PixelText(text: title, scale: 2, color: p.grey)
      content
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

struct Note: View {
  let text: String
  let p: Palette

  var body: some View {
    Text(text).font(.system(size: 12)).foregroundStyle(p.grey).lineSpacing(2).fixedSize(horizontal: false, vertical: true)
  }
}

// A square and a line of pixel text: how something stands.
struct StatusLine: View {
  let text: String
  let color: Color

  var body: some View {
    HStack(spacing: 8) {
      Rectangle().fill(color).frame(width: 8, height: 8)
      PixelText(text: text, scale: 2, color: color)
    }
  }
}

struct PixelButton: View {
  let label: String
  let p: Palette
  var enabled = true
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      PixelText(text: label, scale: 2, color: enabled ? Color(hex: 0x0b0a09) : p.grey)
        .padding(.horizontal, 14)
        .frame(height: 34)
    }
    .buttonStyle(PixelButtonStyle(fill: enabled ? p.body : p.faint))
    .disabled(!enabled)
    .accessibilityLabel(label.capitalized)
  }
}

struct PixelButtonStyle: ButtonStyle {
  let fill: Color
  @State private var hover = false

  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .background(fill.opacity(configuration.isPressed ? 0.7 : hover ? 0.88 : 1))
      .offset(y: configuration.isPressed ? 1 : 0)
      .onHover { hover = $0 }
  }
}

// A small pixel cross that removes a row; red on hover.
struct RemoveButton: View {
  let p: Palette
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      PixelGrid(rows: ["#...#", ".#.#.", "..#..", ".#.#.", "#...#"], px: 2, color: hover ? p.red : p.grey)
        .padding(6)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel("Remove")
  }
}

// Pixel text that acts as a link, for the quieter actions.
struct TextButton: View {
  let label: String
  let color: Color
  let p: Palette
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      VStack(alignment: .leading, spacing: 4) {
        PixelText(text: label, scale: 2, color: hover ? color : p.grey)
        Rectangle().fill(hover ? color : p.faint).frame(width: PixelText.width(label, 2), height: 2)
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel(label.capitalized)
  }
}

struct Field: View {
  let placeholder: String
  @Binding var text: String
  var secure = false
  let p: Palette
  var submit: () -> Void = {}
  @FocusState private var focused: Bool

  var body: some View {
    Group {
      if secure { SecureField(placeholder, text: $text) } else { TextField(placeholder, text: $text) }
    }
    .textFieldStyle(.plain)
    .font(.system(size: 13, design: .monospaced))
    .foregroundStyle(p.ink)
    .focused($focused)
    .onSubmit(submit)
    .padding(.horizontal, 10)
    .frame(height: 34)
    .background(p.panel)
    .overlay(Rectangle().stroke(focused ? p.body : p.faint, lineWidth: 2))
  }
}

func monthName(_ day: Int) -> String {
  let names = ["JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"]
  return names[(TockStats.date(day).month ?? 1) - 1]
}

// MARK: - voice

struct VoicePage: View {
  @ObservedObject var link: TockLink
  let p: Palette
  @State private var key = ""

  // Models Tock can talk with (firmware/tock/talk.h): GPT-Live, or the Realtime API ones.
  static let models: [(id: String, note: String)] = [
    ("gpt-live-1", "Billed per minute while Talk is open; Tock hangs up when idle."),
    ("gpt-realtime-2.1", "Realtime API, billed per token."),
    ("gpt-realtime-2.1-mini", "Realtime API, smaller and cheaper."),
    ("gpt-realtime-1.5", "Realtime API, the earlier version."),
  ]

  var body: some View {
    let saved = link.status?.key ?? ""
    let trimmed = key.trimmingCharacters(in: .whitespacesAndNewlines)
    VStack(alignment: .leading, spacing: 24) {
      PageSection(title: "OPENAI API KEY", p: p) {
        StatusLine(text: saved.isEmpty ? "NO KEY ON TOCK YET" : "ON TOCK, ENDING IN \(saved.uppercased())",
                   color: saved.isEmpty ? p.red : p.body)
        HStack(spacing: 8) {
          Field(placeholder: saved.isEmpty ? "sk-..." : "Paste a new key to replace it", text: $key, secure: true, p: p) {
            if !trimmed.isEmpty { sendKey(trimmed) }
          }
          PixelButton(label: "SEND", p: p, enabled: !trimmed.isEmpty) { sendKey(trimmed) }
        }
        HStack(alignment: .top) {
          Note(text: "Kept on Tock only. Tock talks to OpenAI over its own Wi-Fi, so it works with this Mac closed.", p: p)
          if !saved.isEmpty {
            Spacer(minLength: 16)
            TextButton(label: "REMOVE", color: p.red, p: p) { link.send("key:", done: "API key removed from Tock.") }
          }
        }
      }
      PageSection(title: "MODEL", p: p) {
        VStack(spacing: 4) {
          ForEach(options, id: \.id) { m in
            ModelRow(id: m.id, note: m.note, selected: m.id == current, p: p) {
              link.send("model:\(m.id)", done: "Tock will talk with \(m.id).")
            }
          }
        }
      }
    }
  }

  private var current: String { link.status?.model ?? "" }

  // Keep showing whatever Tock has now, even if it isn't in the list (so it can be changed away).
  private var options: [(id: String, note: String)] {
    VoicePage.models.contains { $0.id == current } || current.isEmpty ? VoicePage.models : VoicePage.models + [(current, "Set on Tock.")]
  }

  private func sendKey(_ k: String) {
    link.send("key:\(k)", done: "API key saved on Tock.")
    key = ""
  }
}

struct ModelRow: View {
  let id: String
  let note: String
  let selected: Bool
  let p: Palette
  let action: () -> Void
  @State private var hover = false

  var body: some View {
    Button(action: action) {
      HStack(alignment: .top, spacing: 12) {
        ZStack {  // a pixel radio button
          Rectangle().stroke(selected ? p.body : p.grey, lineWidth: 2).frame(width: 12, height: 12)
          if selected { Rectangle().fill(p.body).frame(width: 6, height: 6) }
        }
        .padding(.top, 1)
        VStack(alignment: .leading, spacing: 6) {
          PixelText(text: id.uppercased(), scale: 2, color: selected ? p.ink : hover ? p.ink : p.grey)
          Text(note).font(.system(size: 11)).foregroundStyle(p.grey)
        }
        Spacer()
      }
      .padding(.horizontal, 10)
      .padding(.vertical, 8)
      .background(selected ? p.panel : hover ? p.panel.opacity(0.5) : .clear)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .accessibilityLabel(id)
    .accessibilityAddTraits(selected ? .isSelected : [])
  }
}

// MARK: - Wi-Fi

struct WifiPage: View {
  @ObservedObject var link: TockLink
  let p: Palette
  @State private var ssid = ""
  @State private var password = ""

  var body: some View {
    VStack(alignment: .leading, spacing: 30) {
      if let s = link.status {
        PageSection(title: "NOW", p: p) {
          VStack(alignment: .leading, spacing: 10) {
            row("STATUS", s.wifi.uppercased(), s.wifi == "online" ? p.body : s.wifi == "failed" ? p.red : p.ink)
            if s.wifi == "online" {
              row("NETWORK", s.ssid.uppercased(), p.ink)
              row("ADDRESS", s.ip, p.ink)
            }
          }
          .padding(14)
          .frame(maxWidth: .infinity, alignment: .leading)
          .background(p.panel)
        }
      }
      PageSection(title: "JOIN A NETWORK", p: p) {
        Field(placeholder: "Network name", text: $ssid, p: p)
        Field(placeholder: "Password", text: $password, secure: true, p: p) { join() }
        HStack(alignment: .center, spacing: 14) {
          PixelButton(label: "SEND TO TOCK", p: p, enabled: !ssid.isEmpty) { join() }
          Note(text: "Tock saves it and switches over. 2.4 GHz networks only.", p: p)
        }
      }
    }
  }

  private func row(_ label: String, _ value: String, _ color: Color) -> some View {
    HStack(spacing: 0) {
      PixelText(text: label, scale: 2, color: p.grey).frame(width: 90, alignment: .leading)
      PixelText(text: value, scale: 2, color: color)
    }
  }

  private func join() {
    guard !ssid.isEmpty else { return }
    link.send("wifi:\(ssid)\t\(password)", done: "Tock is joining \(ssid).")
    password = ""
  }
}

// MARK: - tasks

// Task groups to file focus under, and this week split by them. Made here, picked on Tock:
// A in the Timer switches task. Every change sends the whole list.
struct TasksPage: View {
  @ObservedObject var link: TockLink
  let p: Palette
  @State private var newName = ""

  var body: some View {
    VStack(alignment: .leading, spacing: 24) {
      if let s = link.stats, s.today >= 0 {
        PageSection(title: "THIS WEEK", p: p) {
          WeekChart(stats: s, taskStats: link.taskStats, tasks: link.tasks, p: p)
        }
      }
      PageSection(title: "TASKS", p: p) {
        VStack(spacing: 6) {
          ForEach(link.tasks) { t in
            TaskRow(task: t, minutes: weekMinutes(t.id), current: t.id != 0 && t.id == link.currentTask, p: p,
                    rename: { name in update(t.id) { $0.name = name } },
                    recolor: { c in update(t.id) { $0.color = c } },
                    remove: { save(link.tasks.filter { $0.id != t.id }, "\(t.name) removed. Its time stays in the totals.") })
          }
          if link.tasks.count < TockLink.maxTasks {
            HStack(spacing: 8) {
              Field(placeholder: link.tasks.isEmpty ? "A first task, like STUDY" : "Another task", text: $newName, p: p) { add() }
              PixelButton(label: "ADD", p: p, enabled: !clean(newName).isEmpty) { add() }
            }
          }
        }
        Note(text: link.tasks.isEmpty
             ? "Optional. Tasks sort your focus time by color. On Tock, press A in the Timer to pick one."
             : "On Tock, press A in the Timer to switch task. Removing one keeps its time, untagged.", p: p)
      }
    }
  }

  private func weekMinutes(_ id: Int) -> Int {
    guard let s = link.stats, let ts = link.taskStats, s.today >= 0 else { return 0 }
    let start = s.today - TockStats.weekday(s.today)
    return (start...s.today).reduce(0) { $0 + ts.on($1, id) }
  }

  private func clean(_ s: String) -> String {
    let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :!.?-/%+,'")
    let up = String(s.uppercased().filter { allowed.contains($0) })
    return String(up.split(separator: " ").joined(separator: " ").prefix(TockLink.taskNameMax))
  }

  private func add() {
    let name = clean(newName)
    guard !name.isEmpty, link.tasks.count < TockLink.maxTasks else { return }
    let used = Set(link.tasks.map(\.color))
    let color = (0..<8).first { !used.contains($0) } ?? link.tasks.count % 8
    save(link.tasks + [TockTask(id: 0, color: color, name: name)], "\(name) added.")
    newName = ""
  }

  private func update(_ id: Int, _ change: (inout TockTask) -> Void) {
    var list = link.tasks
    guard let i = list.firstIndex(where: { $0.id == id }) else { return }
    change(&list[i])
    list[i].name = clean(list[i].name)
    guard !list[i].name.isEmpty, list[i] != link.tasks[i] else { return }
    save(list, "Tasks saved on Tock.")
  }

  private func save(_ list: [TockTask], _ done: String) { link.saveTasks(list, done: done) }
}

struct TaskRow: View {
  let task: TockTask
  let minutes: Int
  let current: Bool
  let p: Palette
  let rename: (String) -> Void
  let recolor: (Int) -> Void
  let remove: () -> Void
  @State private var name = ""
  @State private var picking = false
  @FocusState private var editing: Bool

  var body: some View {
    HStack(spacing: 10) {
      Button { picking = true } label: {
        Rectangle().fill(Palette.task(task.color)).frame(width: 18, height: 18)
          .overlay(Rectangle().stroke(p.faint, lineWidth: 1))
      }
      .buttonStyle(.plain)
      .help("Color")
      .accessibilityLabel("Color: \(Palette.taskColors[task.color % 8].name)")
      .popover(isPresented: $picking, arrowEdge: .bottom) {
        HStack(spacing: 6) {
          ForEach(0..<8, id: \.self) { i in
            Button { recolor(i); picking = false } label: {
              Rectangle().fill(Palette.task(i)).frame(width: 22, height: 22)
                .overlay(Rectangle().stroke(i == task.color ? p.ink : .clear, lineWidth: 2))
            }
            .buttonStyle(.plain)
            .help(Palette.taskColors[i].name)
            .accessibilityLabel(Palette.taskColors[i].name)
          }
        }
        .padding(10)
        .background(p.panel)
      }
      TextField("Name", text: $name)
        .textFieldStyle(.plain)
        .font(.system(size: 13, weight: .medium, design: .monospaced))
        .foregroundStyle(p.ink)
        .focused($editing)
        .onSubmit { rename(name) }
        .onChange(of: editing) { _, now in if !now { rename(name) } }
      if current { PixelText(text: "ON TOCK", scale: 2, color: p.grey).help("The task Tock's Timer counts for now") }
      PixelText(text: minutes > 0 ? duration(minutes) : "-", scale: 2, color: minutes > 0 ? p.ink : p.grey)
        .frame(width: 64, alignment: .trailing)
      RemoveButton(p: p, action: remove).help("Remove \(task.name)")
    }
    .padding(.horizontal, 10)
    .frame(height: 34)
    .background(p.panel)
    .overlay(Rectangle().stroke(editing ? p.body : .clear, lineWidth: 2))
    .onAppear { name = task.name }
    .onChange(of: task.name) { _, n in if !editing { name = n } }
  }
}

// Monday to Sunday, each day's bar split by task color; untagged time on top in mustard.
struct WeekChart: View {
  let stats: TockStats
  let taskStats: TaskStats?
  let tasks: [TockTask]
  let p: Palette

  var body: some View {
    let start = stats.today - TockStats.weekday(stats.today)
    let days = (0..<7).map { start + $0 }
    let most = max(60, days.map(total).max() ?? 0)
    HStack(alignment: .bottom, spacing: 10) {
      ForEach(days, id: \.self) { d in
        VStack(spacing: 8) {
          VStack(spacing: 0) {
            Spacer(minLength: 0)
            ForEach(parts(d).reversed(), id: \.0) { _, minutes, color in
              Rectangle().fill(color).frame(height: CGFloat(minutes) / CGFloat(most) * 96)
            }
          }
          .frame(height: 96)
          .background(alignment: .bottom) { Rectangle().fill(p.faint).frame(height: 2) }
          PixelText(text: String(["M", "T", "W", "T", "F", "S", "S"][d - start]), scale: 2,
                    color: d == stats.today ? p.ink : p.grey)
        }
        .frame(maxWidth: .infinity)
        .help(d > stats.today ? "" : "\(duration(total(d))) on \(["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"][d - start])")
      }
    }
  }

  private func total(_ day: Int) -> Int {
    let i = day - (stats.today - (stats.days.count - 1))
    return day <= stats.today && i >= 0 && i < stats.days.count ? stats.days[i].minutes : 0
  }

  // Bottom to top: tasks in order, then untagged.
  private func parts(_ day: Int) -> [(Int, Int, Color)] {
    var out: [(Int, Int, Color)] = []
    var tagged = 0
    for t in tasks {
      let m = taskStats?.on(day, t.id) ?? 0
      if m > 0 { out.append((t.id, m, Palette.task(t.color))) }
      tagged += m
    }
    let rest = total(day) - tagged
    if rest > 0 { out.append((0, rest, p.body)) }
    return out
  }
}

// MARK: - saver

// The screensaver's lines. Tock shows each as large as it fits, one at a time.
struct SaverPage: View {
  @ObservedObject var link: TockLink
  let p: Palette
  @State private var newLine = ""

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      PageSection(title: "SCREENSAVER LINES", p: p) {
        ScrollView {
          VStack(spacing: 6) {
            ForEach(Array(link.messages.enumerated()), id: \.offset) { i, line in
              LineRow(text: line, p: p,
                      edit: { t in change { $0[i] = t } },
                      remove: { change { $0.remove(at: i) } })
            }
          }
        }
        .frame(height: min(330, max(0, CGFloat(link.messages.count) * 40 - 6)))  // scrolls past 8 lines
        if link.messages.count < TockLink.maxMessages {
          HStack(spacing: 8) {
            Field(placeholder: "A new line, like KEEP GOING.", text: $newLine, p: p) { add() }
            PixelButton(label: "ADD", p: p, enabled: !clean(newLine).isEmpty) { add() }
          }
        }
        Note(text: "Up to \(TockLink.maxMessages) lines of \(TockLink.messageMax) letters. Remove them all and Tock goes back to its own two.", p: p)
      }
    }
  }

  private func clean(_ s: String) -> String {
    let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :!.?-/%+,'")
    let up = String(s.uppercased().filter { allowed.contains($0) })
    return String(up.split(separator: " ").joined(separator: " ").prefix(TockLink.messageMax))
  }

  private func add() {
    let line = clean(newLine)
    guard !line.isEmpty else { return }
    change { $0.append(line) }
    newLine = ""
  }

  private func change(_ edit: (inout [String]) -> Void) {
    var list = link.messages
    edit(&list)
    list = list.map(clean).filter { !$0.isEmpty }
    guard list != link.messages else { return }
    link.saveMessages(list, done: "Screensaver lines saved on Tock.")
  }
}

struct LineRow: View {
  let text: String
  let p: Palette
  let edit: (String) -> Void
  let remove: () -> Void
  @State private var value = ""
  @FocusState private var editing: Bool

  var body: some View {
    HStack(spacing: 10) {
      TextField("Line", text: $value)
        .textFieldStyle(.plain)
        .font(.system(size: 13, weight: .medium, design: .monospaced))
        .foregroundStyle(p.ink)
        .focused($editing)
        .onSubmit { edit(value) }
        .onChange(of: editing) { _, now in if !now { edit(value) } }
      RemoveButton(p: p, action: remove).help("Remove this line")
    }
    .padding(.horizontal, 10)
    .frame(height: 34)
    .background(p.panel)
    .overlay(Rectangle().stroke(editing ? p.body : .clear, lineWidth: 2))
    .onAppear { value = text }
    .onChange(of: text) { _, t in if !editing { value = t } }
  }
}

// MARK: - the app

// The app itself: starting at login, and the log it keeps.
struct MacPage: View {
  @ObservedObject var link: TockLink
  let p: Palette
  @State private var atLogin = SMAppService.mainApp.status == .enabled
  @State private var summary: (days: Int, sessions: Int, since: Int?) = (0, 0, nil)

  var body: some View {
    VStack(alignment: .leading, spacing: 28) {
      PageSection(title: "START", p: p) {
        Toggle(isOn: Binding(get: { atLogin }, set: setLogin)) {
          Text("Open Tock at login").font(.system(size: 13)).foregroundStyle(p.ink)
        }
        .toggleStyle(.switch)
        .tint(p.body)
        Note(text: "Tock lives in the menu bar and keeps running when this window closes, so it logs your focus whenever the FIRE is near. Quit it from the panel.", p: p)
      }
      PageSection(title: "YOUR LOG", p: p) {
        HStack(spacing: 10) {
          Tile(label: "DAYS", value: "\(summary.days)", p: p)
          Tile(label: "SESSIONS", value: "\(summary.sessions)", p: p)
        }
        if let since = summary.since {
          let d = TockStats.date(since)
          Note(text: "Kept on this Mac since \(d.day ?? 1) \(monthName(since).capitalized) \(d.year ?? 2026), in SQLite.", p: p)
        } else {
          Note(text: "Nothing logged yet. It fills in as soon as the FIRE connects.", p: p)
        }
        TextButton(label: "SHOW IN FINDER", color: p.body, p: p) {
          NSWorkspace.shared.activateFileViewerSelecting([Store.url])
        }
      }
    }
    .onAppear {
      summary = link.store?.summary() ?? (0, 0, nil)
      atLogin = SMAppService.mainApp.status == .enabled
    }
  }

  private func setLogin(_ on: Bool) {
    do {
      if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
      link.note = on ? "Tock will open at login." : "Tock won't open at login."
    } catch {
      link.note = "macOS didn't allow that: \(error.localizedDescription)"
    }
    atLogin = SMAppService.mainApp.status == .enabled
  }
}

struct Tile: View {
  let label: String
  let value: String
  let p: Palette

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      PixelText(text: label, scale: 2, color: p.grey)
      PixelText(text: value, scale: 3, color: p.ink)
    }
    .padding(12)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(p.panel)
  }
}

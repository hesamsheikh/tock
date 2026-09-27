// The Bluetooth link to Tock: find it, pair (macOS asks for the code Tock shows), read its status
// and stats every few seconds, and send it commands. The protocol is in firmware/tock/net.h.

import CoreBluetooth
import CryptoKit
import Foundation

struct TockStatus: Decodable {
  var version: String?  // the FIRE's firmware version (from 0.1.0)
  let name: String
  let battery: Int
  let charging: Bool
  let clock: Bool
  let wifi: String  // "online", "connecting", "failed", "not set up", "off"
  let ssid: String
  let ip: String
  let key: String  // last four characters of the API key, or "" when none is set
  let model: String
  var goalDay: Int?  // the day the daily goal was last set (days since 1970), -1 never
  var goalMin: Int?  // the daily goal in minutes (older firmware: only STATS' whole hours)
  var timer: TimerInfo?
}

// What Tock's Timer is doing, as of the last status read.
struct TimerInfo: Decodable {
  let state: String  // ready, running, paused, done, waiting (a break is over)
  let onBreak: Bool
  let left, total, round, rounds, task: Int  // seconds; task 0 = none

  enum CodingKeys: String, CodingKey { case state, onBreak = "break", left, total, round, rounds, task }
}

struct TockStats {
  struct Day { let minutes: Int; let sessions: Int }
  let goalHours: Int
  let today: Int  // days since 1970-01-01 on Tock's clock, -1 if Tock doesn't know the date
  let days: [Day]  // oldest first, the last one is today

  // u8 version, u8 goal hours, i32 today, u16 count, then count x (u16 minutes, u8 sessions)
  init?(_ d: Data) {
    let b = [UInt8](d)
    guard b.count >= 8, b[0] == 1 else { return nil }
    goalHours = Int(b[1])
    today = Int(Int32(bitPattern: UInt32(b[2]) | UInt32(b[3]) << 8 | UInt32(b[4]) << 16 | UInt32(b[5]) << 24))
    let count = Int(b[6]) | Int(b[7]) << 8
    guard b.count >= 8 + count * 3 else { return nil }
    days = (0..<count).map { i in
      let o = 8 + i * 3
      return Day(minutes: Int(b[o]) | Int(b[o + 1]) << 8, sessions: Int(b[o + 2]))
    }
  }

  init(goalHours: Int, today: Int, days: [Day]) {
    self.goalHours = goalHours
    self.today = today
    self.days = days
  }

  var todayMinutes: Int { days.last?.minutes ?? 0 }
  var todaySessions: Int { days.last?.sessions ?? 0 }

  // Monday = 0; 1970-01-01 was a Thursday.
  static func weekday(_ day: Int) -> Int { ((day % 7) + 7 + 3) % 7 }

  static func date(_ day: Int) -> DateComponents {
    var cal = Calendar(identifier: .gregorian)
    cal.timeZone = TimeZone(identifier: "UTC")!
    return cal.dateComponents([.year, .month, .day], from: Date(timeIntervalSince1970: TimeInterval(day) * 86400))
  }

  var weekMinutes: Int { days.suffix(TockStats.weekday(today) + 1).reduce(0) { $0 + $1.minutes } }
  var monthMinutes: Int { days.suffix(TockStats.date(today).day ?? 1).reduce(0) { $0 + $1.minutes } }

  // Days in a row with some focus, up to today (today still counts as open if it's empty so far).
  var streak: Int {
    var n = 0
    for (i, d) in days.reversed().enumerated() {
      if d.minutes > 0 { n += 1 } else if i > 0 { break }
    }
    return n
  }
}

// A task group (firmware/tock/tasks.h): focus can be filed under one. Ids 1-8; 0 = no task.
struct TockTask: Identifiable, Equatable {
  var id: Int
  var color: Int  // index into Palette.taskColors
  var name: String
}

// Per task, focused minutes for the last 14 days (oldest first; the last is today).
struct TaskStats {
  let today: Int
  let minutes: [Int: [Int]]

  // u8 version = 1, i32 today, u8 count, then per task: u8 id, 14 x u16 minutes
  init?(_ d: Data) {
    let b = [UInt8](d)
    guard b.count >= 6, b[0] == 1 else { return nil }
    today = Int(Int32(bitPattern: UInt32(b[1]) | UInt32(b[2]) << 8 | UInt32(b[3]) << 16 | UInt32(b[4]) << 24))
    var m: [Int: [Int]] = [:]
    var o = 6
    for _ in 0..<Int(b[5]) {
      guard o + 29 <= b.count else { return nil }
      m[Int(b[o])] = (0..<14).map { k in Int(b[o + 1 + k * 2]) | Int(b[o + 2 + k * 2]) << 8 }
      o += 29
    }
    minutes = m
  }

  init(today: Int, minutes: [Int: [Int]]) {
    self.today = today
    self.minutes = minutes
  }

  // A task's minutes on a day (days since 1970), 0 outside the 14 days.
  func on(_ day: Int, _ id: Int) -> Int {
    let i = day - (today - 13)
    guard let m = minutes[id], i >= 0, i < m.count else { return 0 }
    return m[i]
  }
}

final class TockLink: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
  static let service = CBUUID(string: "7a0c0001-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let statusUUID = CBUUID(string: "7a0c0002-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let statsUUID = CBUUID(string: "7a0c0003-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let commandUUID = CBUUID(string: "7a0c0004-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let tasksUUID = CBUUID(string: "7a0c0005-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let messagesUUID = CBUUID(string: "7a0c0006-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let taskStatsUUID = CBUUID(string: "7a0c0007-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let updateUUID = CBUUID(string: "7a0c0008-4c3f-4d7e-9b6a-70c6f1a0c0de")
  static let maxTasks = 8, taskNameMax = 12, maxMessages = 16, messageMax = 28

  enum Phase { case bluetoothOff, searching, connecting, pairing, connected }

  @Published var phase: Phase = .searching
  @Published var status: TockStatus?
  @Published var statusAt = Date()  // when `status` was read, to count the Timer down in between
  @Published var stats: TockStats?
  @Published var tasks: [TockTask] = []
  @Published var currentTask = 0
  @Published var messages: [String] = []
  @Published var taskStats: TaskStats?
  @Published var note = ""  // the last thing that happened, for the user
  @Published var firmware: FirmwareProgress?  // while a firmware update is on its way

  private var central: CBCentralManager!
  private var peripheral: CBPeripheral?
  private var chStatus, chStats, chCommand, chTasks, chMessages, chTaskStats, chUpdate: CBCharacteristic?
  private var outbox: [(command: String, done: String)] = []
  private var writing = false
  private var poller: Timer?

  private let demo: Bool
  let store: Store?  // the Mac's own log (Store.swift); none in demo mode

  // `demo` fills in a made-up Tock instead of using Bluetooth (for screenshots: Tock.app --demo).
  init(demo: Bool = false) {
    self.demo = demo
    store = demo ? nil : Store()
    super.init()
    setvbuf(stdout, nil, _IOLBF, 0)  // line by line, for `open --stdout <file>` when debugging
    if demo {
      fillDemo()
    } else {
      central = CBCentralManager(delegate: self, queue: .main)
    }
  }

  private func fillDemo() {
    status = TockStatus(name: "Tock-A1B2", battery: 80, charging: false, clock: true, wifi: "online",
                        ssid: "home-wifi", ip: "192.168.1.42", key: "a1b2", model: "gpt-live-1", goalDay: -1, goalMin: 240,
                        timer: TimerInfo(state: "running", onBreak: false, left: 14 * 60 + 12, total: 25 * 60, round: 2,
                                         rounds: 4, task: 1))
    let today = Int(Date().timeIntervalSince1970 / 86400)
    var rng = SystemRandomNumberGenerator()
    let days = (0..<112).map { i -> TockStats.Day in
      let m = i == 111 ? 55 : Int.random(in: 0...5, using: &rng) == 0 ? 0 : Int.random(in: 10...280, using: &rng)
      return TockStats.Day(minutes: m, sessions: m / 25)
    }
    stats = TockStats(goalHours: 4, today: today, days: days)
    tasks = [TockTask(id: 1, color: 0, name: "STUDY"), TockTask(id: 2, color: 1, name: "SIDE PROJECT"),
             TockTask(id: 3, color: 2, name: "READING")]
    currentTask = 1
    var split: [Int: [Int]] = [1: [], 2: [], 3: []]
    for d in days.suffix(14) {  // most of each day goes to a task
      let a = d.minutes * Int.random(in: 2...5, using: &rng) / 10, b = d.minutes * Int.random(in: 1...3, using: &rng) / 10
      split[1]!.append(a)
      split[2]!.append(b)
      split[3]!.append(max(0, d.minutes - a - b - d.minutes / 8))
    }
    taskStats = TaskStats(today: today, minutes: split)
    messages = ["EVERY SECOND COUNTS.", "YOU OWE HIM.", "ONE MORE ROUND."]
    phase = CommandLine.arguments.contains("--searching") ? .searching : .connected
  }

  // Queue a command; `done` is shown once Tock has it.
  func send(_ command: String, done: String) {
    if demo {
      note = done
      applyLocally(command)
      return
    }
    applyLocally(command)
    outbox.append((command, done))
    pump()
  }

  // Lists show the change right away; the next read from Tock confirms it.
  private func applyLocally(_ command: String) {
    if command.hasPrefix("tasks:") {
      tasks = TockLink.parseTasks(String(command.dropFirst(6)))
    } else if command.hasPrefix("msgs:") {
      messages = String(command.dropFirst(5)).split(separator: "\n").map(String.init)
    }
  }

  // Lines "id|color|name"; a line "cur:<id>" names the current task.
  static func parseTasks(_ text: String) -> [TockTask] {
    text.split(separator: "\n").compactMap { line in
      let f = line.split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false)
      guard f.count == 3, let id = Int(f[0]), let color = Int(f[1]) else { return nil }
      return TockTask(id: id, color: color, name: String(f[2]))
    }
  }

  static let goalHours = [1, 2, 3, 4, 5, 6, 8]  // the quick picks; any length from 15 min to 16 h works

  var goalMinutes: Int { status?.goalMin ?? max(1, stats?.goalHours ?? 4) * 60 }

  // Today's goal. Tock notes the day, which ends the menu bar's reminder.
  func setGoal(minutes: Int) {
    let m = min(16 * 60, max(15, minutes))
    status?.goalMin = m
    status?.goalDay = stats?.today
    send("goalmin:\(m)", done: "Today's goal: \(duration(m).lowercased()).")
  }

  // Send the whole list (new tasks go with id 0: Tock gives them one).
  func saveTasks(_ list: [TockTask], done: String) {
    let lines = list.prefix(TockLink.maxTasks).map { "\($0.id)|\($0.color)|\($0.name)" }.joined(separator: "\n")
    send("tasks:\(lines)", done: done)
  }

  func saveMessages(_ list: [String], done: String) {
    send("msgs:\(list.prefix(TockLink.maxMessages).joined(separator: "\n"))", done: done)
  }

  // MARK: finding and connecting

  func centralManagerDidUpdateState(_ c: CBCentralManager) {
    print("tock: bluetooth state \(c.state.rawValue)")
    if c.state == .poweredOn { search() } else { phase = .bluetoothOff }
  }

  private func search() {
    reset()
    phase = .searching
    if let id = UserDefaults.standard.string(forKey: "tock.peripheral").flatMap(UUID.init),
       let known = central.retrievePeripherals(withIdentifiers: [id]).first {
      connect(known)  // paired before: connects as soon as Tock is in range and visible
    }
    // no service filter: macOS doesn't always match a 128-bit UUID that sits in the scan response,
    // so Tock is recognized by its service or its "Tock-" name
    central.scanForPeripherals(withServices: nil)
  }

  func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
    let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
    let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? p.name ?? ""
    guard services.contains(TockLink.service) || name.hasPrefix("Tock-") else { return }
    print("tock: found \(name) \(p.identifier) rssi \(rssi)")
    if peripheral == nil || phase == .searching { connect(p) }
  }

  private func connect(_ p: CBPeripheral) {
    print("tock: connecting to \(p.identifier) state \(p.state.rawValue)")
    peripheral = p
    p.delegate = self
    phase = .connecting
    central.connect(p)
  }

  func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
    print("tock: connected")
    central.stopScan()
    p.discoverServices([TockLink.service])
  }

  func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
    print("tock: failed to connect: \(String(describing: error))")
    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.search() }
  }

  func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
    print("tock: disconnected: \(String(describing: error))")
    store?.observe(timer: nil)  // a session in progress ends here, as far as the log can tell
    if firmware?.stage == .sending { firmwareFinish("Tock went away during the update. It keeps its firmware; try again.") }
    if firmware?.stage == .installing { firmware?.stage = .restarting }  // it went before we saw "done"
    note = "Tock went away. Looking for it again."
    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.search() }
  }

  private func reset() {
    poller?.invalidate()
    poller = nil
    chStatus = nil
    chStats = nil
    chCommand = nil
    chTasks = nil
    chMessages = nil
    chTaskStats = nil
    chUpdate = nil
    writing = false
    status = nil
  }

  // Tock's characteristics changed (a firmware update): forget the old ones and look again.
  func peripheral(_ p: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
    poller?.invalidate()
    poller = nil
    chStatus = nil
    chStats = nil
    chCommand = nil
    chTasks = nil
    chMessages = nil
    chTaskStats = nil
    chUpdate = nil
    writing = false
    p.discoverServices([TockLink.service])
  }

  func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
    guard let svc = p.services?.first(where: { $0.uuid == TockLink.service }) else { return }
    p.discoverCharacteristics([TockLink.statusUUID, TockLink.statsUUID, TockLink.commandUUID, TockLink.tasksUUID,
                               TockLink.messagesUUID, TockLink.taskStatsUUID, TockLink.updateUUID], for: svc)
  }

  func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
    for ch in svc.characteristics ?? [] {
      switch ch.uuid {
      case TockLink.statusUUID: chStatus = ch
      case TockLink.statsUUID: chStats = ch
      case TockLink.commandUUID: chCommand = ch
      case TockLink.tasksUUID: chTasks = ch
      case TockLink.messagesUUID: chMessages = ch
      case TockLink.taskStatsUUID: chTaskStats = ch
      case TockLink.updateUUID: chUpdate = ch
      default: break
      }
    }
    // Tock has no clock of its own: hand it ours first, and our time zone, for when its days start.
    // This also triggers pairing the first time.
    outbox.insert(("time:\(Int(Date().timeIntervalSince1970))", ""), at: 0)
    outbox.insert(("tz:\(TockLink.posixTimeZone())", ""), at: 1)
    pump()
    poll()
    poller = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.poll() }
  }

  // This Mac's time zone as a POSIX TZ string, "STD-1DST-2,M3.5.0,M10.5.0/3" for Central Europe:
  // the offsets, and the rules for this year's daylight saving changes if it has them.
  static func posixTimeZone(_ zone: TimeZone = .current, now: Date = Date()) -> String {
    func clock(_ a: Int) -> String {
      "\(a / 3600)" + (a % 3600 == 0 ? "" : String(format: ":%02d", a % 3600 / 60)) + (a % 60 == 0 ? "" : String(format: ":%02d", a % 60))
    }
    func offset(_ seconds: Int) -> String { (seconds > 0 ? "-" : "") + clock(abs(seconds)) }  // POSIX counts west as positive
    guard let t1 = zone.nextDaylightSavingTimeTransition(after: now),
          let t2 = zone.nextDaylightSavingTimeTransition(after: t1) else {
      return "STD" + offset(zone.secondsFromGMT(for: now))
    }
    let a = zone.secondsFromGMT(for: t1), b = zone.secondsFromGMT(for: t2)  // just after each change
    let std = min(a, b), dst = max(a, b)
    let (start, end) = a == dst ? (t1, t2) : (t2, t1)
    // a change as "M<month>.<week>.<weekday>/<time>", the time on the clock just before it
    func rule(_ t: Date, clockOffset: Int) -> String {
      var cal = Calendar(identifier: .gregorian)
      cal.timeZone = TimeZone(secondsFromGMT: clockOffset)!
      let c = cal.dateComponents([.month, .day, .weekday, .hour, .minute, .second], from: t)
      let days = cal.range(of: .day, in: .month, for: t)!.count
      let week = c.day! + 7 > days ? 5 : (c.day! - 1) / 7 + 1
      let secs = c.hour! * 3600 + c.minute! * 60 + c.second!
      return "M\(c.month!).\(week).\(c.weekday! - 1)" + (secs == 7200 ? "" : "/" + clock(secs))
    }
    return "STD\(offset(std))DST\(offset(dst)),\(rule(start, clockOffset: std)),\(rule(end, clockOffset: dst))"
  }

  // MARK: reading

  private func poll() {
    guard let p = peripheral, firmware == nil || firmware?.stage == .restarting else { return }  // the update gets the link
    if let s = chStatus { p.readValue(for: s) }
    if let s = chStats { p.readValue(for: s) }
    // the lists: not while a change is on its way, or the old list would flash back
    if outbox.isEmpty && !writing {
      if let s = chTasks { p.readValue(for: s) }
      if let s = chMessages { p.readValue(for: s) }
    }
    if let s = chTaskStats { p.readValue(for: s) }
  }

  private func needsPairing(_ error: Error?) -> Bool {
    guard let e = error as? CBATTError else { return false }
    return e.code == .insufficientAuthentication || e.code == .insufficientEncryption
  }

  func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
    if let error {
      if needsPairing(error) { phase = .pairing }
      return
    }
    guard let data = ch.value else { return }
    if ch.uuid == TockLink.updateUUID {
      firmwareReport(data)
      return
    }
    if ch.uuid == TockLink.statusUUID {
      status = try? JSONDecoder().decode(TockStatus.self, from: data)
      statusAt = Date()
      if let s = status {
        if firmware?.stage == .restarting { firmwareBack(s.version) }
        store?.observe(timer: s.timer)
        if let day = s.goalDay, let m = s.goalMin, day == stats?.today { store?.save(goal: m, day: day) }
      }
    } else if ch.uuid == TockLink.statsUUID {
      stats = TockStats(data)
      if let s = stats { store?.save(stats: data, s) }
    } else if ch.uuid == TockLink.taskStatsUUID {
      taskStats = TaskStats(data)
      if let ts = taskStats { store?.save(taskStats: data, ts) }
    } else if ch.uuid == TockLink.tasksUUID, outbox.isEmpty {
      let text = String(decoding: data, as: UTF8.self)
      tasks = TockLink.parseTasks(text)
      store?.save(tasks: tasks)
      if let cur = text.split(separator: "\n").first(where: { $0.hasPrefix("cur:") }) { currentTask = Int(cur.dropFirst(4)) ?? 0 }
    } else if ch.uuid == TockLink.messagesUUID, outbox.isEmpty {
      messages = String(decoding: data, as: UTF8.self).split(separator: "\n").map(String.init)
    }
    if phase != .connected {
      phase = .connected
      UserDefaults.standard.set(p.identifier.uuidString, forKey: "tock.peripheral")
    }
  }

  // MARK: writing, one command at a time

  private func pump() {
    guard !writing, let p = peripheral, let ch = chCommand, let next = outbox.first else { return }
    writing = true
    p.writeValue(Data(next.command.utf8), for: ch, type: .withResponse)
  }

  func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
    writing = false
    if let error {
      if needsPairing(error) {
        phase = .pairing  // keep it queued, try again once paired
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.pump() }
        return
      }
      note = "Tock didn't take that: \(error.localizedDescription)"
      outbox.removeFirst()
    } else {
      let done = outbox.removeFirst().done
      if !done.isEmpty { note = done }
      poll()
    }
    pump()
  }

  // MARK: firmware updates (firmware/tock/update.h)
  //
  // "update:<size> <sha256> <deflated size>" gets the FIRE ready; then the image, deflated (about
  // 2/3 of it), goes in chunks (u32 offset, u32 FNV-1a of the bytes, the bytes) without waiting for each, and a read of UPDATE every window says how far the FIRE got:
  // anything it missed is sent again from there. "update:end" has it check and install the image.

  private var fwImage = Data()
  private var fwSent = 0, fwGot = 0, fwSentAtRead = 0
  private var fwReading = false, fwAccepted = false
  private var fwTarget = "", fwFrom = ""
  private var fwStarted = Date(), fwMoved = Date(), fwRateAt = Date()
  private var fwRateGot = 0, fwResent = 0
  private var fwWatch: Timer?
  private var fwDone: ((String?) -> Void)?
  private static let fwWindow = 12 * 1024  // the FIRE buffers 16 KB on its way to flash

  var canUpdateFirmware: Bool { phase == .connected && chUpdate != nil }

  // Why the FIRE can't take an update right now, or nil.
  var firmwareBlocked: String? {
    if phase != .connected { return "The FIRE isn't connected." }
    if chUpdate == nil { return "This FIRE's firmware predates updates from the Mac: flash it once over USB (firmware/flash.sh)." }
    if let t = status?.timer, t.state == "running" || t.state == "paused" { return "Tock's Timer is going. Update once it's done." }
    return nil
  }

  // Send `image` (firmware `version`) to the FIRE; `done` gets nil once it runs the new one, or why not.
  func updateFirmware(_ image: Data, version: String, done: @escaping (String?) -> Void) {
    if let why = firmwareBlocked { return done(why) }
    // deflated: the FIRE inflates it as it comes (the ESP32's ROM has the decoder)
    let packed = (try? (image as NSData).compressed(using: .zlib) as Data) ?? image
    fwImage = packed
    fwSent = 0
    fwRateGot = 0
    fwResent = 0
    fwRateAt = Date()
    fwGot = 0
    fwReading = false
    fwAccepted = false
    fwTarget = version
    fwFrom = status?.version ?? ""
    fwStarted = Date()
    fwMoved = Date()
    fwDone = done
    firmware = FirmwareProgress(stage: .sending, fraction: 0)
    let hash = SHA256.hash(data: image).map { String(format: "%02x", $0) }.joined()
    print("tock: firmware \(version), \(image.count) bytes, \(packed.count) deflated")
    send(packed.count < image.count ? "update:\(image.count) \(hash) \(packed.count)" : "update:\(image.count) \(hash)", done: "")
    fwWatch?.invalidate()
    fwWatch = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.firmwareTick() }
  }

  private func firmwareTick() {
    guard let f = firmware else { return }
    let quiet = Date().timeIntervalSince(fwMoved)
    switch f.stage {
    case .sending, .installing:
      if quiet > 20 {
        send("update:cancel", done: "")
        return firmwareFinish("The FIRE stopped taking the update. It keeps its firmware; try again.")
      }
      firmwareRead()
    case .restarting:
      if quiet > 90 { firmwareFinish("The FIRE hasn't come back after the update. Check that it's on and Bluetooth is too.") }
    }
  }

  private func firmwareRead() {
    guard !fwReading, let p = peripheral, let ch = chUpdate, outbox.isEmpty else { return }
    fwReading = true
    fwSentAtRead = fwSent
    p.readValue(for: ch)
  }

  // u8 state (0 idle, 1 receiving, 2 installing, 3 done, 4 failed), u8 error, u32 got, u32 size, u32 written
  private func firmwareReport(_ d: Data) {
    fwReading = false
    let b = [UInt8](d)
    guard b.count >= 14, firmware != nil else { return }
    let u32 = { (o: Int) in Int(b[o]) | Int(b[o + 1]) << 8 | Int(b[o + 2]) << 16 | Int(b[o + 3]) << 24 }
    let state = b[0], got = u32(2), size = u32(6), written = u32(10)
    let ours = size == fwImage.count
    switch state {
    case 1 where ours && firmware?.stage == .sending:
      fwAccepted = true
      if got != fwGot { fwMoved = Date() }
      fwGot = got
      if got < fwSentAtRead {  // it missed some: again from there
        fwResent += fwSentAtRead - got
        fwSent = got
      }
      fwSent = max(fwSent, got)
      firmware = FirmwareProgress(stage: .sending, fraction: Double(got) / Double(fwImage.count))
      if Date().timeIntervalSince(fwRateAt) >= 2 {
        print("tock: firmware: \(got / 1024) KB, \((got - fwRateGot) / 1024 / 2) KB/s, \(fwResent / 1024) KB sent again")
        fwRateAt = Date()
        fwRateGot = got
      }
      if got == fwImage.count {
        send("update:end", done: "")
        firmware = FirmwareProgress(stage: .installing, fraction: 0)
      } else {
        firmwarePump()
      }
    case 2 where ours:
      fwMoved = Date()
      firmware = FirmwareProgress(stage: .installing, fraction: Double(written) / Double(max(1, size)))
    case 3 where ours:
      fwMoved = Date()
      firmware = FirmwareProgress(stage: .restarting, fraction: 1)
      note = "The FIRE is restarting with \(fwTarget)."
    case 4 where fwAccepted || Date().timeIntervalSince(fwStarted) > 5:
      let why = ["", "The firmware is too big for the FIRE.", "The FIRE ran out of memory for it.",
                 "Some of it got lost on the way.", "It didn't check out on the FIRE.", "The FIRE couldn't write it.",
                 "The FIRE waited too long for it.", "It was cancelled."]
      firmwareFinish("The update didn't go through: \(why[min(Int(b[1]), why.count - 1)]) The FIRE keeps its firmware.")
    default:
      break  // not started yet: the next tick reads again
    }
  }

  // As many chunks as the link takes, up to a window ahead of what the FIRE confirmed.
  private func firmwarePump() {
    guard firmware?.stage == .sending, fwAccepted, let p = peripheral, let ch = chUpdate else { return }
    let size = min(512, p.maximumWriteValueLength(for: .withoutResponse)) - 8
    while fwSent < fwImage.count && fwSent - fwGot < TockLink.fwWindow && p.canSendWriteWithoutResponse {
      let n = min(size, fwImage.count - fwSent)
      let bytes = fwImage.subdata(in: fwSent..<fwSent + n)
      var packet = Data(capacity: n + 8)
      withUnsafeBytes(of: UInt32(fwSent).littleEndian) { packet.append(contentsOf: $0) }
      withUnsafeBytes(of: TockLink.fnv1a(bytes).littleEndian) { packet.append(contentsOf: $0) }
      packet.append(bytes)
      p.writeValue(packet, for: ch, type: .withoutResponse)
      fwSent += n
    }
    if fwSent == fwImage.count || fwSent - fwGot >= TockLink.fwWindow { firmwareRead() }
  }

  static func fnv1a(_ d: Data) -> UInt32 {
    d.reduce(2166136261 as UInt32) { ($0 ^ UInt32($1)) &* 16777619 }
  }

  func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) {
    firmwarePump()
  }

  // Back after the restart: the new version, or the old one if the new one didn't make it.
  private func firmwareBack(_ version: String?) {
    if version == fwTarget {
      firmwareFinish(nil)
    } else if Date().timeIntervalSince(fwMoved) > 45 {
      firmwareFinish("The FIRE went back to \(version ?? fwFrom): the new firmware didn't start properly.")
    }
  }

  private func firmwareFinish(_ error: String?) {
    print("tock: firmware update \(error == nil ? "done" : "failed") after \(Int(Date().timeIntervalSince(fwStarted))) s")
    fwWatch?.invalidate()
    fwWatch = nil
    fwImage = Data()
    firmware = nil
    let done = fwDone
    fwDone = nil
    done?(error)
  }
}

struct FirmwareProgress: Equatable {
  enum Stage { case sending, installing, restarting }
  var stage: Stage
  var fraction: Double
}

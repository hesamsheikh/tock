// The Mac's own log of your focus, in SQLite at ~/Library/Application Support/Tock/tock.sqlite.
// Tock keeps its history on the FIRE (400 days), but the Mac keeps everything it has seen, for
// good: the app runs in the menu bar from login, so whenever Tock is in range this stays current.
//
//   days       day (days since 1970, Tock's local calendar) -> focused minutes, finished sessions
//   tasks      every task Tock has had; `device_id` is Tock's 1-8, which Tock reuses after a task
//              is removed, so a removed task keeps its own row here and its history with it
//   task_days  day x task (tasks.rowid) -> minutes
//   goals      day -> the goal set for it, in minutes
//   sessions   each focus or break the Mac saw run on Tock's Timer: start, end, task, and whether
//              it ran to zero. Seen through Bluetooth every few seconds, so the times are close,
//              not exact, and only sessions while the Mac was near Tock are here.

import Foundation
import SQLite3

final class Store {
  private var db: OpaquePointer?
  private var lastStats = Data(), lastTaskStats = Data()
  private var lastTasks: [TockTask]?
  private var openSession: (rowid: Int64, kind: String)?

  static var url: URL {
    let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Tock")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir.appendingPathComponent("tock.sqlite")
  }

  init?() {
    guard sqlite3_open(Store.url.path, &db) == SQLITE_OK else { return nil }
    exec("""
      PRAGMA journal_mode = WAL;
      CREATE TABLE IF NOT EXISTS days (day INTEGER PRIMARY KEY, minutes INTEGER NOT NULL, sessions INTEGER NOT NULL,
                                       updated INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS tasks (device_id INTEGER NOT NULL, name TEXT NOT NULL, color INTEGER NOT NULL,
                                        removed INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS task_days (day INTEGER NOT NULL, task INTEGER NOT NULL REFERENCES tasks(rowid),
                                            minutes INTEGER NOT NULL, PRIMARY KEY (day, task));
      CREATE TABLE IF NOT EXISTS goals (day INTEGER PRIMARY KEY, minutes INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, task INTEGER,
                                           started INTEGER NOT NULL, ended INTEGER, planned INTEGER NOT NULL,
                                           finished INTEGER NOT NULL DEFAULT 0);
      """)
    // a session left open by a quit or a crash: end it where it was last seen
    exec("UPDATE sessions SET ended = started WHERE ended IS NULL")
  }

  deinit { sqlite3_close(db) }

  // MARK: what Tock sends

  // The day totals (the last 112 days, Tock is the truth for those).
  func save(stats data: Data, _ s: TockStats) {
    guard data != lastStats, s.today >= 0 else { return }
    lastStats = data
    let now = Int64(Date().timeIntervalSince1970)
    transaction {
      for (i, d) in s.days.enumerated() where d.minutes > 0 || d.sessions > 0 {
        run("INSERT INTO days VALUES (?, ?, ?, ?) ON CONFLICT(day) DO UPDATE SET minutes = excluded.minutes, "
            + "sessions = excluded.sessions, updated = excluded.updated WHERE minutes != excluded.minutes OR sessions != excluded.sessions",
            [Int64(s.today - (s.days.count - 1) + i), Int64(d.minutes), Int64(d.sessions), now])
      }
    }
  }

  // Tock's task list: new ones get a row, renamed or recolored ones are updated, missing ones
  // are marked removed (their history stays).
  func save(tasks: [TockTask]) {
    guard tasks != lastTasks else { return }
    lastTasks = tasks
    lastTaskStats = Data()  // file the per-task minutes again, under the rows as they are now
    let now = Int64(Date().timeIntervalSince1970)
    transaction {
      var active: [Int: (rowid: Int64, name: String)] = [:]
      query("SELECT rowid, device_id, name FROM tasks WHERE removed = 0") { row in
        active[Int(row.int(1))] = (row.int(0), row.text(2))
      }
      for t in tasks {
        if let a = active[t.id] {
          run("UPDATE tasks SET name = ?, color = ? WHERE rowid = ?", [t.name, Int64(t.color), a.rowid])
          active[t.id] = nil
        } else {
          run("INSERT INTO tasks (device_id, name, color, added) VALUES (?, ?, ?, ?)", [Int64(t.id), t.name, Int64(t.color), now])
        }
      }
      for a in active.values { run("UPDATE tasks SET removed = 1 WHERE rowid = ?", [a.rowid]) }
    }
  }

  // Per-task minutes for the last 14 days.
  func save(taskStats data: Data, _ ts: TaskStats) {
    guard data != lastTaskStats, ts.today >= 0, lastTasks != nil else { return }  // the task list comes first
    lastTaskStats = data
    transaction {
      for (deviceId, minutes) in ts.minutes {
        guard let row = activeTask(deviceId) else { continue }
        for (i, m) in minutes.enumerated() where m > 0 {
          run("INSERT INTO task_days VALUES (?, ?, ?) ON CONFLICT(day, task) DO UPDATE SET minutes = excluded.minutes",
              [Int64(ts.today - 13 + i), row, Int64(m)])
        }
      }
    }
  }

  // Today's goal, once Tock says it was set today.
  func save(goal minutes: Int, day: Int) {
    run("INSERT INTO goals VALUES (?, ?) ON CONFLICT(day) DO UPDATE SET minutes = excluded.minutes", [Int64(day), Int64(minutes)])
  }

  // The Timer, as each status read finds it: opens a session when a focus or break starts
  // running, closes it when that stops (finished if it ran out rather than being paused or reset).
  func observe(timer t: TimerInfo?) {
    let now = Int64(Date().timeIntervalSince1970)
    let kind = t.flatMap { $0.state == "running" ? ($0.onBreak ? "break" : "focus") : nil }
    if let open = openSession, open.kind != kind {
      let finished = t.map { $0.state == "done" || $0.state == "waiting" || ($0.onBreak && open.kind == "focus") } ?? false
      run("UPDATE sessions SET ended = ?, finished = ? WHERE id = ?", [now, finished ? 1 : 0, open.rowid])
      openSession = nil
    }
    if let kind, openSession == nil, let t {
      let task = kind == "focus" && t.task > 0 ? activeTask(t.task) : nil
      run("INSERT INTO sessions (kind, task, started, planned) VALUES (?, ?, ?, ?)",
          [kind, task, now - Int64(max(0, t.total - t.left)), Int64(t.total)])
      openSession = (sqlite3_last_insert_rowid(db), kind)
    }
  }

  // MARK: what the Settings window shows

  func summary() -> (days: Int, sessions: Int, since: Int?) {
    var out = (days: 0, sessions: 0, since: Int?.none)
    query("SELECT COUNT(*), MIN(day) FROM days") { row in
      out.days = Int(row.int(0))
      out.since = row.isNull(1) ? nil : Int(row.int(1))
    }
    query("SELECT COUNT(*) FROM sessions WHERE kind = 'focus'") { row in out.sessions = Int(row.int(0)) }
    return out
  }

  // MARK: SQLite

  private func activeTask(_ deviceId: Int) -> Int64? {
    var id: Int64?
    query("SELECT rowid FROM tasks WHERE device_id = ? AND removed = 0", [Int64(deviceId)]) { id = $0.int(0) }
    return id
  }

  private func exec(_ sql: String) {
    sqlite3_exec(db, sql, nil, nil, nil)
  }

  private func transaction(_ body: () -> Void) {
    exec("BEGIN")
    body()
    exec("COMMIT")
  }

  private func prepare(_ sql: String, _ args: [Any?]) -> OpaquePointer? {
    var st: OpaquePointer?
    guard sqlite3_prepare_v2(db, sql, -1, &st, nil) == SQLITE_OK else { return nil }
    let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
    for (i, a) in args.enumerated() {
      let n = Int32(i + 1)
      switch a {
      case let v as Int64: sqlite3_bind_int64(st, n, v)
      case let v as Int: sqlite3_bind_int64(st, n, Int64(v))
      case let v as String: sqlite3_bind_text(st, n, v, -1, transient)
      default: sqlite3_bind_null(st, n)
      }
    }
    return st
  }

  private func run(_ sql: String, _ args: [Any?] = []) {
    guard let st = prepare(sql, args) else { return }
    sqlite3_step(st)
    sqlite3_finalize(st)
  }

  private struct Row {
    let st: OpaquePointer
    func int(_ i: Int32) -> Int64 { sqlite3_column_int64(st, i) }
    func text(_ i: Int32) -> String { sqlite3_column_text(st, i).map { String(cString: $0) } ?? "" }
    func isNull(_ i: Int32) -> Bool { sqlite3_column_type(st, i) == SQLITE_NULL }
  }

  private func query(_ sql: String, _ args: [Any?] = [], _ each: (Row) -> Void) {
    guard let st = prepare(sql, args) else { return }
    while sqlite3_step(st) == SQLITE_ROW { each(Row(st: st)) }
    sqlite3_finalize(st)
  }
}

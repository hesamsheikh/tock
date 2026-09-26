// Updates, from the repo's latest GitHub release. release.sh makes those, from main only, with
// three files: Tock.zip (this app), tock-fire.bin (the FIRE's firmware) and SHA256SUMS. The FIRE
// goes first, over Bluetooth (TockLink); then the app replaces itself and starts again.
//
// Checked once a day, and from the button in the panel and in Settings > MAC. For testing, point
// it at another release JSON: defaults write dev.tock.companion tock.releases <url>
//   Tock.app --update-now   check, wait for the FIRE (a minute at most), and install what's newer

import AppKit
import CryptoKit
import Foundation

struct Version: Comparable, CustomStringConvertible {
  let parts: [Int]

  init?(_ text: String?) {
    guard var t = text?.trimmingCharacters(in: .whitespaces), !t.isEmpty else { return nil }
    if t.hasPrefix("v") { t.removeFirst() }
    let p = t.split(separator: ".").map { Int($0) }
    guard !p.isEmpty, p.allSatisfy({ $0 != nil }) else { return nil }
    parts = p.map { $0! }
  }

  private func part(_ i: Int) -> Int { i < parts.count ? parts[i] : 0 }

  static func < (a: Version, b: Version) -> Bool {
    for i in 0..<max(a.parts.count, b.parts.count) where a.part(i) != b.part(i) { return a.part(i) < b.part(i) }
    return false
  }

  static func == (a: Version, b: Version) -> Bool { !(a < b) && !(b < a) }

  var description: String { parts.map(String.init).joined(separator: ".") }
}

@MainActor
final class Updater: ObservableObject {
  struct Release {
    let version: Version
    let notes: String
    let files: [String: URL]
  }

  enum Step: Equatable {
    case idle
    case checking
    case downloading
    case fire  // TockLink.firmware has the details
    case app
    case failed(String)
  }

  @Published private(set) var latest: Release?
  @Published private(set) var step: Step = .idle
  @Published private(set) var checkedAt: Date?
  @Published private(set) var message = ""  // the outcome of the last check or update

  let link: TockLink
  private var daily: Timer?

  static let appVersion = Version(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String)
    ?? Version("0")!

  private static var feed: URL? {
    let s = UserDefaults.standard.string(forKey: "tock.releases")
      ?? Bundle.main.object(forInfoDictionaryKey: "TockReleases") as? String
    return s.flatMap(URL.init(string:))
  }

  init(link: TockLink, automatic: Bool) {
    self.link = link
    guard automatic else { return }
    if CommandLine.arguments.contains("--update-now") {
      Task {
        await check()
        for _ in 0..<60 where link.status == nil { try? await Task.sleep(for: .seconds(1)) }
        try? await Task.sleep(for: .seconds(3))  // the rest of the first reads
        print("tock: update now: \(latest?.version.description ?? "none"), fire \(fireBehind), app \(appBehind) \(message)")
        await update()
        print("tock: update now: done: \(message)")
      }
      return
    }
    // shortly after launch, then every few hours whether a day has passed
    DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in Task { await self?.check(quietly: true) } }
    let t = Timer(timeInterval: 3 * 3600, repeats: true) { [weak self] _ in
      Task { @MainActor in
        guard let self, Date().timeIntervalSince(self.checkedAt ?? .distantPast) > 20 * 3600 else { return }
        await self.check(quietly: true)
      }
    }
    RunLoop.main.add(t, forMode: .common)
    daily = t
  }

  var fireVersion: Version? { Version(link.status?.version) }
  var appBehind: Bool { latest.map { $0.version > Updater.appVersion } ?? false }
  var fireBehind: Bool {
    guard let l = latest, link.phase == .connected, link.status != nil else { return false }
    return fireVersion.map { l.version > $0 } ?? true  // no version at all: older than 0.1.0
  }
  var available: Bool { appBehind || fireBehind }
  var busy: Bool { [.checking, .downloading, .fire, .app].contains(step) }

  // What an update would change, for the button's label.
  var what: String {
    switch (fireBehind, appBehind) {
    case (true, true): return "the FIRE and this app"
    case (true, false): return "the FIRE"
    default: return "this app"
    }
  }

  // MARK: checking

  func check(quietly: Bool = false) async {
    guard !busy, let url = Updater.feed else { return }
    step = .checking
    defer { if step == .checking { step = .idle } }
    do {
      var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
      req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
      let (data, response) = try await URLSession.shared.data(for: req)
      let code = (response as? HTTPURLResponse)?.statusCode ?? 200
      if code == 404 {
        latest = nil
        checkedAt = Date()
        if !quietly { message = "No release to update to yet." }
        return
      }
      guard code == 200 else { throw Failure("GitHub answered \(code).") }
      let r = try JSONDecoder().decode(GitHubRelease.self, from: data)
      // only proper releases cut from main (release.sh); never drafts, pre-releases or other branches
      guard !r.draft, !r.prerelease, r.target_commitish == "main", let v = Version(r.tag_name) else {
        latest = nil
        checkedAt = Date()
        if !quietly { message = "No release to update to yet." }
        return
      }
      latest = Release(version: v, notes: r.body ?? "",
                       files: Dictionary(r.assets.map { ($0.name, $0.browser_download_url) }, uniquingKeysWith: { a, _ in a }))
      checkedAt = Date()
      message = available ? "" : "Up to date."
    } catch {
      if !quietly { message = "Couldn't check for updates: \(describe(error))" }
    }
  }

  // MARK: updating

  func update() async {
    guard !busy, let r = latest, available else { return }
    let fire = fireBehind && link.firmwareBlocked == nil, app = appBehind
    if fireBehind && !app, let why = link.firmwareBlocked {
      message = why
      return
    }
    message = ""
    do {
      step = .downloading
      let sums = try await checksums(r)
      var firmware: Data?
      if fire { firmware = try await fetch("tock-fire.bin", r, sums) }
      var zip: URL?
      if app { zip = try await download("Tock.zip", r, sums) }

      if let firmware {
        step = .fire
        if let error = await sendFirmware(firmware, version: r.version.description) { throw Failure(error) }
      }
      if let zip {
        step = .app
        try install(zip, version: r.version)  // quits and starts the new one
        return
      }
      step = .idle
      message = "The FIRE runs \(r.version) now."
    } catch {
      step = .failed(describe(error))
      message = describe(error)
    }
  }

  private func sendFirmware(_ image: Data, version: String) async -> String? {
    await withCheckedContinuation { c in
      link.updateFirmware(image, version: version) { error in c.resume(returning: error) }
    }
  }

  // SHA256SUMS: "<hex>  <name>" per line, as shasum -a 256 writes it.
  private func checksums(_ r: Release) async throws -> [String: String] {
    let text = String(decoding: try await fetch("SHA256SUMS", r, nil), as: UTF8.self)
    var sums: [String: String] = [:]
    for line in text.split(separator: "\n") {
      let f = line.split(separator: " ", omittingEmptySubsequences: true)
      if f.count == 2 { sums[String(f[1]).trimmingCharacters(in: CharacterSet(charactersIn: "*"))] = String(f[0]).lowercased() }
    }
    return sums
  }

  private func fetch(_ name: String, _ r: Release, _ sums: [String: String]?) async throws -> Data {
    guard let url = r.files[name] else { throw Failure("The release is missing \(name).") }
    let (data, response) = try await URLSession.shared.data(from: url)
    if let code = (response as? HTTPURLResponse)?.statusCode, code != 200 { throw Failure("Downloading \(name): \(code).") }
    if let sums { try verify(data, name, sums) }
    return data
  }

  private func download(_ name: String, _ r: Release, _ sums: [String: String]) async throws -> URL {
    let data = try await fetch(name, r, sums)
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tock-update-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let file = dir.appendingPathComponent(name)
    try data.write(to: file)
    return file
  }

  private func verify(_ data: Data, _ name: String, _ sums: [String: String]) throws {
    let hash = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    guard let want = sums[name] else { throw Failure("SHA256SUMS doesn't list \(name).") }
    guard hash == want else { throw Failure("\(name) didn't match its checksum.") }
  }

  // Unpack the new app next to the download, check it's Tock and signed as Tock, swap it in for
  // this one, and start it. macOS keys the Bluetooth permission to the signature's identifier,
  // which stays the same, so it carries over.
  private func install(_ zip: URL, version: Version) throws {
    let fm = FileManager.default
    let dir = zip.deletingLastPathComponent()
    try run("/usr/bin/ditto", ["-x", "-k", zip.path, dir.path])
    let new = dir.appendingPathComponent("Tock.app")
    guard let b = Bundle(url: new), b.bundleIdentifier == Bundle.main.bundleIdentifier,
          Version(b.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) == version
    else { throw Failure("The download isn't the Tock \(version) it should be.") }
    try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", "-R=identifier \"dev.tock.companion\"", new.path])

    let here = Bundle.main.bundleURL
    let old = here.deletingLastPathComponent().appendingPathComponent(".Tock-previous.app")
    try? fm.removeItem(at: old)
    do {
      try fm.moveItem(at: here, to: old)
    } catch {
      throw Failure("Couldn't replace \(here.path): \(error.localizedDescription)")
    }
    do {
      try fm.moveItem(at: new, to: here)
    } catch {
      try? fm.moveItem(at: old, to: here)
      throw Failure("Couldn't put the new app in place: \(error.localizedDescription)")
    }
    try? fm.removeItem(at: old)
    try? fm.removeItem(at: dir)

    let cfg = NSWorkspace.OpenConfiguration()
    cfg.createsNewApplicationInstance = true
    NSWorkspace.shared.openApplication(at: here, configuration: cfg) { _, _ in
      DispatchQueue.main.async { NSApp.terminate(nil) }
    }
  }

  private func run(_ tool: String, _ args: [String]) throws {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = args
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try p.run()
    p.waitUntilExit()
    guard p.terminationStatus == 0 else { throw Failure("\(URL(fileURLWithPath: tool).lastPathComponent) failed (\(p.terminationStatus)).") }
  }

  private func describe(_ error: Error) -> String {
    (error as? Failure)?.text ?? error.localizedDescription
  }
}

private struct Failure: Error {
  let text: String
  init(_ text: String) { self.text = text }
}

private struct GitHubRelease: Decodable {
  struct Asset: Decodable {
    let name: String
    let browser_download_url: URL
  }

  let tag_name: String
  let target_commitish: String
  let body: String?
  let draft: Bool
  let prerelease: Bool
  let assets: [Asset]
}

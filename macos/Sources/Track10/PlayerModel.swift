import AppKit
import CoreAudio
import Observation
import SwiftUI
import Track10Core

struct Stem: Identifiable {
    let id: Int
    let name: String
    let color: Color
    var volume: Double
    /// The volume to restore when a muted stem is unmuted.
    var unmutedVolume: Double
}

/// Wraps a T10Player for SwiftUI and polls it for position and events.
@MainActor
@Observable
final class PlayerModel {
    let title: String?
    let artist: String?
    let album: String?
    let duration: Double
    let cover: NSImage?
    var stems: [Stem]

    private(set) var position: Double = 0
    private(set) var isPlaying = false
    private(set) var rate: Double = 1
    private(set) var error: String?
    /// Set while the scrubber is dragged, so polling does not fight the drag.
    var isScrubbing = false

    @ObservationIgnored private var player: OpaquePointer?
    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private var deviceListener: AudioObjectPropertyListenerBlock?
    @ObservationIgnored private var pendingReset: DispatchWorkItem?

    init(url: URL) throws {
        var message: UnsafeMutablePointer<CChar>?
        guard let player = url.withUnsafeFileSystemRepresentation({ t10_player_open($0, &message) }) else {
            let text = message.map { String(cString: $0) } ?? "Could not open the file"
            free(message)
            throw PlayerError(message: text)
        }
        self.player = player

        func string(_ s: UnsafePointer<CChar>?) -> String? { s.map { String(cString: $0) } }
        title = string(t10_player_title(player))
        artist = string(t10_player_artist(player))
        album = string(t10_player_album(player))
        duration = t10_player_duration(player)

        var coverSize = 0
        if let bytes = t10_player_cover(player, &coverSize) {
            cover = NSImage(data: Data(bytes: bytes, count: coverSize))
        } else {
            cover = nil
        }

        stems = (0..<t10_player_stem_count(player)).map { i in
            let rgb = t10_player_stem_color(player, i)
            return Stem(
                id: Int(i),
                name: string(t10_player_stem_name(player, i)) ?? "Stem \(i + 1)",
                color: Color(
                    red: Double(rgb >> 16 & 0xFF) / 255,
                    green: Double(rgb >> 8 & 0xFF) / 255,
                    blue: Double(rgb & 0xFF) / 255),
                volume: 1,
                unmutedVolume: 1)
        }

        let timer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer

        // GStreamer's macOS output stays on the device it opened with, so
        // follow the system's default output by hand.
        let listener: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            MainActor.assumeIsolated { self?.defaultOutputChanged() }
        }
        var address = Self.defaultOutputAddress
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main, listener)
        deviceListener = listener
    }

    private static let defaultOutputAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)

    /// Connecting headphones can change the default several times in a row,
    /// so reset once things settle.
    private func defaultOutputChanged() {
        pendingReset?.cancel()
        let reset = DispatchWorkItem { [weak self] in
            guard let self, let player = self.player else { return }
            if !t10_player_reset_output(player) {
                self.error = t10_player_error(player).map { String(cString: $0) }
            }
            self.isPlaying = t10_player_is_playing(player)
        }
        pendingReset = reset
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3, execute: reset)
    }

    func close() {
        timer?.invalidate()
        timer = nil
        pendingReset?.cancel()
        if let deviceListener {
            var address = Self.defaultOutputAddress
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main, deviceListener)
        }
        deviceListener = nil
        if let player {
            t10_player_free(player)
        }
        player = nil
    }

    private func poll() {
        guard let player else { return }
        let events = t10_player_poll(player)
        if events & UInt32(T10_EVENT_ERROR) != 0 {
            error = t10_player_error(player).map { String(cString: $0) }
        }
        isPlaying = t10_player_is_playing(player)
        if !isScrubbing {
            position = t10_player_position(player)
        }
    }

    // MARK: Transport

    func togglePlay() {
        guard let player else { return }
        if isPlaying { t10_player_pause(player) } else { t10_player_play(player) }
        isPlaying = t10_player_is_playing(player)
    }

    func skip(by seconds: Double) {
        seek(to: position + seconds)
    }

    /// Moves the playhead shown while scrubbing, without seeking yet.
    func scrub(to seconds: Double) {
        position = min(max(seconds, 0), duration)
    }

    func seek(to seconds: Double) {
        guard let player else { return }
        position = min(max(seconds, 0), duration)
        t10_player_seek(player, position)
    }

    func setRate(_ newRate: Double) {
        guard let player else { return }
        t10_player_set_rate(player, newRate)
        rate = t10_player_rate(player)
    }

    // MARK: Stems

    func setVolume(_ volume: Double, of stem: Int) {
        guard let player else { return }
        stems[stem].volume = volume
        if volume > 0 {
            stems[stem].unmutedVolume = volume
        }
        t10_player_set_stem_volume(player, Int32(stem), volume)
    }

    func toggleMute(_ stem: Int) {
        let s = stems[stem]
        setVolume(s.volume > 0 ? 0 : max(s.unmutedVolume, 0.05), of: stem)
    }

    /// Plays only this stem, or everything again when it is already soloed.
    func solo(_ stem: Int) {
        let soloed = stems.indices.allSatisfy { ($0 == stem) == (stems[$0].volume > 0) }
        for i in stems.indices {
            let on = soloed || i == stem
            if on != (stems[i].volume > 0) {
                toggleMute(i)
            }
        }
    }
}

struct PlayerError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

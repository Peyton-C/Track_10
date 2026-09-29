import SwiftUI

/// A player window's content, laid out like Quick Look's audio preview with
/// the stem mixer in place of the volume slider.
struct PlayerView: View {
    @Bindable var model: PlayerModel

    var body: some View {
        VStack(spacing: 16) {
            HStack(alignment: .top, spacing: 24) {
                CoverView(image: model.cover, stems: model.stems)
                VStack(alignment: .leading, spacing: 16) {
                    TrackInfo(model: model)
                    StemMixer(model: model)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, 8)
            TransportBar(model: model)
        }
        .padding(16)
        .frame(width: 640)
        .alert("Playback Stopped", isPresented: .constant(model.error != nil)) {
            Button("OK") {}
        } message: {
            Text(model.error ?? "")
        }
    }
}

struct CoverView: View {
    let image: NSImage?
    let stems: [Stem]

    var body: some View {
        Group {
            if let image {
                Image(nsImage: image).resizable().aspectRatio(contentMode: .fill)
            } else {
                LinearGradient(colors: stems.map(\.color), startPoint: .topLeading, endPoint: .bottomTrailing)
                    .overlay {
                        Image(systemName: "music.note")
                            .font(.system(size: 64, weight: .medium))
                            .foregroundStyle(.white.opacity(0.85))
                    }
            }
        }
        .frame(width: 200, height: 200)
        .clipShape(.rect(cornerRadius: 4))
    }
}

struct TrackInfo: View {
    let model: PlayerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(model.title ?? "Untitled")
                .font(.title.bold())
                .lineLimit(2)
                .padding(.bottom, 4)
            if let artist = model.artist { InfoLine(label: "Artist", value: artist) }
            if let album = model.album { InfoLine(label: "Album", value: album) }
            InfoLine(label: "Time", value: formatTime(model.duration))
        }
    }
}

struct InfoLine: View {
    let label: String
    let value: String

    var body: some View {
        Text("\(label): \(Text(value).fontWeight(.semibold))")
            .font(.title3)
            .foregroundStyle(.secondary)
            .lineLimit(1)
    }
}

struct StemMixer: View {
    @Bindable var model: PlayerModel

    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 6) {
            ForEach(model.stems) { stem in
                GridRow {
                    Button {
                        if NSEvent.modifierFlags.contains(.option) {
                            model.solo(stem.id)
                        } else {
                            model.toggleMute(stem.id)
                        }
                    } label: {
                        HStack(spacing: 6) {
                            Circle()
                                .fill(stem.volume > 0 ? stem.color : .clear)
                                .strokeBorder(stem.color, lineWidth: 1.5)
                                .frame(width: 9, height: 9)
                            Text(stem.name)
                                .foregroundStyle(stem.volume > 0 ? .primary : .secondary)
                        }
                        .contentShape(.rect)
                    }
                    .buttonStyle(.plain)
                    .help("Click to mute, Option-click to solo")

                    Slider(value: Binding(
                        get: { stem.volume },
                        set: { model.setVolume($0, of: stem.id) }))
                        .tint(stem.color)
                        .controlSize(.small)
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.quaternary.opacity(0.6), in: .rect(cornerRadius: 14))
    }
}

struct TransportBar: View {
    @Bindable var model: PlayerModel
    private static let rates: [Double] = [0.5, 0.75, 1, 1.25, 1.5, 2]

    var body: some View {
        HStack(spacing: 14) {
            Button { model.skip(by: -15) } label: { Image(systemName: "gobackward.15") }
                .keyboardShortcut(.leftArrow, modifiers: .command)
            Button { model.togglePlay() } label: {
                Image(systemName: model.isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 18)
            }
            .keyboardShortcut(.space, modifiers: [])
            Button { model.skip(by: 15) } label: { Image(systemName: "goforward.15") }
                .keyboardShortcut(.rightArrow, modifiers: .command)

            Text(formatTime(model.position))
                .monospacedDigit()
                .font(.body)
                .foregroundStyle(.secondary)
            Slider(
                value: Binding(get: { model.position }, set: { model.scrub(to: $0) }),
                in: 0...max(model.duration, 0.1)
            ) { editing in
                model.isScrubbing = editing
                if !editing { model.seek(to: model.position) }
            }
            .controlSize(.small)
            .tint(.primary)
            Text(formatTime(model.duration))
                .monospacedDigit()
                .font(.body)
                .foregroundStyle(.secondary)

            Menu {
                Picker("Speed", selection: Binding(get: { model.rate }, set: { model.setRate($0) })) {
                    ForEach(Self.rates, id: \.self) { rate in
                        Text(rate == 1 ? "Normal" : "\(rate.formatted())×").tag(rate)
                    }
                }
                .pickerStyle(.inline)
            } label: {
                Image(systemName: "gauge.with.dots.needle.67percent")
            }
            .menuStyle(.button)
            .menuIndicator(.hidden)
            .fixedSize()
            .help("Playback speed")
        }
        .font(.title3)
        .buttonStyle(.borderless)
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
        .background(.quaternary.opacity(0.6), in: .capsule)
    }
}

func formatTime(_ seconds: Double) -> String {
    let total = Int(seconds.rounded(.down))
    return String(format: "%02d:%02d", total / 60, total % 60)
}

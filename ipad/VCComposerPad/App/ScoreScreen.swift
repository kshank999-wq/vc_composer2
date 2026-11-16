import SwiftUI

/// One project: a part's staff, engraved by the shared core, with the
/// floating keypad, transport and part picker.
struct ScoreScreen: View {
    @ObservedObject var document: ScoreDocument
    @StateObject private var entry = EntryState()
    @StateObject private var player = Player()
    @State private var trackId = ""
    @Environment(\.undoManager) private var undoManager

    private var tracks: [VCCTrack] { document.core.tracks }
    private var track: VCCTrack? { tracks.first { $0.identifier == trackId } ?? tracks.first }

    var body: some View {
        VStack(spacing: 0) {
            header
            ZStack(alignment: .bottomTrailing) {
                ScoreCanvas(document: document, entry: entry, trackId: track?.identifier ?? "", playhead: player.position)
                    .ignoresSafeArea(edges: .bottom)
                Keypad(entry: entry,
                       onDelete: deleteSelection,
                       onAccidental: setAccidental,
                       onUndo: { undoManager?.undo() },
                       onRedo: { undoManager?.redo() })
                    .padding(24)
            }
        }
        .onAppear { if trackId.isEmpty { trackId = tracks.first?.identifier ?? "" } }
        .onDisappear { player.stop() }
    }

    private var header: some View {
        HStack(spacing: 16) {
            Menu {
                ForEach(tracks, id: \.identifier) { t in
                    Button(t.name) { trackId = t.identifier; entry.selection = [] }
                }
            } label: {
                Label(track?.name ?? "No parts", systemImage: "music.note.list").font(.headline)
            }
            .accessibilityIdentifier("part")

            Button { player.playing ? player.stop() : player.play(document) } label: {
                Image(systemName: player.playing ? "stop.fill" : "play.fill").font(.title2)
            }
            .accessibilityIdentifier("play")
            .accessibilityLabel(player.playing ? "Stop" : "Play")

            Text("\(Int(document.core.tempo)) bpm  ·  \(document.core.meter)  ·  \(document.core.key)")
                .font(.subheadline).foregroundStyle(.secondary)
            Spacer()
            Text(entry.preview.isEmpty ? entry.status : entry.preview)
                .font(.subheadline.monospaced())
                .foregroundStyle(entry.preview.isEmpty ? Color.secondary : Color.orange)
                .accessibilityIdentifier("status")
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 10)
        .background(.bar)
    }

    private func deleteSelection() {
        guard let t = track else { return }
        var removed = 0
        for id in entry.selection where document.core.removeNote(id, inTrack: t.identifier) {
            document.edited(undoManager)
            removed += 1
        }
        entry.selection = []
        entry.status = removed > 0 ? "Deleted" : "Select a note to delete it"
    }

    private func setAccidental(_ a: Int) {
        guard let t = track else { return }
        for id in entry.selection where document.core.setAccidental(a, forNote: id, inTrack: t.identifier) {
            document.edited(undoManager)
        }
        entry.status = a < 0 ? "Flat" : a > 0 ? "Sharp" : "Natural"
    }
}

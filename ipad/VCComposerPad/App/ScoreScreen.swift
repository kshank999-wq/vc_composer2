import SwiftUI

/// One project: a part's staff, engraved by the shared core, with the
/// floating keypad, transport and part picker.
struct ScoreScreen: View {
    @ObservedObject var document: ScoreDocument
    @StateObject private var entry = EntryState()
    @StateObject private var player = Player()
    @StateObject private var ink = InkController()
    @State private var writing = false
    @AppStorage("fingerDraws") private var fingerDraws = false
    @State private var sending: SendItem?
    @State private var trackId = ""
    @Environment(\.undoManager) private var undoManager

    private var tracks: [VCCTrack] { document.core.tracks }
    private var track: VCCTrack? { tracks.first { $0.identifier == trackId } ?? tracks.first }

    var body: some View {
        VStack(spacing: 0) {
            header
            ZStack(alignment: .bottomTrailing) {
                ScoreCanvas(document: document, entry: entry, trackId: track?.identifier ?? "", playhead: player.position,
                            ink: ink, writing: writing, fingerDraws: fingerDraws)
                    .ignoresSafeArea(edges: .bottom)
                if writing { inkBar.padding(24).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading) }
                Keypad(entry: entry,
                       onDelete: deleteSelection,
                       onAccidental: setAccidental,
                       onUndo: { undoManager?.undo() },
                       onRedo: { undoManager?.redo() })
                    .padding(24)
            }
        }
        .onAppear { if trackId.isEmpty { trackId = tracks.first?.identifier ?? "" } }
        .sheet(item: $sending) { item in ShareSheet(items: [item.url]) }
        .onDisappear { player.stop() }
    }

    private var header: some View {
        HStack(spacing: 16) {
            Image("Logo")
                .resizable()
                .scaledToFit()
                .frame(height: 34)
                .accessibilityLabel("VC Composer")
                .accessibilityIdentifier("logo")

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

            Button { writing.toggle() } label: {
                Label("Write", systemImage: writing ? "pencil.tip.crop.circle.fill" : "pencil.tip.crop.circle")
                    .font(.headline)
            }
            .accessibilityIdentifier("write")
            .accessibilityAddTraits(writing ? .isSelected : [])

            Button { send() } label: { Label("Send", systemImage: "square.and.arrow.up").font(.headline) }
                .accessibilityIdentifier("send")
                .accessibilityHint("Share the score as a VC Composer project to open on the desktop")

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

    /// Handwriting: what was read, and Accept / Clear (spec §6.2: preview,
    /// correct, then commit).
    private var inkBar: some View {
        HStack(spacing: 12) {
            Image(systemName: "pencil.and.scribble").foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(ink.ready ? ink.summary : "Write with the Pencil on the staff")
                    .font(.subheadline.weight(.semibold))
                    .accessibilityIdentifier("ink-summary")
                if ink.toCheck > 0 {
                    Text("Tap a red note to take its other reading").font(.caption).foregroundStyle(.red)
                }
            }
            Toggle("Finger draws", isOn: $fingerDraws).toggleStyle(.button).font(.caption)
                .accessibilityIdentifier("finger-draws")
            Button("Read") { ink.read() }.accessibilityIdentifier("ink-read")
            Button("Clear", role: .destructive) { ink.clear() }.accessibilityIdentifier("ink-clear")
            Button("Accept") { ink.accept() }
                .buttonStyle(.borderedProminent)
                .disabled(!ink.ready)
                .accessibilityIdentifier("ink-accept")
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .shadow(radius: 6)
        .frame(maxWidth: 640)
    }

    /// Send to Desktop (spec §6.3's first step): the project as the desktop's
    /// own .vcc file, through the share sheet (AirDrop to the Mac, iCloud
    /// Drive, Files). The desktop opens it as it is.
    private func send() {
        let name = document.core.title.isEmpty ? "Score" : document.core.title
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(name + ".vcc")
        try? FileManager.default.removeItem(at: url)
        do {
            try document.core.save(toPath: url.path)
            sending = SendItem(url: url)
        } catch {
            entry.status = "Could not prepare the file: \(error.localizedDescription)"
        }
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

struct SendItem: Identifiable {
    let id = UUID()
    let url: URL
}

/// The system share sheet.
struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }
    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

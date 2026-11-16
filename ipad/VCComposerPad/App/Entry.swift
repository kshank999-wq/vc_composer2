import SwiftUI

/// SMuFL codepoints the app draws itself (Bravura).
enum Glyphs {
    static func noteGlyph(_ value: VCCValue) -> UInt32 {  // keypad: note with stem
        switch value {
        case .whole: return 0xE1D2
        case .half: return 0xE1D3
        case .quarter: return 0xE1D5
        case .eighth: return 0xE1D7
        case .sixteenth: return 0xE1D9
        case .thirtySecond: return 0xE1DB
        @unknown default: return 0xE1D5
        }
    }
    static func notehead(_ value: VCCValue) -> UInt32 {  // the shadow note
        switch value {
        case .whole: return 0xE0A2
        case .half: return 0xE0A3
        default: return 0xE0A4
        }
    }
    static func rest(_ value: VCCValue) -> UInt32 {
        switch value {
        case .whole: return 0xE4E3
        case .half: return 0xE4E4
        case .quarter: return 0xE4E5
        case .eighth: return 0xE4E6
        case .sixteenth: return 0xE4E7
        case .thirtySecond: return 0xE4E8
        @unknown default: return 0xE4E5
        }
    }
    static let dot: UInt32 = 0xE1E7
    static let flat: UInt32 = 0xE260
    static let natural: UInt32 = 0xE261
    static let sharp: UInt32 = 0xE262

    static func text(_ codepoint: UInt32) -> String { String(Character(UnicodeScalar(codepoint)!)) }
}

/// What the next note entered will be, and what is selected.
final class EntryState: ObservableObject {
    @Published var value: VCCValue = .quarter
    @Published var dots = 0
    @Published var restMode = false
    /// nil: as the key signature spells it; else -1, 0, 1.
    @Published var accidental: Int? = nil
    @Published var selection: Set<UInt64> = []
    /// The note under the pen or finger before it is placed: "B4", "Rest".
    @Published var preview = ""
    @Published var status = "Tap the staff to enter a note"
}

/// The floating keypad (spec §6.2's "normal tap-entry mode"): note values,
/// dot, rest, accidentals, delete and undo. It can be dragged anywhere.
struct Keypad: View {
    @ObservedObject var entry: EntryState
    var onDelete: () -> Void
    var onAccidental: (Int) -> Void
    var onUndo: () -> Void
    var onRedo: () -> Void
    @State private var offset = CGSize.zero
    @GestureState private var drag = CGSize.zero

    private let values: [(VCCValue, String)] = [(.whole, "whole"), (.half, "half"), (.quarter, "quarter"),
                                                (.eighth, "eighth"), (.sixteenth, "sixteenth"), (.thirtySecond, "thirty-second")]

    var body: some View {
        VStack(spacing: 6) {
            Capsule().fill(Color.secondary.opacity(0.5)).frame(width: 40, height: 5).padding(.top, 4)
                .accessibilityHidden(true)
            HStack(spacing: 6) {
                ForEach(values, id: \.1) { value, name in
                    key(Glyphs.noteGlyph(value), on: entry.value == value && !entry.restMode, id: name, label: name + " note") {
                        entry.value = value
                        entry.restMode = false
                    }
                }
            }
            HStack(spacing: 6) {
                key(Glyphs.rest(entry.value), on: entry.restMode, id: "rest", label: "rest") { entry.restMode.toggle() }
                key(Glyphs.dot, on: entry.dots > 0, id: "dot", label: "dot") { entry.dots = entry.dots == 0 ? 1 : 0 }
                key(Glyphs.flat, on: entry.accidental == -1, id: "flat", label: "flat") { accidental(-1) }
                key(Glyphs.natural, on: entry.accidental == 0, id: "natural", label: "natural") { accidental(0) }
                key(Glyphs.sharp, on: entry.accidental == 1, id: "sharp", label: "sharp") { accidental(1) }
                textKey("trash", id: "delete", label: "delete", action: onDelete)
            }
            HStack(spacing: 6) {
                textKey("arrow.uturn.backward", id: "undo", label: "undo", action: onUndo)
                textKey("arrow.uturn.forward", id: "redo", label: "redo", action: onRedo)
            }
        }
        .padding(10)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .shadow(radius: 8)
        .offset(x: offset.width + drag.width, y: offset.height + drag.height)
        .gesture(DragGesture().updating($drag) { v, s, _ in s = v.translation }
                    .onEnded { v in offset.width += v.translation.width; offset.height += v.translation.height })
        // No identifier here: on a container it would replace the keys' own.
        .accessibilityElement(children: .contain)
    }

    /// An accidental: on the selection when there is one, else for the next note.
    private func accidental(_ a: Int) {
        if !entry.selection.isEmpty { onAccidental(a) } else { entry.accidental = entry.accidental == a ? nil : a }
    }

    private func key(_ glyph: UInt32, on: Bool, id: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(Glyphs.text(glyph)).font(.custom("Bravura", size: 30)).frame(width: 52, height: 56)
                .offset(y: 8)
        }
        .buttonStyle(KeyStyle(on: on))
        .accessibilityIdentifier(id)
        .accessibilityLabel(label)
        .accessibilityAddTraits(on ? .isSelected : [])
    }

    private func textKey(_ symbol: String, id: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).font(.title3).frame(width: 52, height: 44) }
            .buttonStyle(KeyStyle(on: false))
            .accessibilityIdentifier(id)
            .accessibilityLabel(label)
    }
}

struct KeyStyle: ButtonStyle {
    let on: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(on ? Color.white : Color.primary)
            .background(RoundedRectangle(cornerRadius: 9).fill(on ? Color.accentColor : Color(.secondarySystemBackground)))
            .opacity(configuration.isPressed ? 0.6 : 1)
    }
}

import SwiftUI
import UIKit

/// Draws a track's engraving (the desktop's layout engine, through the core)
/// and turns touches into entry.
///
/// Fingers pan and pinch-zoom; the Pencil writes. Pencil down shows a shadow
/// note at the pitch under the tip, moving adjusts it, lifting enters it.
/// A finger tap enters a note at once (or selects the note under it), and a
/// finger press-and-hold behaves like the Pencil.
final class ScoreCanvasView: UIView {
    var document: ScoreDocument!
    var entry: EntryState!
    var trackId = ""
    var undoManagerProvider: () -> UndoManager? = { nil }
    var playhead: Double? { didSet { setNeedsDisplay() } }  // seconds, while playing

    /// Points per staff space, and staff spaces per quarter note.
    var scale: CGFloat = 9 { didSet { relayout() } }
    var spacesPerQuarter = 6.0
    /// Staff-space offset that keeps the drawing inside the view.
    private var origin = CGPoint.zero
    private var shadow: VCCPlacement?

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.985, alpha: 1)
        isAccessibilityElement = true
        accessibilityIdentifier = "score"
        accessibilityLabel = "Score"

        let pencil = UILongPressGestureRecognizer(target: self, action: #selector(press(_:)))
        pencil.minimumPressDuration = 0
        pencil.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
        addGestureRecognizer(pencil)

        let hold = UILongPressGestureRecognizer(target: self, action: #selector(press(_:)))
        hold.minimumPressDuration = 0.3
        hold.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        addGestureRecognizer(hold)

        let tap = UITapGestureRecognizer(target: self, action: #selector(tap(_:)))
        tap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        addGestureRecognizer(tap)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    /// Engraves again and resizes to fit (after an edit or a zoom).
    func relayout() {
        guard let document, !trackId.isEmpty else { return }
        let r = document.core.layoutTrack(trackId, spacesPerQuarter: spacesPerQuarter)
        origin = CGPoint(x: -r.minX + 2, y: -r.minY + 2)
        let size = CGSize(width: (r.width + 6) * scale, height: (r.height + 4) * scale)
        if frame.size != size { frame.size = size; (superview as? UIScrollView)?.contentSize = size }
        accessibilityValue = "\(document.core.performance().count) notes"
        if ProcessInfo.processInfo.arguments.contains("-UITestNewScore") {
            // For the UI tests: where beat 1 of bar 1 sits on the first staff's
            // middle line, and the width of a quarter note, in view points.
            let x = (document.core.x(forTick: 0, inTrack: trackId) + origin.x + 0.6) * scale
            let y = (document.core.y(forStaff: 0, position: 4, inTrack: trackId) + origin.y) * scale
            accessibilityLabel = String(format: "Score %.1f,%.1f,%.1f", x, y, spacesPerQuarter * scale)
        }
        setNeedsDisplay()
    }

    private func spaces(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x / scale - origin.x, y: p.y / scale - origin.y) }

    override func draw(_ rect: CGRect) {
        guard let ctx = UIGraphicsGetCurrentContext(), let document else { return }
        ctx.translateBy(x: origin.x * scale, y: origin.y * scale)
        let selected = Set(entry.selection.map { NSNumber(value: $0) })
        document.core.drawTrack(trackId, in: ctx, scale: scale, selected: selected,
                                color: UIColor.black.cgColor, highlight: UIColor.systemBlue.cgColor)
        if let s = shadow, s.valid {
            let x = document.core.x(forTick: s.tick, inTrack: trackId)
            let y = entry.restMode ? document.core.y(forStaff: s.staff, position: 4, inTrack: trackId)
                                   : document.core.y(forStaff: s.staff, position: s.staffPosition, inTrack: trackId)
            let glyph = entry.restMode ? Glyphs.rest(entry.value) : Glyphs.notehead(entry.value)
            document.core.drawGlyph(glyph, atX: x, y: y, in: ctx, scale: scale,
                                    color: UIColor.systemOrange.withAlphaComponent(0.75).cgColor)
        }
        if let t = playhead {
            // The playhead: seconds to ticks at the project tempo, then x.
            let tick = Int64(t * document.core.tempo / 60.0 * 960.0)
            let x = document.core.x(forTick: tick, inTrack: trackId) * scale
            ctx.setStrokeColor(UIColor.systemRed.cgColor)
            ctx.setLineWidth(2)
            ctx.move(to: CGPoint(x: x, y: -origin.y * scale))
            ctx.addLine(to: CGPoint(x: x, y: bounds.height))
            ctx.strokePath()
        }
    }

    // MARK: Entry

    private func placement(at point: CGPoint) -> VCCPlacement {
        let p = spaces(point)
        return document.core.placement(inTrack: trackId, x: p.x, y: p.y, grid: entry.value)
    }

    @objc private func press(_ g: UILongPressGestureRecognizer) {
        switch g.state {
        case .began, .changed:
            let p = placement(at: g.location(in: self))
            shadow = p
            entry.preview = p.valid ? (entry.restMode ? "Rest" : p.pitchName) : ""
            setNeedsDisplay()
        case .ended:
            if let s = shadow { commit(s) }
            fallthrough
        default:
            shadow = nil
            entry.preview = ""
            setNeedsDisplay()
        }
    }

    @objc private func tap(_ g: UITapGestureRecognizer) {
        let point = g.location(in: self)
        let p = spaces(point)
        let note = document.core.note(inTrack: trackId, x: p.x, y: p.y)
        if note != 0 {
            if entry.selection.contains(note) { entry.selection.remove(note) } else { entry.selection = [note] }
            entry.status = "Selected"
            setNeedsDisplay()
            return
        }
        entry.selection = []
        commit(placement(at: point))
    }

    private func commit(_ at: VCCPlacement) {
        guard at.valid else { return }
        let ok: Bool
        if entry.restMode {
            ok = document.core.enterRest(inTrack: trackId, at: at, value: entry.value, dots: entry.dots)
        } else if let a = entry.accidental {
            ok = document.core.enterNote(inTrack: trackId, at: at, value: entry.value, dots: entry.dots, alteration: a)
        } else {
            ok = document.core.enterNote(inTrack: trackId, at: at, value: entry.value, dots: entry.dots)
        }
        if ok {
            document.edited(undoManagerProvider())
            entry.status = entry.restMode ? "Rest entered" : "Entered \(at.pitchName)"
            Haptics.tick()
        } else {
            entry.status = "That does not fit there"
        }
        relayout()
    }
}

enum Haptics {
    static func tick() { UISelectionFeedbackGenerator().selectionChanged() }
}

/// The canvas in a scroll view: fingers pan; pinch zooms (re-engraved at the
/// new size, never a stretched bitmap); the Pencil never scrolls.
struct ScoreCanvas: UIViewRepresentable {
    @ObservedObject var document: ScoreDocument
    @ObservedObject var entry: EntryState
    var trackId: String
    var playhead: Double?
    @Environment(\.undoManager) private var undoManager

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIScrollView {
        let scroll = UIScrollView()
        scroll.backgroundColor = UIColor(white: 0.93, alpha: 1)
        scroll.panGestureRecognizer.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        scroll.pinchGestureRecognizer?.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        scroll.minimumZoomScale = 0.4
        scroll.maximumZoomScale = 3
        scroll.delegate = context.coordinator
        let canvas = ScoreCanvasView(frame: .zero)
        scroll.addSubview(canvas)
        context.coordinator.canvas = canvas
        return scroll
    }

    func updateUIView(_ scroll: UIScrollView, context: Context) {
        guard let canvas = context.coordinator.canvas else { return }
        canvas.document = document
        canvas.entry = entry
        canvas.undoManagerProvider = { undoManager }
        canvas.trackId = trackId
        canvas.playhead = playhead
        if context.coordinator.drawn != (document.revision, trackId) {
            context.coordinator.drawn = (document.revision, trackId)
            canvas.relayout()
        } else {
            canvas.setNeedsDisplay()
        }
    }

    final class Coordinator: NSObject, UIScrollViewDelegate {
        weak var canvas: ScoreCanvasView?
        var drawn: (UInt64, String) = (UInt64.max, "")
        func viewForZooming(in scrollView: UIScrollView) -> UIView? { canvas }
        func scrollViewDidEndZooming(_ scrollView: UIScrollView, with view: UIView?, atScale zoom: CGFloat) {
            guard let canvas else { return }
            let s = min(30, max(4, canvas.scale * zoom))
            scrollView.zoomScale = 1
            canvas.transform = .identity
            canvas.scale = s
        }
    }
}

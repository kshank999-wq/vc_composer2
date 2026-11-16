import PencilKit
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
    private var pencilEntry: UILongPressGestureRecognizer!
    private var holdEntry: UILongPressGestureRecognizer!

    // Handwriting (spec §6.2): an ink layer over the score, read when the
    // writer pauses, previewed, corrected, then committed.
    let ink = PKCanvasView()
    weak var inkController: InkController?
    private(set) var recognition: VCCRecognition?
    private var readTimer: Timer?
    private(set) var writing = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.985, alpha: 1)
        isAccessibilityElement = true
        accessibilityIdentifier = "score"
        accessibilityLabel = "Score"

        ink.backgroundColor = .clear
        ink.isOpaque = false
        ink.isScrollEnabled = false
        ink.drawingPolicy = .pencilOnly
        ink.tool = PKInkingTool(.pen, color: .systemBlue, width: 2.5)
        ink.isHidden = true
        ink.delegate = self
        ink.accessibilityIdentifier = "ink"
        addSubview(ink)

        let pencil = UILongPressGestureRecognizer(target: self, action: #selector(press(_:)))
        pencil.minimumPressDuration = 0
        pencil.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
        addGestureRecognizer(pencil)
        pencilEntry = pencil

        let hold = UILongPressGestureRecognizer(target: self, action: #selector(press(_:)))
        hold.minimumPressDuration = 0.3
        hold.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
        addGestureRecognizer(hold)
        holdEntry = hold

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
        ink.frame = bounds
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
        drawRecognition(ctx)
        if let t = playhead {
            // The playhead: seconds to ticks at the project tempo, then x.
            let tick = Int64(t * document.core.tempo / 60.0 * Double(VCCDocument.ticksPerQuarter()))
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
        if writing {
            fixRecognition(at: p)
            return
        }
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
    var ink: InkController
    var writing: Bool
    var fingerDraws: Bool
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
        canvas.inkController = ink
        ink.canvas = canvas
        if canvas.writing != writing || canvas.ink.drawingPolicy != (fingerDraws ? .anyInput : .pencilOnly) {
            canvas.setWriting(writing, fingerDraws: fingerDraws)
        }
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

// MARK: - Handwriting

extension ScoreCanvasView: PKCanvasViewDelegate {
    /// Write mode: the ink layer takes the Pencil (and, if chosen, fingers);
    /// tap entry steps aside.
    func setWriting(_ on: Bool, fingerDraws: Bool) {
        writing = on
        ink.isHidden = !on
        ink.isUserInteractionEnabled = on
        ink.drawingPolicy = fingerDraws ? .anyInput : .pencilOnly
        pencilEntry.isEnabled = !on
        holdEntry.isEnabled = !on
        if !on { clearInk() }
    }

    func canvasViewDrawingDidChange(_ canvasView: PKCanvasView) {
        // Read when the writer pauses, as a person pauses after a phrase.
        readTimer?.invalidate()
        readTimer = Timer.scheduledTimer(withTimeInterval: 0.8, repeats: false) { [weak self] _ in self?.readInk() }
    }

    func readInk() {
        guard let document, !trackId.isEmpty else { return }
        let strokes: [[NSValue]] = ink.drawing.strokes.map { stroke in
            stroke.path.interpolatedPoints(by: .distance(3)).map { point in
                NSValue(cgPoint: spaces(point.location.applying(stroke.transform)))
            }
        }
        recognition = strokes.isEmpty ? nil : document.core.recognize(inTrack: trackId, strokes: strokes)
        inkController?.show(recognition)
        setNeedsDisplay()
    }

    /// Accept: the previewed phrase into the score, as one undo step.
    func acceptInk() {
        guard let document, let r = recognition, !r.items.isEmpty else { return }
        if document.core.commit(r, inTrack: trackId) {
            document.edited(undoManagerProvider())
            entry.status = "Written: " + r.summary
            Haptics.tick()
            clearInk()
            relayout()
        } else {
            entry.status = "That could not be written there"
        }
    }

    func clearInk() {
        readTimer?.invalidate()
        ink.drawing = PKDrawing()
        recognition = nil
        inkController?.show(nil)
        setNeedsDisplay()
    }

    /// A tap on a doubtful head takes its other reading.
    fileprivate func fixRecognition(at p: CGPoint) {
        guard let r = recognition else { return }
        for (i, item) in r.items.enumerated() where item.hasAlternative {
            let x = document.core.x(forTick: item.tick, inTrack: trackId)
            let y = document.core.y(forStaff: item.staff, position: item.positions.first?.intValue ?? 0, inTrack: trackId)
            if abs(p.x - x) < 1.5 && abs(p.y - y) < 1.5 {
                r.useAlternative(at: i)
                inkController?.show(r)
                setNeedsDisplay()
                return
            }
        }
    }

    /// The preview: each symbol as it will be written, in orange; anything
    /// to check ringed in red with the reason.
    fileprivate func drawRecognition(_ ctx: CGContext) {
        guard let r = recognition, let document else { return }
        for item in r.items {
            let x = document.core.x(forTick: item.tick, inTrack: trackId)
            let colour = item.doubtful ? UIColor.systemRed : UIColor.systemOrange
            let value = VCCValue(rawValue: item.value) ?? .quarter
            if item.isRest {
                let y = document.core.y(forStaff: item.staff, position: 4, inTrack: trackId)
                document.core.drawGlyph(Glyphs.rest(value), atX: x, y: y, in: ctx, scale: scale, color: colour.cgColor)
                continue
            }
            for pos in item.positions {
                let y = document.core.y(forStaff: item.staff, position: pos.intValue, inTrack: trackId)
                document.core.drawGlyph(Glyphs.notehead(value), atX: x, y: y, in: ctx, scale: scale,
                                        color: colour.withAlphaComponent(0.85).cgColor)
            }
            if item.doubtful {
                let y = document.core.y(forStaff: item.staff, position: item.positions.first?.intValue ?? 4, inTrack: trackId)
                let ring = CGRect(x: (x - 0.6) * scale, y: (y - 1.2) * scale, width: 2.4 * scale, height: 2.4 * scale)
                ctx.setStrokeColor(UIColor.systemRed.cgColor)
                ctx.setLineWidth(2)
                ctx.strokeEllipse(in: ring)
                (item.why as NSString).draw(at: CGPoint(x: ring.minX, y: ring.maxY + 2),
                                            withAttributes: [.font: UIFont.systemFont(ofSize: 12, weight: .medium),
                                                             .foregroundColor: UIColor.systemRed])
            }
        }
    }
}

/// What the ink bar shows: the reading's summary and whether there is one.
final class InkController: ObservableObject {
    @Published private(set) var summary = ""
    @Published private(set) var ready = false
    @Published private(set) var toCheck = 0
    weak var canvas: ScoreCanvasView?

    func show(_ r: VCCRecognition?) {
        summary = r?.summary ?? ""
        ready = (r?.items.count ?? 0) > 0
        toCheck = r?.doubtfulCount ?? 0
    }
    func accept() { canvas?.acceptInk() }
    func clear() { canvas?.clearInk() }
    func read() { canvas?.readInk() }
}

import UIKit
import XCTest
@testable import VCComposerPad

/// The shared core inside the app: the bundled content loads, notes enter
/// through the desktop's score editor, files are the desktop's format, and
/// the engraving draws with Bravura.
final class BridgeTests: XCTestCase {
    func testEnterSaveAndReopenInTheDesktopFormat() throws {
        let doc = try Core.make()
        doc.newEmpty(withInstrument: "keys.piano")
        let track = try XCTUnwrap(doc.tracks.first)
        XCTAssertEqual(track.staves, 2)
        let bounds = doc.layoutTrack(track.identifier, spacesPerQuarter: 6)
        XCTAssertGreaterThan(bounds.width, 10)
        let y = doc.y(forStaff: 0, position: 4, inTrack: track.identifier)
        let at = doc.placement(inTrack: track.identifier, x: 0.6, y: y, grid: .quarter)
        XCTAssertTrue(at.valid)
        XCTAssertEqual(at.pitchName, "B4")
        XCTAssertTrue(doc.enterNote(inTrack: track.identifier, at: at, value: .quarter, dots: 0))
        XCTAssertEqual(doc.performance().count, 1)
        XCTAssertEqual(doc.undoLabel, "Enter note")

        let url = FileManager.default.temporaryDirectory.appendingPathComponent("BridgeTests.vcc2")
        try doc.save(toPath: url.path)
        let back = try Core.make()
        try back.load(fromPath: url.path)
        XCTAssertEqual(back.performance().first?.pitch, 71)
        XCTAssertTrue(doc.undo())
        XCTAssertEqual(doc.performance().count, 0)
    }

    func testTheDesktopTemplatesOpen() throws {
        let doc = try Core.make()
        try doc.newFromTemplate("cinematic.orchestra.consolidated")
        XCTAssertEqual(doc.tracks.count, 15)
    }

    func testBravuraIsRegisteredAndTheScoreDraws() throws {
        XCTAssertNotNil(UIFont(name: "Bravura", size: 20), "the music font is in the bundle and registered")
        let doc = try Core.make()
        doc.newEmpty(withInstrument: "woodwinds.flute")
        let track = try XCTUnwrap(doc.tracks.first)
        let r = doc.layoutTrack(track.identifier, spacesPerQuarter: 6)
        let scale: CGFloat = 8
        let image = UIGraphicsImageRenderer(size: CGSize(width: (r.width + 4) * scale, height: (r.height + 4) * scale)).image { c in
            UIColor.white.setFill()
            c.fill(c.format.bounds)
            c.cgContext.translateBy(x: (-r.minX + 2) * scale, y: (-r.minY + 2) * scale)
            doc.drawTrack(track.identifier, in: c.cgContext, scale: scale, selected: [], color: UIColor.black.cgColor,
                          highlight: UIColor.blue.cgColor)
        }
        // Dark pixels: staff lines and the clef were drawn.
        let cg = try XCTUnwrap(image.cgImage)
        let data = try XCTUnwrap(cg.dataProvider?.data as Data?)
        var dark = 0
        stride(from: 0, to: data.count, by: 4 * 3).forEach { i in if data[i] < 80 { dark += 1 } }
        XCTAssertGreaterThan(dark, 200)
    }
}

/// Handwriting through the bridge: strokes in, a preview out, committed.
final class HandwritingTests: XCTestCase {
    private func line(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double) -> [NSValue] {
        (0...12).map { i in
            let t = Double(i) / 12
            return NSValue(cgPoint: CGPoint(x: x1 + (x2 - x1) * t, y: y1 + (y2 - y1) * t))
        }
    }
    private func scribble(_ cx: Double, _ cy: Double) -> [NSValue] {
        (0...10).map { i in
            let y = cy - 0.4 + 0.08 * Double(i)
            let half = 0.6 * max(0.22, (1 - pow((y - cy) / 0.45, 2)).squareRoot())
            return NSValue(cgPoint: CGPoint(x: i % 2 == 1 ? cx + half : cx - half, y: y))
        }
    }

    func testAHandwrittenQuarterIsPreviewedThenWritten() throws {
        let doc = try Core.make()
        doc.newEmpty(withInstrument: "woodwinds.flute")
        let track = try XCTUnwrap(doc.tracks.first)
        _ = doc.layoutTrack(track.identifier, spacesPerQuarter: 6)
        let middle = doc.y(forStaff: 0, position: 4, inTrack: track.identifier)
        let r = doc.recognize(inTrack: track.identifier,
                              strokes: [scribble(0.4, middle), line(1.0, middle, 1.0, middle - 3.4)])
        XCTAssertEqual(r.items.count, 1)
        XCTAssertEqual(r.items.first?.value, VCCValue.quarter.rawValue)
        XCTAssertEqual(r.items.first?.positions.first?.intValue, 4)
        XCTAssertEqual(r.doubtfulCount, 0)
        XCTAssertEqual(doc.performance().count, 0, "a preview writes nothing")
        XCTAssertTrue(doc.commit(r, inTrack: track.identifier))
        XCTAssertEqual(doc.performance().first?.pitch, 71)
        XCTAssertEqual(doc.undoLabel, "Handwrite 1 note")
    }
}

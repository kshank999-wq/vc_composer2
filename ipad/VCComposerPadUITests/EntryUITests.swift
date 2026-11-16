import XCTest

/// Tap entry on the iPad, as a person does it: pick a value on the keypad,
/// tap the staff, undo. Screenshots go to $SCREENSHOT_DIR when it is set.
final class EntryUITests: XCTestCase {
    override func setUp() { continueAfterFailure = false }

    func testTapEntryUndoAndKeypad() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-UITestNewScore"]
        app.launch()
        let score = app.otherElements["score"]
        XCTAssertTrue(score.waitForExistence(timeout: 20))
        XCTAssertEqual(score.value as? String, "0 notes")
        shot(app, "01-new-score")

        // Where the first beat and a quarter note's width are, from the canvas.
        let parts = score.label.replacingOccurrences(of: "Score ", with: "").split(separator: ",").compactMap { Double($0) }
        XCTAssertEqual(parts.count, 3, "the canvas publishes its staff position for tests")
        let (x, y, step) = (parts[0], parts[1], parts[2])
        let origin = score.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0))

        app.buttons["quarter"].tap()
        origin.withOffset(CGVector(dx: x, dy: y)).tap()
        XCTAssertTrue(waitFor(score, value: "1 notes"))
        origin.withOffset(CGVector(dx: x + step, dy: y - step / 6)).tap()  // a step higher, next beat
        XCTAssertTrue(waitFor(score, value: "2 notes"))
        app.buttons["eighth"].tap()
        origin.withOffset(CGVector(dx: x + 2 * step, dy: y + step / 6)).tap()
        XCTAssertTrue(waitFor(score, value: "3 notes"))
        shot(app, "02-three-notes")

        app.buttons["undo"].tap()
        XCTAssertTrue(waitFor(score, value: "2 notes"))
        app.buttons["redo"].tap()
        XCTAssertTrue(waitFor(score, value: "3 notes"))

        // Select the first note and delete it.
        origin.withOffset(CGVector(dx: x, dy: y)).tap()
        app.buttons["delete"].tap()
        XCTAssertTrue(waitFor(score, value: "2 notes"))
        shot(app, "03-after-delete")
    }

    private func waitFor(_ e: XCUIElement, value: String) -> Bool {
        let p = NSPredicate(format: "value == %@", value)
        return XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: p, object: e)], timeout: 5) == .completed
    }

    private func shot(_ app: XCUIApplication, _ name: String) {
        let s = app.screenshot()
        let a = XCTAttachment(screenshot: s)
        a.name = name
        a.lifetime = .keepAlways
        add(a)
        if let dir = ProcessInfo.processInfo.environment["SCREENSHOT_DIR"], !dir.isEmpty {
            try? s.pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
        }
    }
}

import SwiftUI

/// Picks the app: the document browser normally; a fresh score straight away
/// for the UI tests.
@main
enum Launcher {
    static func main() {
        if ProcessInfo.processInfo.arguments.contains("-UITestNewScore") {
            TestingApp.main()
        } else {
            VCComposerPadApp.main()
        }
    }
}

struct VCComposerPadApp: App {
    var body: some Scene {
        DocumentGroup(newDocument: { ScoreDocument() }) { file in
            ScoreScreen(document: file.document)
        }
    }
}

struct TestingApp: App {
    var body: some Scene {
        WindowGroup { ScoreScreen(document: ScoreDocument()) }
    }
}

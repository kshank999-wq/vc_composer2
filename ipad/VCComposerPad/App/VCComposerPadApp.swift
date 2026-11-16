import SwiftUI

@main
struct VCComposerPadApp: App {
    /// UI tests open a fresh score straight away instead of the document browser.
    private let testing = ProcessInfo.processInfo.arguments.contains("-UITestNewScore")

    var body: some Scene {
        if testing {
            WindowGroup { ScoreScreen(document: ScoreDocument()) }
        } else {
            DocumentGroup(newDocument: { ScoreDocument() }) { file in
                ScoreScreen(document: file.document)
            }
        }
    }
}

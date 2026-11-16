import SwiftUI
import UniformTypeIdentifiers

extension UTType {
    /// The desktop's project file (.vcc2), unchanged: a score saved here opens
    /// on the desktop as it is, and the other way round.
    static let vccProject = UTType(exportedAs: "com.vccomposer.project")
}

/// The shared core with the app's bundled content.
enum Core {
    static func make() throws -> VCCDocument {
        guard let content = Bundle.main.path(forResource: "content", ofType: nil) else {
            throw CocoaError(.fileNoSuchFile, userInfo: [NSLocalizedDescriptionKey: "The app's content folder is missing"])
        }
        let metadata = Bundle.main.path(forResource: "bravura_metadata", ofType: "json") ?? ""
        return try VCCDocument(contentDirectory: content, fontMetadata: metadata)
    }
}

/// One open project. Every edit goes through the core (the desktop's score
/// editor) and is registered with the window's undo manager, which is also
/// what tells the system the document needs saving.
final class ScoreDocument: ReferenceFileDocument {
    static var readableContentTypes: [UTType] { [.vccProject] }
    static var writableContentTypes: [UTType] { [.vccProject] }

    let core: VCCDocument
    /// Moves on every change; views redraw when it does.
    @Published private(set) var revision: UInt64 = 0

    init() {
        // The bundled content is part of the app: without it nothing works.
        core = try! Core.make()  // swiftlint:disable:this force_try
        core.newEmpty(withInstrument: "keys.piano")
        revision = core.revision
    }

    required init(configuration: ReadConfiguration) throws {
        core = try Core.make()
        guard let data = configuration.file.regularFileContents else { throw CocoaError(.fileReadCorruptFile) }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".vcc2")
        try data.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        try core.load(fromPath: url.path)
        revision = core.revision
    }

    func snapshot(contentType: UTType) throws -> Data {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".vcc2")
        defer { try? FileManager.default.removeItem(at: url) }
        try core.save(toPath: url.path)
        return try Data(contentsOf: url)
    }

    func fileWrapper(snapshot: Data, configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: snapshot)
    }

    /// Call after a successful core edit: redraw, and make it undoable here.
    func edited(_ undoManager: UndoManager?) {
        revision = core.revision
        register(undoManager, undo: true, name: core.undoLabel)
    }

    private func register(_ undoManager: UndoManager?, undo: Bool, name: String) {
        undoManager?.registerUndo(withTarget: self) { doc in
            _ = undo ? doc.core.undo() : doc.core.redo()
            doc.revision = doc.core.revision
            doc.register(undoManager, undo: !undo, name: name)
        }
        undoManager?.setActionName(name)
    }
}

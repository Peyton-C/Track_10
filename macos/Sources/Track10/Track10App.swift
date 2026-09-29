import SwiftUI
import UniformTypeIdentifiers

@main
struct Track10App: App {
    var body: some Scene {
        DocumentGroup(viewing: StemDocument.self) { file in
            if let url = file.fileURL {
                PlayerWindow(url: url)
            }
        }
        .windowResizability(.contentSize)
    }
}

/// Stands in for a stem file. The player reads the file itself from its URL,
/// so nothing is loaded here.
struct StemDocument: FileDocument {
    static let readableContentTypes: [UTType] = [.mpeg4Movie, .mpeg4Audio]

    init(configuration: ReadConfiguration) throws {}

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        throw CocoaError(.featureUnsupported)
    }
}

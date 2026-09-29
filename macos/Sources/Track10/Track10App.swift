import AppKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct Track10App: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        // Player windows are plain AppKit windows (see AppDelegate), so this
        // scene only exists to carry the menu commands.
        Settings { EmptyView() }
            .commands {
                CommandGroup(replacing: .newItem) {
                    Button("Open…") { appDelegate.showOpenPanel() }
                        .keyboardShortcut("o")
                }
            }
    }
}

/// Opens one window per file. DocumentGroup would do this too, but it treats
/// files as editable documents, adding a proxy icon with a rename popover to
/// the title bar and Rename, Duplicate and Revert to the File menu.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var windows: [NSWindow] = []
    private var players: [ObjectIdentifier: PlayerModel] = [:]
    private var openedAtLaunch = false

    func applicationWillFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Files opened from Finder arrive around launch; ask for one only
        // when the app was started on its own.
        DispatchQueue.main.async {
            if !self.openedAtLaunch && self.windows.isEmpty {
                self.showOpenPanel()
            }
        }
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        openedAtLaunch = true
        urls.forEach(open)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        if !hasVisibleWindows {
            showOpenPanel()
        }
        return false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func showOpenPanel() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.mpeg4Movie, .mpeg4Audio]
        panel.allowsMultipleSelection = true
        panel.message = "Choose stem files to play"
        guard panel.runModal() == .OK else {
            if windows.isEmpty {
                NSApp.terminate(nil)
            }
            return
        }
        panel.urls.forEach(open)
    }

    func open(_ url: URL) {
        if let window = windows.first(where: { $0.identifier?.rawValue == url.path }) {
            window.makeKeyAndOrderFront(nil)
            return
        }

        // Open the file before making the window, so the window is created at
        // its final size. Letting SwiftUI resize it to fit afterwards can send
        // AppKit into a layout loop that it ends by throwing.
        guard let model = loadPlayer(url) else {
            if windows.isEmpty {
                NSApp.terminate(nil)
            }
            return
        }

        let window = NSWindow(contentRect: .zero, styleMask: [.titled, .closable, .miniaturizable],
                              backing: .buffered, defer: false)
        let host = NSHostingController(rootView: playerView(model, in: window))
        host.sizingOptions = []
        host.safeAreaRegions = []
        window.contentViewController = host
        window.setContentSize(Self.contentSize(of: host))
        window.isReleasedWhenClosed = false
        window.delegate = self
        if let last = windows.last {
            window.setFrameTopLeftPoint(last.cascadeTopLeft(from: NSPoint(x: last.frame.minX, y: last.frame.maxY)))
        } else {
            window.center()
        }
        windows.append(window)
        show(model, playing: url, in: window)
        window.makeKeyAndOrderFront(nil)
    }

    /// Swaps the track playing in `window`, for a file dropped onto it.
    func replace(_ window: NSWindow, with url: URL) {
        guard window.identifier?.rawValue != url.path, let model = loadPlayer(url),
              let host = window.contentViewController as? NSHostingController<PlayerView>
        else { return }

        players.removeValue(forKey: ObjectIdentifier(window))?.close()
        host.rootView = playerView(model, in: window)

        // Resize explicitly, keeping the top edge still, as tracks without an
        // artist or with a long title are shorter or taller.
        let size = Self.contentSize(of: host)
        var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        frame.origin = NSPoint(x: window.frame.minX, y: window.frame.maxY - frame.height)
        window.setFrame(frame, display: true, animate: true)
        show(model, playing: url, in: window)
    }

    /// Measures the view from its current rootView. The hosting view's fittingSize
    /// reads 0 until SwiftUI lays out a new rootView, collapsing the window.
    private static func contentSize(of host: NSHostingController<PlayerView>) -> NSSize {
        host.sizeThatFits(in: NSSize(width: 640, height: CGFloat.greatestFiniteMagnitude))
    }

    private func loadPlayer(_ url: URL) -> PlayerModel? {
        do {
            return try PlayerModel(url: url)
        } catch {
            let alert = NSAlert()
            alert.messageText = "Can’t Play “\(url.lastPathComponent)”"
            alert.informativeText = error.localizedDescription
            alert.runModal()
            return nil
        }
    }

    private func playerView(_ model: PlayerModel, in window: NSWindow) -> PlayerView {
        PlayerView(model: model) { [weak self, weak window] url in
            guard let self, let window else { return }
            self.replace(window, with: url)
        }
    }

    private func show(_ model: PlayerModel, playing url: URL, in window: NSWindow) {
        players[ObjectIdentifier(window)] = model
        window.title = url.lastPathComponent
        window.identifier = NSUserInterfaceItemIdentifier(url.path)
        NSDocumentController.shared.noteNewRecentDocumentURL(url)
        model.togglePlay()
    }

    func windowWillClose(_ notification: Notification) {
        guard let window = notification.object as? NSWindow else { return }
        players.removeValue(forKey: ObjectIdentifier(window))?.close()
        windows.removeAll { $0 === window }
    }
}

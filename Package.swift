// swift-tools-version: 6.2
// The macOS app. The C core is compiled straight from core/, linking against
// the system GStreamer found through pkg-config (Homebrew's, normally).
import PackageDescription

let package = Package(
    name: "Track10",
    platforms: [.macOS(.v26)],
    targets: [
        .systemLibrary(
            name: "CGStreamer",
            path: "macos/CGStreamer",
            pkgConfig: "gstreamer-1.0",
            providers: [.brew(["gstreamer"])]
        ),
        .target(
            name: "Track10Core",
            dependencies: ["CGStreamer"],
            path: "core",
            exclude: ["tools"],
            sources: ["src"],
            publicHeadersPath: "include"
        ),
        .executableTarget(
            name: "Track10",
            dependencies: ["Track10Core"],
            path: "macos/Sources/Track10"
        ),
    ],
    swiftLanguageModes: [.v5]
)

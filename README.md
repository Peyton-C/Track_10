# Track 10
A minimalist NI format stem player.

Opens a `.stem.mp4` in a small window modelled on macOS Quick Look's audio preview, with a volume slider for each stem. Click a stem's name to mute it, Option-click (Ctrl-click on Linux) to solo it.

## Build
Both frontends use the system GStreamer rather than bundling it.

### macOS
Needs Xcode (not just the Command Line Tools, which lack SwiftUI's macros) and GStreamer:

```sh
brew install gstreamer
macos/bundle.sh            # build/macos/Track 10.app
macos/bundle.sh --install  # and copy it to /Applications
```

### Linux
Needs GTK 4.10+, GStreamer with the base, good and libav plugins, and Meson:

```sh
sudo apt install libgtk-4-dev libgstreamer1.0-dev gstreamer1.0-plugins-good gstreamer1.0-libav meson   # Debian/Ubuntu
sudo dnf install gtk4-devel gstreamer1-devel gstreamer1-plugins-good gstreamer1-libav meson            # Fedora

meson setup build
ninja -C build
./build/linux/track10 song.stem.mp4
sudo ninja -C build install   # optional: installs the app, desktop entry and icon
```

## Layout

| Path | What |
| --- | --- |
| `core/` | The C player both frontends share: one GStreamer pipeline mixing every stem, plus a reader for the stem names, colours, tags and cover art |
| `macos/` | SwiftUI frontend |
| `linux/` | GTK 4 frontend |

`build/t10-info` (built by Meson) prints what the core reads from a file, and `t10-info --test` plays a few seconds through it. Set `T10_AUDIO_SINK=fakesink` to test silently.

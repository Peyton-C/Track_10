#!/bin/sh
# Builds "Track 10.app" into build/macos. GStreamer is not bundled, the app
# links against Homebrew's, so it runs on machines with `brew install
# gstreamer`.
#
#   macos/bundle.sh            release build
#   macos/bundle.sh --install  also copies the app to /Applications
set -eu

cd "$(dirname "$0")/.."

# SwiftUI's macros ship with Xcode, not the Command Line Tools, so prefer
# Xcode when xcode-select points at the latter.
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app ]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi

swift build -c release --product Track10
bin="$(swift build -c release --show-bin-path)/Track10"

app="build/macos/Track 10.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/Track10"
cp macos/Info.plist "$app/Contents/Info.plist"

xcrun actool assets/track_10_icon/track_10_icon.icon \
    --compile "$app/Contents/Resources" \
    --app-icon track_10_icon \
    --enable-on-demand-resources NO \
    --development-region en \
    --target-device mac \
    --platform macosx \
    --minimum-deployment-target 26.0 \
    --output-partial-info-plist build/macos/icon.plist >/dev/null

codesign --force --sign - "$app"
echo "Built $app"

if [ "${1:-}" = "--install" ]; then
    rm -rf "/Applications/Track 10.app"
    cp -R "$app" /Applications/
    echo "Installed to /Applications/Track 10.app"
fi

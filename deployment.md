# macOS build / sign / export — generic (JUCE app or plugin, this machine)
Developer / company: CitizenOfTomorrow

## Machine constraints
- Command Line Tools only, no full Xcode → use CMake with the default Makefiles/Ninja generator
  (no `-G Xcode`, no xcodebuild, no .jucer/Projucer exports).
- /usr/local/bin/cmake is an Intel build running under Rosetta → ALWAYS set
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. Without it binaries are x86_64-only and arm64 hosts
  (Ableton Live 12, Logic) silently won't load plugins.
- JUCE 8.0.12: /Users/OliverHeinrich_1/JUCE/JUCE (use via add_subdirectory). JUCE 8 quirks: open MIDI
  devices on the message thread; JUCE's MidiOutput SysEx may not reach hardware (use CoreMIDI directly if needed);
  use FontOptions instead of the deprecated Font(size, style) ctor.

## Identity in the project
juce_add_plugin(<Target>                      # or juce_add_gui_app
    COMPANY_NAME "CitizenOfTomorrow"
    BUNDLE_ID "com.citizenoftomorrow.<product>"
    PLUGIN_MANUFACTURER_CODE Ctzt             # 4 chars, ≥1 uppercase; keep identical across all your plugins
    PLUGIN_CODE <4 unique chars per plugin>
    ...)
- Never change BUNDLE_ID / manufacturer / plugin codes after release (hosts lose saved instances).

## Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build --target <Target>_VST3 <Target>_AU <Target>_Standalone -j8   # plugin
cmake --build build --target <Target> -j8                                          # juce_add_gui_app
- Plugins: COPY_PLUGIN_AFTER_BUILD TRUE → auto-install to ~/Library/Audio/Plug-Ins/{VST3,Components}.
- Artefacts: build/<Target>_artefacts/Release/{VST3,AU,Standalone}/...
- Keep build dirs out of git (.gitignore: build*/, .DS_Store, __pycache__/).

## Verify
- lipo -archs "<bundle>/Contents/MacOS/<binary>"        → "x86_64 arm64"
- codesign -dv "<bundle>"                               → shows the signing identity
- AU: killall -9 AudioComponentRegistrar; auval -v <type> <subtype> Ctzt   → "AU VALIDATION SUCCEEDED"
  (type: aufx effect, aumu instrument, aumi MIDI FX)
- Plain-C++ logic: keep it JUCE-free and unit-test with clang++ -fsanitize=address,undefined / ctest.
- UIs can't be opened headlessly: render components to PNG via a small juce_add_console_app.

## Sign (as CitizenOfTomorrow)
- Requires the "Developer ID Application" certificate for CitizenOfTomorrow in the login keychain.
  Currently NONE is installed (`security find-identity -v -p codesigning` → 0). The user must create it in
  their Apple Developer account and install it; never handle certificates, passwords or Apple IDs yourself.
- Confirm the exact identity string first:
  security find-identity -v -p codesigning     # e.g. "Developer ID Application: CitizenOfTomorrow (ABCDE12345)"
- Sign each bundle/app (inner binaries first is handled by --deep for simple JUCE bundles):
  codesign --force --deep --timestamp --options runtime \
    --sign "Developer ID Application: CitizenOfTomorrow (<TEAMID>)" "<bundle or .app>"
  Standalone apps using audio input: add --entitlements <file> with com.apple.security.device.audio-input.
- Without the certificate, builds stay ad-hoc signed (fine for this Mac only).

## Notarize (distribution only)
ditto -c -k --keepParent "<bundle>" out.zip
xcrun notarytool submit out.zip --keychain-profile CitizenOfTomorrow --wait
  # profile created once by the user: xcrun notarytool store-credentials CitizenOfTomorrow
xcrun stapler staple "<bundle>"
spctl -a -vvv -t install "<bundle>"     # expect: accepted, source=Notarized Developer ID

## Export
- Zip with ditto (keeps signatures): ditto -c -k --keepParent "<bundle>" <Product>-<version>-macOS.zip
- Install paths: VST3 → ~/Library/Audio/Plug-Ins/VST3, AU → ~/Library/Audio/Plug-Ins/Components,
  apps → /Applications.
- After installing plugins: restart the DAW; Live: Option-click Rescan; Logic: Plug-in Manager → Reset & Rescan.

## Housekeeping
- Don't commit unless asked; branch first if on main. Repos here may auto-stage (Jujutsu .jj dir present).
- Report what was verified (arch, signature, auval, tests) vs. what wasn't (in-DAW behaviour).
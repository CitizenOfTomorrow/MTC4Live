# MTC4Live

An audio effect plugin (AU and VST3, macOS) that reads Ableton Live's transport and sends MIDI Time Code (MTC). It sends straight to a CoreMIDI port of its own, so it does not depend on Live's MIDI routing. Audio passes through unchanged.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build -j8
```

`COPY_PLUGIN_AFTER_BUILD` installs the plugin into `~/Library/Audio/Plug-Ins/{Components,VST3}`. In Live, Option-click Rescan, or restart Live. Signing and notarization follow `deployment.md`. Until a Developer ID certificate is installed, builds are ad-hoc signed and only run on this Mac.

| Identity | |
|---|---|
| Bundle ID | `com.citizenoftomorrow.mtc4live` |
| AU | `aufx Mtc4 Ctzt` |

## Use in Live

1. Put **MTC4Live** on any audio track (an empty audio track works), or on the Master track. Use **one active instance** per project. More than one will not crash, but each one sends its own MTC.
2. **Output**:
   - **Virtual port: Live MTC** (the default) creates a CoreMIDI source called "Live MTC". Choose it as the MTC input in the receiving app. No IAC setup is needed.
   - Or pick a hardware or IAC destination. If the device is unplugged, the plugin closes it and reopens it when the device comes back.
   - **Refresh** rescans the device list.
3. **Frame rate**: 24, 25 (default), 29.97 drop-frame, or 30.
4. **Start offset**: the timecode at Live's position 0. The default is `01:00:00:00`.
5. **Latency (ms)**: a positive value sends MTC earlier. Use it to match the receiver to the audio you hear. Live calls the plugin about one buffer ahead of the audio output, so with no offset, MTC runs early by roughly your output latency. A negative value delays MTC.
6. **Active** turns sending on or off. **Full frame when stopped** keeps the receiver parked on the stopped position, and it follows locates while stopped.

All settings are saved with the Live set.

## How it works

- **Audio thread** (`processBlock`): reads the play head into a lock-free snapshot `{playing, position, wall time, discontinuity counter}`. It does no MIDI, no locking and no allocation. The position comes from `getTimeInSeconds()`. If that is missing, it is derived from PPQ, with tempo changes integrated so they do not look like jumps. A start, or a position jump of more than half a frame (a locate or a loop wrap), bumps the discontinuity counter. Offline render (freeze or export) counts as stopped.
- **Sender thread** (`MtcSender`, real-time time-constraint priority): converts host position to wall time with a smoothed anchor, so it stays locked to the audio clock rather than drifting with wall time. It hands each quarter frame to CoreMIDI about 20 ms early, stamped with its exact due time. CoreMIDI holds the message and delivers it on time, including on the virtual port, so a late thread wakeup inside a busy Live doesn't show up as jitter. The cost: after a locate on the virtual port, up to 20 ms of stale quarter frames still go out. The full frame is always stamped after them, so the receiver ends up in the right place. Hardware and IAC outputs drop those stale messages instead. When the discontinuity counter changes, or the rate, offset or port changes, it sends a full-frame SysEx and restarts the cycle at piece 0 on the next frame boundary. If no audio block arrives for 250 ms, it treats the transport as stopped (track frozen, device off, engine halted).
- **MIDI output** (`CoreMidiOutput`): CoreMIDI is called directly, through the legacy `MIDIPacketList` API. JUCE 8's `MidiOutput` converts to UMP, and its SysEx does not reliably reach some hardware. Ports are opened and closed on the message thread. Sending happens on the sender thread.
- **Timecode maths** (`TimecodeMath`, `MtcMessages`): pure C++ with no JUCE dependency. It covers drop-frame and 24-hour wrap. Every piece of a quarter-frame cycle encodes the frame on which piece 0 was sent, and the receiver adds the 2 frames.

## Tests

```bash
ctest --test-dir build
```

This runs the unit tests: every frame of 24 h round-tripped at all four rates, drop-frame boundaries, message bytes, and a simulated receiver decoding the stream. It also runs the same tests under ASan and UBSan.

```bash
./build/MTC4LiveTool_artefacts/Release/MTC4LiveTool e2e
```

The end-to-end test takes about 45 s. It drives the real processor in real time with a fake play head through these steps: play, locate, two loop wraps, stop, play, stop. It receives the virtual port over CoreMIDI and decodes the MTC. Checks, at every rate:

- full frames on each start, locate and loop wrap
- pieces arrive in order
- every piece lands within 5 ms of the host position
- nothing is sent after stop
- audio is bit-identical

Typical result: sd about 0.4 ms, maximum about 2–3 ms.

```bash
./build/MTC4LiveTool_artefacts/Release/MTC4LiveTool monitor
```

This is a live MTC reader for checking the plugin inside Live without other software. It waits for the "Live MTC" port, and reconnects if the port is recreated. To read another source, pass its name as an argument. Each second it prints:

- the decoded timecode and frame rate
- quarter frames per second
- interval jitter, read from the CoreMIDI timestamps
- the latest arrival behind a message's timestamp

Every full frame (start, locate, loop) gets its own line. Ctrl-C quits. Healthy output shows about 96–120 QF/s, jitter well under 1 ms, and no SEQUENCE ERRORS.

```bash
./build/MTC4LiveTool_artefacts/Release/MTC4LiveTool snapshot ui.png
```

This renders the editor to a PNG.

## Still to verify in Live

- [ ] Loads as AU and as VST3 in Live 11/12. Audio is unchanged.
- [ ] MIDI Monitor (Snoize) on "Live MTC" shows F1 quarter frames while playing, and F0 7F 7F 01 01 … on start, locate and loop.
- [ ] A receiver (Reaper, Logic, QLab or hardware) locks within about 1 s at all four rates.
- [ ] Locate and loop in Live make the receiver relocate.
- [ ] 29.97 DF matches a reference generator over at least 10 minutes.
- [ ] Sample rates 44.1, 48 and 96 kHz, buffers 64 to 1024, tempo changes, rapid start/stop.
- [ ] Settings survive saving and reloading the set. Unplugging and replugging the output device recovers.
- [ ] Freezing or disabling the track stops MTC cleanly.

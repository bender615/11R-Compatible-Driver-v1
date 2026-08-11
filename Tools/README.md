# Diagnostic tools

These standalone programs record the clean-room USB, audio, MIDI, and rig-data
observations used by the DriverKit implementation. They are intentionally kept
outside the signed driver targets.

Build all 13 executables and the arm64 probe object on macOS:

```sh
make -C Tools
```

The results are written to `Tools/build/`, which is ignored by Git.

## Tool groups

- `erprobe`, `speedcheck`: enumerate the Eleven Rack and its USB interfaces.
- `erstream`, `erduplex`, `erformat`, `erlanes`: inspect the isochronous audio
  transport and sample layout.
- `erengine`, `ersweep`, `ringstat`: exercise the user-space audio engine and
  its shared ring buffer.
- `ermidi_test`, `ermidi_bridge`: test raw USB-MIDI and expose the Rig/External
  CoreMIDI ports.
- `errig_read`: read and decode the edit-buffer rig name or Rig Vol value.
- `gtr_live_probe`, `gtr_probe_bytes.s`: local interoperability research aids;
  they do not distribute or link vendor binaries into the project.

## Safety and privacy

Several tools open or seize a USB interface and will temporarily conflict with
another audio/MIDI driver or application. Stop audio software first and keep
speaker/monitor levels low. Some tools stream silence or low-level test audio;
read the source header before running one on connected equipment.

Rig dumps and SysEx captures can contain personal names and settings. They are
ignored by Git and should not be attached to issues unless deliberately
sanitized.

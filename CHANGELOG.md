# Changelog

All notable changes to this fork will be documented here.

## Unreleased

- Added native AudioDriverKit and USBDriverKit project scaffolding for Apple
  Silicon and Intel Macs.
- Added 8-input/6-output Core Audio topology and channel names.
- Added runtime 44.1, 48, 88.2, and 96 kHz format selection.
- Implemented asynchronous USB isochronous capture and playback transport.
- Added a user-space CoreMIDI bridge for Rig and External MIDI ports.
- Added read-only edit-buffer, rig-name, and Rig Vol diagnostics.
- Added connected-device validation notes and strict diagnostic builds.

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
- Added hardware-backed Internal, AES/EBU, and S/PDIF clock selection and
  readback, plus external-rate following in the macOS control app.
- Added USB-frame-anchored Core Audio timestamps, published transport latency
  and safety offsets, and stalled-stream/external-clock recovery monitoring.
- Expanded edit-buffer decoding to cover the complete rig structure, amplifier,
  cabinet, microphone, effect identities, enable states, and known parameter
  display curves.
- Added paced, resumable Factory/User rig inventory, exhaustive CSV export, and
  comparative analysis tools while keeping raw captures outside the repository.
- Documented the rig data model and preserved the Apache-2.0 notice for the
  ElevenHack protocol reference material.
- Added connected-device validation notes and strict diagnostic builds.

# Notices and attribution

## Primary work

This Apple Silicon Eleven Rack compatibility driver and its original additions
are Copyright © 2026 Paul Bender and distributed under the MIT license in
[`LICENSE.txt`](LICENSE.txt).

The work developed for this repository includes the AudioDriverKit and
USBDriverKit extension, asynchronous isochronous audio transport, named
8-input/6-output Core Audio topology, switchable 44.1/48/88.2/96 kHz operation,
USB-MIDI/CoreMIDI bridging, read-only rig-name and Rig Vol retrieval, protocol
decoding, and the associated hardware validation and documentation.

## Upstream starting point

Parts of the work began from or were informed by Matt Housley's Eleven Rack
application and user-space driver code. Matt Housley is credited as the author
and contributor of that beginning base code. His copyright and MIT permission
notice are preserved verbatim in
[`LICENSES/Matt-Housley-MIT.txt`](LICENSES/Matt-Housley-MIT.txt). That notice
continues to apply to portions copied or derived from his work.

The inherited project also identified portions of its Xcode structure and
AudioDriverKit lifecycle as derived from Apple's “Creating an audio device
driver” sample and included an Avid MIT notice. That notice is preserved
verbatim in
[`LICENSES/Avid-Sample-Code-MIT.txt`](LICENSES/Avid-Sample-Code-MIT.txt).

No Avid application binaries, installer packages, presets, artwork, or other
proprietary assets are included in this repository.

Eleven Rack, Avid, Apple, macOS, and other names and marks belong to their
respective owners. This is an independent interoperability project and is not
affiliated with, endorsed by, or supported by Avid or Apple.

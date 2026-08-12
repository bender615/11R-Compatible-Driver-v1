# Eleven Rack Driver for Apple Silicon

Native macOS AudioDriverKit/USBDriverKit driver project for the Avid Eleven Rack
(USB vendor `0x0DBA`, product `0xB011`). This DriverKit implementation is
developed by Paul Bender. Parts began from or were informed by Matt Housley's
MIT-licensed Eleven Rack application and user-space driver, which is credited
as an upstream starting point rather than as the author of the new DriverKit,
MIDI rig-reading, and protocol-decoding work.

> [!IMPORTANT]
> This is experimental, independently developed compatibility software. The
> DriverKit extension compiles but still requires Apple-granted entitlements,
> signing, installation, and full live validation before normal use.

The Core Audio device is declared as:

- 8 inputs: Guitar, Mic, Eleven Rig L/R, Digital L/R, Line L/R
- 6 outputs: Main L/R, Re-Amp L/R, Digital L/R
- 44.1, 48, 88.2, and 96 kHz, with 32-bit floating-point Core Audio buffers

Core Audio channel map (the numeric order is part of the compatibility contract):

| Channel | Input name | Channel | Output name |
| ---: | --- | ---: | --- |
| 1 | Guitar Input | 1 | Main Output L |
| 2 | Mic Input | 2 | Main Output R |
| 3 | Eleven Rig L | 3 | Re-Amp L |
| 4 | Eleven Rig R | 4 | Re-Amp R |
| 5 | Digital Input L | 5 | Digital Output L |
| 6 | Digital Input R | 6 | Digital Output R |
| 7 | Line Input L |  |  |
| 8 | Line Input R |  |  |

The hardware MIDI transport is USB interface 2 (Audio class, MIDIStreaming
subclass), using bulk OUT endpoint `0x02` and bulk IN endpoint `0x82`, both with
512-byte packets. Current macOS does not publish CoreMIDI endpoints for it on the
test machine even though the interface is nominally class compliant. OEM parity
therefore requires a persistent CoreMIDI bridge exposing the Eleven Rack Rig and
External ports; this is an implementation item rather than being delegated to
Apple's USB-MIDI class driver.

The standalone C programs in [`Tools/`](Tools/) are protocol diagnostics.
`Tools/erengine.c` contains the verified legacy IOUSBLib isochronous packet
handling that was used to validate the DriverKit transport.

## Current status

The project contains a native macOS installer app, a physical-device-matched
AudioDriverKit extension, correct Core Audio topology, USB identity matching,
and the required entitlement declarations. Its USBDriverKit transport now:

- opens capture interface 4 and playback interface 3;
- selects streaming alternate setting 1 in both directions;
- schedules eight asynchronous 16-microframe isochronous requests per direction;
- decodes eight signed 32-bit input lanes into Core Audio float buffers;
- encodes six Core Audio output lanes as signed 32-bit device words;
- uses fractional packet sizing at 44.1 and 88.2 kHz and integral sizing at
  48 and 96 kHz;
- sends the Eleven Rack clock-source and sample-rate `SET_CUR` requests;
- publishes OEM-style names for every Core Audio input and output element;
- advertises 44.1, 48, 88.2, and 96 kHz as runtime-switchable formats; and
- anchors Core Audio timestamps to USB controller frame time rather than a
  synthetic software timer;
- publishes the isochronous scheduling latency and safety offsets to Core Audio;
- follows a supported externally clocked rate through a Core Audio device
  configuration change; and
- aborts, closes, and reopens streams across stop/start and rate changes, with a
  watchdog retry after a stalled transfer or recovered external clock.

The extension and host app compile for arm64 and x86_64. Live hardware probing
confirmed playback endpoint `0x03`, capture endpoint `0x83`, and a 416-byte
maximum packet in both directions. The device accepted and read back 44.1, 48,
88.2, and 96 kHz, and completed short full-duplex streams at every rate. Observed
capture averages were 176.43, 192.02, 352.79, and 384.00 bytes per microframe,
respectively. Signed-driver validation, DriverKit frame-list offsets, prolonged
stability, and physical routing of every input/output channel remain to be tested.

The macOS host app provides explicit controls for all four sample rates and for
the three observed hardware clock selections: Internal, AES/EBU, and S/PDIF.
Sample-rate changes use the standard Core Audio nominal-rate property; clock
selection and hardware readback use the DriverKit user client. The app disables
its rate picker for external clocking, and the driver rejects clock changes
while isochronous streaming is active. Hardware status reads USB clock entities
`0x80` and `0x81` rather than merely echoing the last requested values. The app
shows external-clock lock, hardware rate, streaming state, Core Audio buffer
size, input/output latency and safety offsets, and an estimated (not physically
measured) round-trip value. It polls for unplug/replug and wake recovery and can
export a privacy-safe JSON diagnostic containing configuration only—never the
unit serial number, rig data, MIDI messages, or audio content. Sample-driver
sine-tone, artificial input-volume, and data-source controls are not published.

The CoreMIDI registry was tested with the device connected and currently reports
zero physical devices, sources, or destinations. Descriptor probing confirmed
that MIDI support must use interface 2, endpoints `0x02`/`0x82`, and USB-MIDI 1.0
four-byte event packets.

`ermidi_bridge.c` implements that missing transport as two persistent CoreMIDI
source/destination pairs named **Eleven Rack Rig** and **Eleven Rack External**.
It routes USB-MIDI cable numbers 0 and 1, translates channel/system messages and
SysEx in both directions, and includes an end-to-end `--self-test`. The test sends
a universal identity request through the CoreMIDI destination and validates the
Digidesign hardware reply after it returns through the CoreMIDI source.

Editor interoperability has also been verified. `errig_read.c` sends the
recovered read-only edit-buffer request, receives a variable-length rig dump,
decodes the editor's 7-bit payload packing, and reports the complete structural
rig snapshot: globals, signal-chain order, effect names and IDs, every raw
effect parameter, the selected amp/cab/microphone, and known human-readable
settings. The captured test payload decoded to 1,104 bytes and its reported name
matched the connected hardware; the personal rig name and capture are excluded
from this repository. See [Docs/RigDataModel.md](Docs/RigDataModel.md) for the
decoded model and remaining display-curve work, and
[Docs/EditorProtocolNotes.md](Docs/EditorProtocolNotes.md) for the message format,
recovered read objects, payload transform, and the clean-room boundary for using
the installed editor as a reference.

`er_inventory.c` performs a resumable Factory/User hardware inventory with a
hard 15-second minimum between program changes. It verifies each selected
address, atomically saves all 208 edit buffers, and restores the original
program. `er_inventory_export.c` produces the exhaustive long-form setting CSV,
and `er_inventory_analyze.rb` generates component/model tables, parameter ranges,
Factory/User comparisons, manifests, checksums, and a Markdown report. The raw
factory/user captures are intentionally not committed to this repository.

The compact editor control path is also confirmed independently of bulk rig
writes. Green JRC enable was changed and read back using the 14-byte Set message
`F0 13 0B 0F 00 11 04 01 40 00 00 00 10 F7`; individual editor changes do not
require retransmitting a complete edit buffer.

The latest complete build and connected-hardware test matrix is recorded in
[Docs/Validation-2026-08-11.md](Docs/Validation-2026-08-11.md).

## Building

Open `ElevenRackDriver.xcodeproj`, select the **ElevenRack (macOS)** scheme, and
choose **My Mac**. Set a unique bundle identifier and your development team for
the app and driver targets.

For a signing-independent compilation check:

```sh
xcodebuild -project ElevenRackDriver.xcodeproj \
  -scheme 'ElevenRack (macOS)' \
  -configuration Debug \
  -derivedDataPath .DerivedData/Unsigned \
  CODE_SIGNING_ALLOWED=NO build
```

Build the standalone diagnostic tools separately with:

```sh
make -C Tools
```

Run a paced, resumable inventory and export its results with:

```sh
./Tools/build/er_inventory /path/to/inventory
./Tools/build/er_inventory_export /path/to/inventory
ruby ./Tools/er_inventory_analyze.rb /path/to/inventory
```

Distribution requires Apple approval for these DriverKit entitlements:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.family.audio`
- `com.apple.developer.driverkit.transport.usb` for VID `0x0DBA` / PID `0xB011`
- `com.apple.developer.system-extension.install` on the host app

For local development, follow Apple's “Debugging and testing system extensions”
instructions. Do not disable SIP on a daily-use machine merely to run an unsigned
driver.

## Activation

Build and place the app in `/Applications`, connect the Eleven Rack, launch the
app, and click **Activate Driver**. Approve the extension in System Settings if
macOS requests it. A correctly signed and running build appears as **Eleven Rack**
in Audio MIDI Setup.

## Hardware validation

1. Connect Eleven Rack directly to the Mac, avoiding a hub for initial testing.
2. Activate the signed driver and confirm `systemextensionsctl list` shows it.
3. Open Audio MIDI Setup and select each supported sample rate.
4. Record all eight inputs while applying signal to each physical source.
5. Send distinct low-level test tones to all six outputs and verify the Main,
   Re-Amp, and Digital pairs independently.
6. Run at least ten minutes per rate while watching the unified log for messages
   from `ElevenRackDevice`, missed USB frames, underruns, or aborted transfers.
7. Repeat through sleep/wake, unplug/replug, and an aggregate-device clock change.

## Attribution

The primary work in this repository is Copyright © 2026 Paul Bender and is
licensed under the MIT license in [`LICENSE.txt`](LICENSE.txt). Paul developed
the DriverKit transport, switchable-rate implementation, CoreMIDI bridge,
hardware rig-name and Rig Vol reading, protocol decoding, and validation work.

Matt Housley authored and contributed the earlier Eleven Rack application and
user-space driver code used in part as a beginning base. His exact MIT license
is retained as a secondary upstream notice in
[`LICENSES/Matt-Housley-MIT.txt`](LICENSES/Matt-Housley-MIT.txt), and he is
credited in [`CONTRIBUTORS.md`](CONTRIBUTORS.md).

The DriverKit implementation added to this fork uses portions of Apple's
publicly distributed “Creating an audio device driver” (SimpleAudio) sample for
its Xcode structure, AudioDriverKit object lifecycle, user-client communication,
and system-extension activation UI. This sample code did not originate in Matt
Housley's repository and was not taken from an Eleven Rack driver, editor, or
installer. The Copyright © 2024 Avid MIT notice included with Apple's sample is
preserved in
[`LICENSES/Avid-Sample-Code-MIT.txt`](LICENSES/Avid-Sample-Code-MIT.txt). See
[`NOTICE.md`](NOTICE.md) for the complete attribution and trademark disclaimer.

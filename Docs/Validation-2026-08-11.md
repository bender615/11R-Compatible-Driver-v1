# Validation report — 2026-08-11

This report closes the Apple Silicon Eleven Rack development session with a
reproducible snapshot of what was tested. No presets, rig parameters, system
extensions, drivers, or software packages were installed or removed during this
validation pass. The hardware was left on its internal clock at 48 kHz.

## Build and static checks

| Test | Result |
| --- | --- |
| Unsigned arm64 macOS host-app build | Pass |
| Embedded DriverKit extension build | Pass |
| DriverKit binary architectures | Pass: arm64 and x86_64 |
| Xcode static analysis | Pass: no ownership or overwritten-result findings |
| Standalone C diagnostics | Pass: 13 executables with `-Wall -Wextra -Werror` |
| Arm64 protocol-probe assembly | Pass |
| Driver/app plist and entitlement parsing | Pass |
| Rig decoder with AddressSanitizer and UBSan | Pass |

The macOS scheme built successfully with Xcode 17 using the DriverKit 25.2 and
macOS 26.2 SDKs, with code signing disabled for compilation validation.

The strict C build found one unused variable in `ringstat.c`; it was removed and
the complete diagnostic build then passed.

The four initial Xcode analyzer findings (repeated for both architectures) were
resolved and a fresh analysis completed with `ANALYZE SUCCEEDED`:

- `NewUserClient_Impl` initializes its output, explicitly balances the service
  reference returned by `Create`, and transfers one typed reference to the
  caller;
- both ring-buffer descriptors and both stream memory mappings now explicitly
  balance the raw `Create`/`CreateMapping` references before their
  `OSSharedPtr`s leave scope;
- both sample-rate paths preserve and return an input-stream notification error
  instead of overwriting or ignoring it; the output notification is attempted
  only after input succeeds.

No `OSSharedPtr::attach` ownership handoffs remain in these paths. The unsigned
host/extension build also completed successfully after the changes.

## Connected-device USB checks

The connected device enumerated as Digidesign VID `0x0DBA`, PID `0xB011`, at
USB high speed (480 Mbit/s), configuration 1.

Observed interfaces and endpoints:

| Function | Interface | Endpoint | Type | Maximum packet |
| --- | ---: | ---: | --- | ---: |
| Rig/external MIDI output | 2 | `0x02` | bulk OUT | 512 bytes |
| Rig/external MIDI input | 2 | `0x82` | bulk IN | 512 bytes |
| Audio playback | 3, alternate 1 | `0x03` | isochronous OUT | 416 bytes |
| Audio capture | 4, alternate 1 | `0x83` | isochronous IN | 416 bytes |

The device, configuration, alternate settings, and all four endpoints were
opened successfully from user space.

## MIDI checks

The raw USB-MIDI identity test passed. The decoded response was:

```text
F0 7E 0F 06 02 13 0B 00 01 00 30 31 35 37 F7
```

The CoreMIDI bridge created both **Eleven Rack Rig** and **Eleven Rack External**
source/destination pairs. Its end-to-end test sent the identity request through
the CoreMIDI destination, transported it over USB cable 0, returned the hardware
reply through the CoreMIDI source, and passed.

## Sample-rate and audio transport checks

The engine selected the internal hardware clock, wrote each required rate, read
the same rate back from the Eleven Rack, and sustained short full-duplex streams.
The final order ended at 48 kHz.

| Requested rate | Hardware readback | USB capture packets | Average payload/microframe |
| ---: | ---: | ---: | ---: |
| 44,100 Hz | 44,100 Hz | 9,808 | 176.43 bytes |
| 88,200 Hz | 88,200 Hz | 9,824 | 352.79 bytes |
| 96,000 Hz | 96,000 Hz | 9,824 | 384.01 bytes |
| 48,000 Hz | 48,000 Hz | 9,808 | 192.03 bytes |

After the ownership fixes, a second connected-device pass again selected and
read back every rate, ending at 48 kHz. The shorter 0.75-second captures produced
5,808 packets at 44.1 kHz (176.44 bytes average), 5,824 at 88.2 kHz (352.79),
5,808 at 96 kHz (384.01), and 5,808 at 48 kHz (192.03). This preserves the
expected exact payload scaling across the four clocks.

An independent 1.5-second 48 kHz full-duplex test completed 1,488 capture and
1,488 playback request groups with zero USB errors and zero empty transfers.
Capture contained live, non-zero signed 32-bit words. Its average capture payload
was 191.6 bytes per microframe; startup/teardown overhead produced an implied
47,525 Hz over the short measurement window.

The repeat 1.5-second full-duplex run completed 1,488 capture and 1,488 playback
request groups with zero errors and zero empty input transfers. Its average
capture payload was 192.1 bytes per microframe and the data was non-zero.

## Rig-data checks

The previously captured edit-buffer file was revalidated offline and under
AddressSanitizer/UBSan:

- SysEx length: 1,269 bytes;
- framing: `F0 13 0B 0F 12 01 ... F7`;
- decoded settings payload: 1,104 bytes;
- decoded current rig name matched the connected hardware display.

The personal rig name and raw SysEx capture are not included in this public
repository.

## Current boundary

The physical protocol and user-space transports are proven, and the unsigned
DriverKit project compiles. The new DriverKit extension itself is not installed
or active. `systemextensionsctl list` showed no Eleven Rack extension, which is
expected before Apple grants the DriverKit entitlements and a signed build is
installed.

Before calling this an OEM-equivalent production driver, the project still needs:

1. Apple DriverKit entitlement approval, signing, installation, and activation;
2. live validation specifically through the dext rather than the legacy
   user-space IOUSBLib engine;
3. physical routing checks for every one of the eight inputs and six outputs;
4. prolonged runs at every rate plus unplug/replug, sleep/wake, and stress tests;
5. integration of the proven CoreMIDI bridge and rig reader into the host app;
6. clean-room mapping of the remaining rig settings record and parameter values.

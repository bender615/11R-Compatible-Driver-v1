# Eleven Rack editor interoperability notes

These notes record facts independently observed from a connected Eleven Rack
and the locally installed, licensed Avid software. They are intended to support
a clean-room compatibility implementation. Do not redistribute Avid binaries,
artwork, or XML resources with this project.

## Installed editor

The standalone Eleven Rack Editor at `/Applications/Eleven Rack Editor.app` is
version 1.0.0.99 and contains Intel-only executables. Its bundled PACE Eden
runtime is from 2013 and the editor does not complete startup on macOS 15.7.7.
The embedded `License Support.pkg` should not be installed on a current system.

The application is still useful as an interoperability reference:

- `Gtr.framework` exports named device, bridge, message, and parser operations.
- 75 XML view definitions enumerate the amp, cab, distortion, dynamics, EQ,
  modulation, delay, reverb, wah, volume, tuner, and FX-loop user interfaces.
- The XML exposes stable four-character parameter identifiers, control types,
  ranges, and enumerated display values.
- Localized strings and plugin resources provide a feature/name inventory.

The installed Pro Tools application contains a current universal `Gtr.framework`.
Loading that already-licensed framework allowed the same exported routines to be
observed as native arm64 code without modifying or redistributing it.

## MIDI transport

Rig-control traffic uses USB-MIDI cable 0 on interface 2, bulk OUT endpoint
`0x02`, and bulk IN endpoint `0x82`. The endpoints have 512-byte maximum packets.
USB-MIDI 1.0 four-byte event packets carry the SysEx stream.

The hardware answered the universal identity request
`F0 7E 7F 06 01 F7` with device ID `0F`, manufacturer `13 0B`, family `00 01`,
and version bytes `30 31 35 37`.

Eleven Rack requests have this form:

```text
F0 13 0B <device-id> <function> <object> [arguments...] F7
```

The observed read function is `01`. Read objects recovered from the editor are:

| Object | Operation | Additional request data |
| ---: | --- | --- |
| `00` | Stored patch data | one patch index byte |
| `01` | Edit-buffer patch data | none |
| `04` | Stored patch name | bank and patch index bytes |
| `05` | Edit-buffer patch name | none |
| `07` | Edit-buffer rig volume | none |
| `3D` | Rig input | none |
| `50` | Tempo | none |

The read-only edit-buffer rig-volume request was verified against connected
hardware:

```text
request:  F0 13 0B 0F 01 07 F7
response: F0 13 0B 0F 12 07 3F 7F 7F 7F 0F F7
```

The five payload bytes split a 32-bit value into `7/7/7/7/4` bits from most to
least significant. This scalar packing differs from the bulk edit-buffer
byte-stream packing. Treating the resulting word as a signed 32-bit integer,
the full signed range maps linearly across the Rig Vol range:

```text
position = int32(raw) - INT32_MIN
dB = -24.0 + 24.0 * position / UINT32_MAX
```

Three hardware settings establish the mapping: the **-24.0 dB** minimum returned
`0x80000000` (`INT32_MIN`), **0.0 dB** returned `0x7FFFFFFF` (`INT32_MAX`), and
**-10.1 dB** returned `0x14000000`, which calculates to -10.125 dB before the
hardware's one-decimal display rounding. The corresponding values also appear
in full edit-buffer snapshots after the `RVol` key (stored there in
little-endian quadlet order).

For example, the exact edit-buffer request for the observed device is:

```text
F0 13 0B 0F 01 01 F7
```

The connected hardware returned a complete 1,269-byte response beginning:

```text
F0 13 0B 0F 12 01 ... F7
```

Here `12` is the update function and `01` identifies edit-buffer patch data.

## Patch-data packing

The bytes after the six-byte response header and before `F7` are 7-bit safe.
Each group of eight packed bytes produces seven raw bytes. For packed bytes
`p[0]` through `p[7]`, raw byte `r[n]` is:

```text
r[0] = (p[0] << 1) | (p[1] >> 6)
r[1] = (p[1] << 2) | (p[2] >> 5)
...
r[6] = (p[6] << 7) |  p[7]
```

The test response decoded to 1,104 bytes of settings data. Its edit-buffer name
starts at raw offset 8 as NUL-padded, four-byte big-endian text words. The test
rig name decoded correctly and matched the hardware display. The personal rig
name and its raw SysEx capture are deliberately excluded from the repository.

The decoded data contains four-character identifiers such as `RVol`, `Mono`,
`Temp`, `PIGI`, `Sync`, `FXc1` through `FXc4`, `Sust`, `Tone`, `Levl`, `Rate`,
`Dpth`, `Mix `, and `DlyP`. Their presence is confirmed, but their surrounding
record layout and value encoding still need to be mapped before editing is safe.

## Diagnostic utility

`Tools/errig_read.c` performs only an identity inquiry and edit-buffer get. It saves
the complete SysEx response and prints the decoded rig name. It also supports
offline decoding:

```sh
clang -Wall -Wextra -Werror -framework CoreFoundation -framework IOKit \
  -pthread -Wno-deprecated-declarations \
  -o Tools/build/errig_read Tools/errig_read.c
./Tools/build/errig_read eleven_rack_edit_buffer.syx
./Tools/build/errig_read --decode eleven_rack_edit_buffer.syx
./Tools/build/errig_read --rig-volume eleven_rack_rig_volume.syx
./Tools/build/errig_read --decode-rig-volume eleven_rack_rig_volume.syx
```

The deprecated IOUSBLib timeout is unreliable on current macOS. The diagnostic
runs blocking reads on a worker thread and aborts the pipe after a bounded wait,
then joins the worker before closing the interface.

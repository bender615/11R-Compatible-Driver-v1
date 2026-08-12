# Eleven Rack rig data model

`Tools/errig_read` now decodes the edit buffer into a complete structural rig
snapshot. It reports the rig name, rig-global fields, all ten signal-chain
sections (`C` through `L`), the category and effect identifier in every section,
and every four-character parameter/value pair. Known identifiers are rendered
with human-readable names while the original tag, signed value, and hexadecimal
word remain visible for validation.

This is a read-only compatibility implementation. Raw values are always kept in
the output because selector encodings and physical-unit display curves are not
all fully verified yet.

## Running the decoder

```sh
make -C Tools build/errig_read
./Tools/build/errig_read --decode eleven_rack_edit_buffer.syx
```

With the hardware connected, omitting `--decode` requests a fresh edit-buffer
snapshot, prints it, and saves the SysEx response:

```sh
./Tools/build/errig_read eleven_rack_edit_buffer.syx
```

## Record structure

- Bytes 0-7: settings-data header.
- Bytes 8-35: seven reversed four-byte words containing the 28-byte rig name.
- Section `A`: rig globals and the `WorC`/`WstC` through `WorL`/`WstL`
  effect/category table.
- Sections `C` through `L`: one record per signal-chain module. Each record is a
  sequence of a reversed four-character key and a signed 32-bit little-endian
  value.

The decoded rig globals currently include Rig Volume, Mono/Stereo, Tempo, Rig
Input, True-Z, expression-pedal target, Rig Output Level, four Multi-FX control
values, MIDI sync, signal-flow state, and volume-pedal endpoint fields. Unknown
or constant fields are retained rather than discarded.

## Effect and parameter inventory

The wire effect ID may have multiple sibling values for the same visible model;
the decoder deliberately collapses those sibling IDs to one display name.

| Family | Visible model | Known parameters |
| --- | --- | --- |
| Volume | Volume Pedal | enabled, position, minimum volume, linear/log taper |
| Wah | Shine Wah; Black Wah | enabled, position, Vox/Cry character |
| Distortion | Tri-Knob Fuzz | enabled, volume, sustain, tone |
|  | Black Op Distortion | enabled, distortion, cut, volume |
|  | Green JRC Overdrive | enabled, overdrive, tone, level |
|  | White Boost | enabled, gain, treble, bass, volume |
|  | DC Distortion | enabled, distortion, treble, bass, volume |
| Modulation | Chorus/Vibrato | enabled, chorus, rate, sync, depth, chorus/vibrato mode |
|  | Orange Phaser | enabled, rate, sync |
|  | Vibe Phaser | enabled, volume, depth, rate, sync, chorus/vibrato mode |
|  | Flanger | enabled, pre-delay, depth, rate, sync, feedback |
|  | Multi Chorus | enabled, rate, sync, depth, pre-delay, mix, waveform, voices, width, low cut |
|  | Roto Speaker | enabled, speed, balance, speaker type |
| Reverb | Blackpanel Spring Reverb | enabled, mix, decay, tone |
|  | Eleven SR | enabled, mix, decay, tone, pre-delay, reverb type |
| Delay | BBD Delay | enabled, delay, sync, feedback, mix, input, chorus/vibrato modulation, depth, expanded delay, noise |
|  | Tape Echo | enabled, delay, sync, feedback, mix, record level, head, wow, expanded delay, hiss |
|  | Dyn Delay | enabled, delay, sync, feedback, mix, mode, ratio, high cut, low cut, width, envelope rate, envelope feedback, envelope mix |
| Dynamics | Gray Compressor | enabled, sustain, level |
|  | Dyn3 Compressor | enabled, threshold, attack, release, ratio, knee, gain |
| EQ | Graphic EQ | enabled, 100 Hz, 370 Hz, 800 Hz, 2 kHz, 3.25 kHz, output |
|  | Parametric EQ | enabled; low, low-mid, high-mid, and high gain/frequency/Q; low/high filter type; output |
| Utility | FX Loop | enabled, send, return, mix |

## Amplifier, cabinet, and microphone data

Amp and cabinet data occupy the same wire section. The selected amplifier is
`sld6`, cabinet is `sldK`, microphone is `sldL`, microphone axis is `sldM`,
speaker breakup is `sldN`, amp bypass is `sld5`, and cabinet bypass is `sldJ`.
The gate threshold/release and output controls are `sld3`, `sld4`, and `sld2`.

The installed editor resources enumerate these amplifier models:

- 59 Tweed Lux; 59 Tweed Bass
- 64 Black Panel Lux Vib; 64 Black Panel Lux Norm; 64 Black Vib
- 65 Black SR; 65 Black Mini; 65 J45
- 66 AC Hi Boost; 67 Black Duo; 67 Plexiglas Vari; 68 Plexiglas 50W
- 69 Plexiglas 100W; 69 Blue Line Bass
- 82 Lead 800 100W; 85 M-2 Lead
- 89 SL-100 Drive; 89 SL-100 Crunch; 89 SL-100 Clean
- 92 Treadplate Modern; 92 Treadplate Vintage; 93 MS 30
- 97 RB-01b Red; 97 RB-01b Blue; 97 RB-01b Green
- DC Modern Overdrive; DC Modern SOD; DC Modern 800; DC Modern Clean
- DC Vintage Crunch; DC Vintage OD; DC Vintage Clean; DC Bass

The legacy cabinets use small selector indices. Expansion cabinets use printable
four-character wire codes stored as 32-bit words. A complete 208-rig hardware
inventory observed the following codes:

| Wire code | Cabinet | Wire code | Cabinet |
| ---: | --- | ---: | --- |
| `0` | 1x12 Blackpanel Lux | `0x4A617A7A` (`Jazz`) | 1x15 Open Back |
| `1` | 1x12 Tweed Lux | `0x32783330` (`2x30`) | 2x12 B30 |
| `2` | 2x12 AC Blue | `0x4A53584D` (`JSXM`) | 2x12 Silver Cone |
| `3` | 2x12 Blackpanel Duo | `0x34435453` (`4CTS`) | 4x10 Black SR |
| `4` | 4x10 Tweed Bass | `0x34783635` (`4x65`) | 4x12 65W |
| `5` | 4x12 Classic 30 | `0x34783230` (`4x20`) | 4x12 Green 20W |
| `6` | 4x12 Green 25W | `0x38535654` (`8SVT`) | 8x10 Blue Line |

The installed editor also contains 1x8 Custom, but none of the 208 captured
Factory/User rigs selected it, so its hardware wire code remains unobserved.

Microphone codes are: `0` Dyn 7, `1` Dyn 57, `2` Dyn 409, `3` Dyn
421, `4` Cond 67, `5` Cond 87, `6` Cond 414, and `7` Ribbon 121.

The decoder assigns canonical names to the common Amp/Cab controls and exact
per-model controls to the 97 RB-01b Red/Blue/Green models:
Presence, Volume, Treble, Middle, Bass, Gain, Boost, and Bright. Other amp
models retain the canonical `Gain 1`, `Gain 2`, `Master`, `Bass`, `Middle`,
`Treble`, and `Presence` labels. Their model-specific panel wording still needs
paired editor/hardware validation; all original keys and raw words are retained.

## Complete hardware inventory

`Tools/er_inventory` safely walks Factory bank 1 and User bank 0, programs
`01A` through `26D`. It enforces a monotonic 15-second switch interval, verifies
the selected address before every read, saves each response atomically, supports
resume, and restores the original program. `Tools/er_inventory_export` writes
one CSV row for every global and slot setting. `Tools/er_inventory_analyze.rb`
then produces unique model/component counts, enabled/bypassed comparisons,
parameter ranges, Factory/User equality results, and SHA-256 evidence.

The 2026-08-11 connected-device run captured all 208 rigs and 24,100 setting
observations: 32 amp models, 14 cabinet models, all 8 microphone models, and 63
slot/category-specific effect IDs. It found 174 unique complete payloads; 34 of
104 same-address Factory/User pairs were identical and 70 differed.

## Physical value representation

Continuous values occupy the full signed 32-bit word. The normalized position
is `(raw - INT32_MIN) / UINT32_MAX`; zero is therefore the midpoint, not the
minimum. The decoder now applies a parameter-specific display schema and always
retains the original signed/hex word.

| Control family | Display conversion |
| --- | --- |
| Amp panel and Speaker Breakup | linear 0.0-10.0 |
| Rig Volume | linear -24.0 to 0.0 dB |
| Tempo | `raw / 10000` BPM |
| Graphic EQ shelves | linear -12.0 to +12.0 dB |
| Graphic EQ mid bands | linear -18.0 to +18.0 dB |
| Graphic EQ output | linear -20.0 to +6.0 dB |
| Dyn3 threshold, knee, gain | -60..0, 0..30, and 0..40 dB |
| Dyn3 attack | logarithmic 10 microseconds to 300 ms |
| Dyn3 release | logarithmic 5 ms to 4 seconds |
| Dyn3 ratio | logarithmic 1:1 to 100:1 |
| Dyn Delay time | linear 1-4000 ms |
| Dyn Delay feedback, mix, width | 0-100% |
| Dyn Delay envelope feedback/mix | -100% to +100% |
| Dyn Delay L/R ratio | 50:100 through 100:100 to 100:50 |
| Multi-Chorus rate | logarithmic 0.01-10.0 Hz |
| Multi-Chorus depth and pre-delay | linear 0-24 ms |
| Multi-Chorus low cut | logarithmic 20-1000 Hz |
| Multi-Chorus mix and width | 0-100% |

The 0-10 schema also covers the modeled stompbox faceplate controls whose
hardware UI presents an unqualified knob scale. Wet/dry mix, pedal position,
rotor balance, and the delay feedback/mix controls are rendered as percentages.
Roto Speed is decoded as Slow/Brake/Fast, and Roto Type is decoded as 120, 122,
21H, Foam, Drum, Rover, Memphis, Wolf, or Watery.

Paired connected-hardware minimum/maximum calibrations of the
`MagicShroomDefin` edit buffer changed 78 of 108 stored fields. Fifty-seven
continuous fields reached both signed endpoints exactly: `INT32_MIN`
(`0x80000000`) at minimum and `INT32_MAX` (`0x7FFFFFFF`) at maximum. This
confirms that the full signed word is the normalized control domain. Bypass
uses the inverse discrete encoding (`0` enabled, `1` bypassed). Multi-Chorus
Rate and Dyn Delay Time did not reach their continuous maxima because setting
Sync to its last enumeration selected 16th-triplet timing; their stored values
then followed rig tempo. The private calibration captures and their personal
rig name remain outside Git.

`settings.csv` and `parameter_catalog.csv` include `unit`, `range`, and `curve`
columns. A remaining value with no verified endpoint is deliberately displayed
as `% normalized` and marked `unverified`; this prevents a useful raw value from
being mistaken for a physical percentage. The remaining important endpoint
work is the amp noise gate/output, FX Loop send/return, BBD and Tape delay time,
Eleven SR decay/pre-delay, Para EQ frequency/Q, and Dyn Delay cut filters.

## Sources and clean-room boundary

The section format and initial identifier catalog were adapted from Guillaume
Schmid's Apache-2.0 ElevenHack project. The decoder is a new C implementation
and carries a modification notice. Cabinet, microphone, control-tag, and UI
inventory facts were independently cross-checked against the locally installed,
licensed Eleven Rack Editor and connected-device captures. No Avid executable,
XML, image, preset, or other proprietary asset is distributed by this project.

# Contributing

Thank you for helping keep the Eleven Rack useful on current Macs.

## Before opening a change

1. Search the existing issues and describe the hardware, Mac model, macOS
   version, sample rate, and test path involved.
2. Build the diagnostic tools with `make -C Tools`.
3. Build the macOS app and DriverKit extension with code signing disabled, or
   test a properly entitled and signed build on non-production hardware.
4. Keep protocol changes read-only until their framing and value encoding are
   understood and independently validated.

## Pull requests

- Keep changes focused and explain how they were tested.
- Use `-Wall -Wextra -Werror` for the standalone C tools.
- Never commit credentials, signing certificates, provisioning profiles,
  personal presets, device captures containing personal data, commercial
  installers, or vendor binaries/assets.
- Document newly verified protocol behavior in `Docs/` without copying vendor
  code or proprietary resources.
- Preserve all copyright, license, and attribution notices.

Hardware tests can interrupt audio and MIDI service while a diagnostic owns a
USB interface. Stop audio applications first, keep monitor levels low, and use
a directly connected Eleven Rack for initial validation.

# Motu MicroBook driver for Apple silicon (unofficial)

A DriverKit / AudioDriverKit driver that makes the original **MOTU MicroBook** USB audio interface work on
Apple silicon Macs. MOTU's own driver for it is an Intel-only kernel extension.

**Unofficial.** Not affiliated with, endorsed by, or supported by MOTU. "MOTU" and "MicroBook" are their
trademarks. No MOTU code or binaries are included; the USB protocol was worked out from their driver for
interoperability and then verified against the hardware.

**This is source code, not a download-and-run installer.** To use it you build and sign it yourself, which
needs a paid Apple Developer account (see Requirements).

## Supported hardware

| Device | USB ID | Revision (`bcdDevice`) | Status |
|---|---|---|---|
| MicroBook (first generation) | `07fd:0004` | `0x0032` | works |
| Anything else, incl. MicroBook II / IIc | | | **not supported** (the driver leaves other revisions unconfigured) |

Check yours: `ioreg -p IOUSB -l -w0 | grep -B5 -A20 MicroBook | grep -E "bcdDevice|idProduct|idVendor"`
(`bcdDevice = 50` is `0x0032`).

## Status

> **Note:** the code in this repository has been built but never installed or run. It differs slightly from the build
> that was tested on hardware (configurable signing identity, hardware-revision check).

The driver was tested on **one** Apple silicon MacBook Pro running macOS 26.6, with one MicroBook.

Works:
- Appears as "MOTU MicroBook" in System Settings > Sound
- Stereo playback at 44.1 and 48 kHz, with a (software) volume slider
- Input stream: 4 channels; the rear 3.5 mm stereo line input arrives on channels 3 and 4
- Survives reboot and sleep/wake; keeps working with SIP re-enabled after installation

Not tested: inputs 1 and 2, a full-level line source, long sessions, latency measurements, macOS upgrades,
unplugging during playback, installing an update with SIP enabled.

Not implemented: hardware mixer / CueMix features, other sample rates.

## Warnings

- **Use at your own risk.** During early development this Mac had one kernel panic inside Apple's USB stack
  while the MicroBook was being probed (before this driver existed). The cause was never established. Save
  your work the first time you plug in.
- Turn your speakers or headphones down for the first test. The volume slider starts at -24 dB.
- The driver writes diagnostic lines to the system log while streaming (input peak levels once a second, and a
  few raw input sample bytes at the start of each stream).

## Requirements

- Xcode, and [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- A paid Apple Developer Program membership
- DriverKit capabilities for your App ID: **USB Transport** and **Audio**.
  - For building and running on your own registered Mac, Xcode can use the *development-only* versions of these.
  - Distributing a build to other people needs Apple's approval. Request it at
    <https://developer.apple.com/system-extensions/> (USB vendor ID `2045` decimal). The status is shown in the
    developer portal under Identifiers > your driver's App ID > Capability Requests.
  - A successful development build does **not** mean your request was approved.

## Build

```sh
DEVELOPMENT_TEAM=XXXXXXXXXX BUNDLE_PREFIX=com.yourname ./build.sh
```

`CHECK=1 ./build.sh` only checks that everything compiles, without signing.

If signing fails on the USB entitlement, make `dext/MicroBookDriverAudio.entitlements` match what your
provisioning profile grants (`idVendor` is `*` in a development profile).

## Install

This is the sequence that was actually tested. It temporarily lowers your Mac's security; undo it afterwards.

1. Recovery (hold the power button at startup > Options > Utilities > Terminal): `csrutil disable`, reboot.
2. `systemextensionsctl developer on`
3. With the MicroBook unplugged:
   `build-xcode/Build/Products/Debug/MicroBookInstaller.app/Contents/MacOS/MicroBookInstaller activate`
4. Allow it in System Settings > General > Login Items & Extensions > Driver Extensions.
5. Plug in the MicroBook. The LED blinks for about 7 seconds, goes steady, and the device appears in Sound.
6. Once it works: copy `MicroBookInstaller.app` to `/Applications`, run `systemextensionsctl developer off`,
   then in Recovery `csrutil enable`.

Keep the app and your source folder; removing a driver's app can remove the driver. The signature is a
development one, so it only runs on Macs registered to your team and stops being valid when your certificate
or profile expires.

**Remove:** run the installer with `deactivate`.

**Updating:** increase `CFBundleVersion` in `dext/Info-Audio.plist` before every reinstall (reinstalling the same
version leaves macOS with duplicate copies until a reboot), then unplug and replug the MicroBook.

**Logs:** `/usr/bin/log stream --info --predicate 'eventMessage CONTAINS "MicroBook"'`

## Layout

- `dext/MicroBookDevice.*` – selects the USB configuration
- `dext/MicroBookInterface.*` – command channel, isochronous streaming, and (audio build) the Core Audio device
- `dext/MicroBookAudioDriver.*` – builds the same implementation as an AudioDriverKit driver (`iig` ignores
  `#ifdef`, so it is a second class declaration; keep the two `.iig` files in sync)
- `dext/MicroBookAudioDevice.*` – sample-rate switching
- `app/` – command-line installer
- `docs/PROTOCOL.md` – USB protocol notes
- Target `MicroBookDriver` is a USB-only diagnostic build (no audio entitlement needed)

## License

Copyright (C) 2026 Richard3332

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See [LICENSE](LICENSE).

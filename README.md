# AE-5 Linux + AegAudio

An independent Linux driver package and native control panel for the Creative Sound BlasterX AE-5.

**Experimental release.** The recovered 0.15 driver was reported audible at 384 kHz/S32_LE on one AE-5 system. This repository packages that source and adds a Linux interface. The new package/UI have not been installed or playback-tested on the working system. Reboot reliability and full Windows feature parity are not claimed.

- Exact driver target: **Ubuntu 7.0.0-31-generic, x86-64, codec 11020011 / subsystem 11020051**.
- AegAudio: native Qt interface for playback, acoustic effects, ten-band EQ, recording, mixer, profiles and driver status.
- Each hardware change is explicit. No automatic gain selection, profile application or mute changes on UI startup.
- Driver installation does not activate Direct mode. The recovered integration currently requires PipeWeaver.
- Private machine snapshots, signing keys and Creative Windows binaries/artwork are not included.

[Installation and rollback](docs/INSTALL.md) · [Features and limitations](docs/FEATURES.md) · [Optional routing](docs/ROUTING.md)

## Build

On an isolated Linux machine with the matching headers:

```sh
python3 packaging/build-deb.py
python3 -m pip wheel --no-deps . -w dist
```

The first command produces an unsigned exact-kernel Debian package; the second produces the control-panel wheel. Neither installs or activates audio. See the installation guide for dependencies and Secure Boot signing with your own key.

## Source layout

`driver/` preserves the recovered kernel implementation and upstream copyright notices. `integration/` contains the user-account/card configuration, service lifecycle and PipeWire templates. `aegaudio/` is the independent Linux UI. `packaging/` builds the release without installing it.

Aurora RGB, Scout Mode, Dolby/DTS encoding and Creative account features are not implemented. Available ALSA effects are disabled in the UI during Direct playback because that mode bypasses processing. See the complete feature matrix before installing.

GPL-2.0-or-later except files bearing their own retained upstream license notice. Sound Blaster and Creative are trademarks of their owners; this project is not an official Creative release.

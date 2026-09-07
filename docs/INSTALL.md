FIRST-PARTY DEVELOPMENT CONTEXT

Owner / operator: Kyle Monti — Aegiris Systems
Route: none
Scope: Installation of the independent AE-5 Linux driver and AegAudio panel on a user's own computer.
Activity: ordinary software development — packaging, compilation and configuration.
Terminology: ALSA is the Linux hardware audio interface; PipeWire is the audio server; PipeWeaver is a separate routing application. Direct mode here means this driver's 384 kHz/S32_LE path.
Projects in scope: this repository and an explicitly selected installation target.
Out of scope: automatic changes to an existing working audio system, unrelated cards, private signing keys and Windows binary redistribution.

# Installation

This is an **experimental, exact-kernel release**, not a universal Linux driver. Supported build target: Ubuntu kernel **7.0.0-31-generic**, x86-64. The recovered device has codec vendor `11020011` and subsystem `11020051`. Other AE-series cards and AE-5 Plus subsystem variants are not qualified by this release.

The original 0.15 driver was reported audible at 384 kHz/S32_LE on one system. The redistributed driver integration has not completed fresh-installation/playback qualification. The browser UI has since been deployed with read-only verification; full manual control testing, reboot, suspend and upgrade behavior remain untested. Installation never starts Direct mode automatically.

**For the complete preview2 download and installation sequence, use the [README installation guide](../README.md#installation).** It includes the single-download bundle, panel-only setup, backup commands, firmware, signing, activation and rollback. The source-build details below also apply. Preview2 reuses preview1's exact driver binary and adds AegAudio 0.2.0 with the browser panel.

## 1. Keep a rollback copy

Before installing on another machine, back up its audio configuration and ALSA state. Do not import somebody else's `asound.state`, gain, volume values, account IDs or card numbers. AegAudio profile files intentionally exclude headphone gain and output selection.

Keep the distribution's stock CA0132 module installed. The package uses the `updates/ae5` directory and does not remove the stock module.

## 2. Build the driver package

Use a separate Linux build directory with the matching kernel headers:

```sh
sudo apt install build-essential linux-headers-7.0.0-31-generic python3 kmod
python3 packaging/build-deb.py
```

This compiles a module and builds `dist/ae5-direct-driver_0.16-1_amd64.deb`. It does not install a module, restart a service, play sound or run hardware tests. It uses two compiler jobs.

The source retains the recovered native driver's 0.15 implementation. The distribution package is version 0.16 because its integration and packaging differ. There is no DKMS claim: changing kernels requires a separately reviewed build and compatibility work.

## 3. Install the package and desktop firmware

```sh
sudo apt install ./dist/ae5-direct-driver_0.16-1_amd64.deb
```

Install the ALSA `ctefx-desktop.bin` firmware from your distribution where available. The required file is available in the [official ALSA firmware 1.2.4 release](https://www.alsa-project.org/files/pub/firmware/alsa-firmware-1.2.4.tar.bz2). Its SHA-256 is:

```text
c9ab092e5717080bcd90971d44aa7d8d30778058ea691dda76320cd315dcc18e
```

If your distribution does not supply it, extract that release into a temporary directory, retain its license, verify the hash, and place `ca0132/ctefx-desktop.bin` at `/usr/lib/firmware/ctefx-desktop.bin`. Do not substitute a Windows firmware blob. Regenerate the matching initramfs after firmware and module signing are complete:

```sh
sudo update-initramfs -u -k 7.0.0-31-generic
```

## 4. Secure Boot

The newly compiled module is **unsigned**. This project does not distribute the developer's private signing key or require trusting it. If Secure Boot is enabled, sign the installed module with your own enrolled Machine Owner Key before loading it. Ubuntu's [Secure Boot documentation](https://wiki.ubuntu.com/UEFI/SecureBoot) explains enrollment and signing.

For an existing enrolled key, the signing command has this shape; replace both paths with your own files:

```sh
sudo /usr/src/linux-headers-7.0.0-31-generic/scripts/sign-file sha256 /path/to/your/MOK.priv /path/to/your/MOK.der /lib/modules/7.0.0-31-generic/updates/ae5/snd-hda-codec-ca0132.ko
sudo depmod 7.0.0-31-generic
sudo update-initramfs -u -k 7.0.0-31-generic
```

Sign **before configuration**, because configuration records the final module hash. Newly enrolled keys require the firmware's local enrollment process. This document does not ask you to disable Secure Boot.

## 5. Prepare the recovered routing integration

This release preserves the recovered integration's **PipeWeaver dependency**. Install PipeWeaver using its [upstream instructions](https://github.com/pipeweaver/pipeweaver). Version 0.1.9-1 was present in the recovered system. PipeWeaver itself and its Windows alternatives are not bundled.

Start from ordinary working 96 kHz playback to the AE-5 with your normal headphone gain and comfortable listening volume. Configure PipeWeaver's System source to reach its Headphones target attached to the AE-5. The service currently requires a graph node named `pipeweaver_system` and the local API on port 14565. The prerequisite is documented rather than silently constructing a graph or importing the developer's setup.

The captured external-input routing is documented separately in [ROUTING.md](ROUTING.md). It is optional; Direct playback does not require an X-Fi, Scarlett or second computer.

Preview the generated account/card configuration, using your own desktop login:

```sh
sudo python3 /usr/lib/ae5-direct/configure.py --user YOUR_LOGIN
```

Apply that reviewed configuration:

```sh
sudo python3 /usr/lib/ae5-direct/configure.py --user YOUR_LOGIN --apply
```

The configurator identifies the card by codec identity and PCI address, captures module hashes, creates the missing PipeWeaver user unit if absent, and saves replaced deployment files under `/var/lib/ae5-direct/backups/`. It does not alter mixers or restart audio. It does not overwrite an existing PipeWeaver service.

In your desktop user session, explicitly load the new user unit and enable it:

```sh
systemctl --user daemon-reload
systemctl --user enable --now pipeweaver.service
```

At a time when an audio interruption is acceptable, load the installed driver using your normal maintenance/reboot procedure. The package deliberately does not reload the audio card during installation. After ordinary 96 kHz playback and the routing prerequisites are present, explicitly activate:

```sh
sudo systemctl start ae5-direct.service
```

This step restarts the configured audio services and selects the dedicated headphone output. It preserves the existing gain and volumes and enables Front playback. Review the hardware mute indicators as well as PCM status; running DMA alone cannot establish audible output.

Only after you have accepted behavior locally, opt into boot activation:

```sh
sudo python3 /usr/lib/ae5-direct/configure.py --user YOUR_LOGIN --apply --enable-on-boot
```

Boot order is not validated in this release. The service waits for a logged-in user's running audio services and an ordinary 96 kHz stream. Automatic boot activation may time out without those prerequisites. No unattended reliability claim is made.

## 6. Install the native Linux panel

```sh
python3 -m venv ~/.local/share/aegaudio-venv
~/.local/share/aegaudio-venv/bin/pip install .
~/.local/share/aegaudio-venv/bin/aegaudio
```

Or install the release wheel into that virtual environment. PySide6 is downloaded as a dependency. ALSA access follows your distribution's normal seat/device permissions; do not run the GUI as root. Driver lifecycle actions request system authorization only when clicked.

The panel reads current state on open. Each mixer change requires Apply. Refresh reloads changes made elsewhere. Profile import shows a change list before applying it. No profile, mute, volume, gain or routing change occurs automatically on GUI startup.

## Removal

Explicitly stop Direct mode first, then disable its boot units and remove the package:

```sh
sudo systemctl stop ae5-direct.service
sudo systemctl disable ae5-direct.service ae5-direct-sleep.service
sudo apt remove ae5-direct-driver
sudo depmod 7.0.0-31-generic
sudo update-initramfs -u -k 7.0.0-31-generic
```

Removal refuses while Direct mode is active. Review the generated `/etc/systemd/system/ae5-direct.service` and `/usr/lib/ae5-direct/installation.json` yourself after removal; they are configuration artifacts, not silently erased by the package. Keep the stock module and your configuration backups. Do not hot-unload a module still in use.

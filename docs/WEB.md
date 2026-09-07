FIRST-PARTY DEVELOPMENT CONTEXT

Owner / operator: Kyle Monti — Aegiris Systems
Route: none
Scope: AE-5-only browser interface using the existing Linux audio driver.
Activity: ordinary application development and a user-operated listening trial.
Terminology: ALSA readback confirms a control value; it does not measure audible output. Direct mode is the recovered 384 kHz/S32_LE playback path.
Projects in scope: AegAudio browser frontend and its narrow ALSA API.
Out of scope: changing drivers, restarting audio services, configuring other cards, and automated listening tests.

# AegAudio web interface

The web interface follows the page layout of Sound Blaster Command: device title, left navigation, SBX profile carousel, five rotary acoustic controls, graphic equalizer, playback configuration, recording, mixer, and bottom volume strip. The artwork was generated for this project; no Creative binaries, artwork, or proprietary preset library are included. This is an independent Linux implementation, not Creative's Windows application.

Run from a source checkout, using the desktop user's account:

```sh
python3 -m aegaudio.web --host 127.0.0.1 --port 8770
```

The web app uses Python's standard library plus the existing ALSA backend; it does not import Qt. The wheel also provides `aegaudio-web`. Bind to an explicitly chosen private LAN address to use it from another computer. The current API is for a trusted local network, has no user login or TLS, and must not be exposed publicly. Browser writes require the page's session token and reject cross-origin requests. This is not network authentication.

Only one codec `0x11020011` with subsystem `0x11020051` is accepted. The server resolves the device again before a write, validates the control identity and values, checks for concurrent changes, and reads back the result. The client cannot select an arbitrary audio card. This preview qualifies the recovered AE-5 subsystem only.

## Operation

- Rotary effects apply when the user releases a drag or an arrow key. Switches and dropdowns apply on explicit user interaction. The mixer offers Apply for each control and keeps left/right values separate.
- The equalizer has draggable and keyboard-adjustable points. Labels use the Linux driver's band indices because this source does not advertise center frequencies. Fine numeric values are available in an expandable section.
- Master volume and mute are in the bottom strip. Output and headphone gain changes require confirmation and are unavailable during active Direct playback.
- Direct mode and audio quality are read-only indicators in this web release. It does not invoke the Direct lifecycle helper, change kernel module parameters, restart services, or play a test signal.
- DSP effect edits are unavailable while Direct playback is active. The dials show stored settings, not a claim that effects are being processed in Direct mode.
- Profile exports exclude amplifier gain and output topology. Controls whose current values fail their own advertised range are omitted rather than silently corrected. Import previews exact changes, checks that the preview is still current, and attempts rollback if a write fails. A rollback is not guaranteed to succeed.
- The carousel's Save and Load actions operate on the user's own profiles. The images are illustrations, not working Creative Gaming/Cinema/Concert presets.
- Listening History records control changes and readbacks automatically. The user can separately record what they heard. Data is stored in `~/.local/share/aegaudio-command/history.jsonl`; the UI shows and exports the most recent 100 entries.

## Scope limits

Scout Mode, Aurora lighting, Dolby/DTS encoding, Creative cloud profiles, Windows calibration, and vendor update services are not implemented. Raw IEC958 bytes and PCM channel maps are informational. Installing this interface does not add these capabilities to the driver.

Before replacing an existing web panel, copy its source, launch command, and audio configurations. Replace and restart only the verified web-server process. Do not restart PipeWire, PipeWeaver, WirePlumber, or the driver service to deploy a web interface.

The browser can establish that the page loads and reports the selected AE-5 state. Audible behavior and control changes require the owner's manual test. Source compilation and successful reads are not evidence that every audio feature works.

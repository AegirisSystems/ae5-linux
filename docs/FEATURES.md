FIRST-PARTY DEVELOPMENT CONTEXT

Owner / operator: Kyle Monti — Aegiris Systems
Route: none
Scope: Mapping the inspected Windows Sound Blaster Command 3.4.92 functionality to this independent Linux application.
Activity: ordinary software development — static interoperability inspection and UI implementation.
Terminology: DSP processes audio; ALSA controls are Linux driver controls; S32_LE is a 32-bit PCM container and is not an analog precision measurement.
Projects in scope: the Linux driver source and AegAudio interface.
Out of scope: redistributing Creative binaries, artwork, proprietary presets, or claiming unimplemented features work.

# Feature mapping

The supplied Creative installer was statically extracted; it was not executed. AE-5's product class delegates to the shared AE-series UI. The inspected Windows pages include playback, SBX profiles, equalizer, recording, Scout Mode, encoder, mixer, lighting, account and settings. Its speaker configuration passes through SoundCore COM to Windows driver interfaces. AegAudio implements Linux controls directly, rather than attempting to run a Windows ASIO/COM driver on Linux.

The [official Sound Blaster Command guide](https://download.creative.com/manualdn/Manuals/TSD/14190/qfWrGlGToW/Sound%20Blaster%20Command%20Software%20Guide.pdf) describes the product-dependent pages and explains that Direct mode bypasses processing. Presence of an ALSA control does not prove feature parity, correct analog output, or an active DSP effect while Direct mode is running.

| Windows function | Linux implementation in this source | Limits |
|---|---|---|
| Playback volume and mute | ALSA Master, Front and PCM controls, with per-channel values where supplied | Readback is displayed; no analog measurement |
| Speakers/headphones | Output Select and auto-detect controls | Explicit confirmation; disabled during active Direct PCM |
| Headphone impedance preset | AE-5 Headphone Gain enumeration | Never auto-selected, excluded from profiles; disabled during Direct PCM |
| DAC reconstruction filter | AE-5 Sound Filter enumeration | Uses the driver's own advertised values |
| Direct 384 kHz / 32-bit PCM | Preserved native driver; explicit service start/stop in UI | One exact kernel/subsystem, existing PipeWeaver integration, untested new packaging |
| SBX acoustic effects | Surround, Crystalizer, X-Bass/crossover, Dialog Plus, Smart Volume/settings, OutFX | DSP mode only; no copied vendor preset library |
| Equalizer | Ten ALSA band controls, draggable setting curve, driver preset enumeration, dB readout | Band indices follow Linux; curve is not a measured filter response |
| Input selection and levels | Input Source, capture controls, Mic Boost, What U Hear | Capture device capabilities remain those of the installed driver |
| Voice processing | Noise Reduction, Voice Focus/Wedge Angle, Mic SVM/SVM level, VoiceFX enumeration, InFX | Exposed DSP controls only; Windows mic EQ and echo-cancellation parity not established |
| Mixer/balance | Separate values for left/right controls; other supported mixer elements | PCM channel-map and IEC958 byte-array writes are not exposed |
| Profiles | JSON export, compatibility validation, preview, explicit apply and rollback attempt | Gain/output topology excluded; no automatic startup restore |
| Driver diagnostics | PCM state and actual DAC mute bits | Does not certify sound at the physical jack |
| Aurora RGB | Not implemented | No supported ALSA lighting API found in the captured driver |
| Scout Mode | Not implemented | No corresponding control in the captured ALSA inventory |
| Dolby Digital Live / DTS Connect | Not implemented | IEC958 controls are not licensed real-time encoders |
| Creative account/cloud presets/updater | Not implemented | Independent app with no Creative account integration |
| Windows speaker calibration / headphone profile library | Not implemented | No copied proprietary data or invented driver mapping |

This is a functional control-panel source implementation, **not a full Sound Blaster Command clone** and not a claim of hardware-tested UI parity. The unknowns are visible in the application instead of presented as working buttons.

The X-Fi's capture rate and missing SPDIF capture route are separate driver work. This release does not modify `snd_ctxfi` or advertise those capabilities as unlocked.

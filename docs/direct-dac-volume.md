<!--
FIRST-PARTY DEVELOPMENT CONTEXT
Owner / operator: Kyle Monti, Aegiris Systems
Route: none
Scope: Implementation and use of the AE-5 Direct DAC volume candidate. This is repository documentation, not authorization to change a running system.
Activity: ordinary software development, driver tests and packaging on Aegiris-owned resources.
Terminology: DAC attenuation reduces output level; it is separate from amplifier gain. DMA is the hardware audio transfer. A candidate has not completed physical playback qualification.
Projects in scope: ae5-linux and AegAudio.
Out of scope: automatic installation, restarting working audio, changes to headphone amplifier gain, other cards and private signing-key redistribution.
-->

# Direct DAC volume candidate

Driver `0.17~preview1-1` and AegAudio `0.3.0` add an editable **Direct DAC volume** control. The previous driver applies 46 attenuation steps, or −23 dB, during Direct preparation. Its Master and Front mixer controls do not edit that setting.

This candidate must replace the loaded driver before the control becomes available. Updating the web page alone cannot add it. The published `v0.16.0-preview2` bundle does **not** include this change. The candidate is restricted to the existing qualified AE-5 identity and kernel `7.0.0-31-generic`.

## Controls

In AegAudio, open **Playback → Direct DAC volume**. Each channel displays and accepts dB values. The range is −127.5 to 0 dB in 0.5 dB steps. Moving toward 0 dB reduces attenuation. It does not select a different headphone gain preset or apply positive digital gain.

The initial driver setting remains −23 dB. Before an explicit selection, the original startup policy also preserves an already quieter DAC baseline. Opening the panel sends no volume write. During Direct playback, an explicit change uses the DAC control without a service restart or a PCM stop. Larger changes advance at most 1 dB per channel per transaction iteration. Physical behavior still requires validation.

The status beside the control means:

- **Queued:** selected for the next Direct preparation; not claimed as applied to an inactive DAC path.
- **Applied:** the driver last verified the applied setting. A read of this status uses that recorded result.
- **Unknown after error:** the hardware state could not be verified. Further level changes are rejected until explicit recovery.

In `alsamixer`, select the AE-5 with F6 and locate `AE-5: Direct DAC Playback Volume`. ALSA's integer scale is 0–255, with `dB = (value − 255) / 2`. Thus 209 means −23 dB, 211 means −22 dB and 255 means 0 dB. Card numbers vary by machine.

Panel edits are saved in `~/.config/aegaudio/direct-dac.json`, matched to the PCI device and codec/subsystem identity. The updated integration restores that preference before opening Direct playback. Invalid or mismatched preferences are ignored. Direct DAC volume is excluded from generic effect profiles. Changes made directly through ALSA survive stream reopen; use the panel if you want the integration's explicit saved preference updated as well.

## Build and validate without touching audio

Run from a disposable source checkout on a Linux build host with matching headers:

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -Idriver tests/test_dac_live.c driver/ae5_dac_volume.c -o /tmp/ae5-dac-test
/tmp/ae5-dac-test
python3 -m unittest discover -s tests -p 'test_*.py'
node tests/web-sync.test.cjs
python3 packaging/build-deb.py --out /tmp/ae5-dac-package
python3 -m pip wheel --no-deps --no-build-isolation . --wheel-dir /tmp/ae5-dac-package
```

These commands build and test software; they do not load the module, operate audio controls or restart services. The Debian package contains the module, matching source, integration code and tests. The UI wheel is a separate artifact. `packaging/build-bundle.py` remains the historical preview2 bundler and intentionally rejects this changed driver rather than combining it with the old binary.

For installation, follow the README's firmware and Secure Boot prerequisites, sign the **new** module before recording its hash, and configure the matching integration. Back up the installed module, installation metadata, service definitions and current audio configuration first. The 0.15 host installation uses `/usr/local/lib/ae5-direct`; the packaged integration uses `/usr/lib/ae5-direct`. Preserve existing host settings during that transition. Do not replace a working configuration with the package defaults merely to add this control.

Installing files does not replace a module already loaded in memory. Activation requires closing the relevant audio handles and loading the new module, which interrupts playback. It is an explicit separate operation. This candidate does not provide a supported hot-patch of the running 0.15 driver.

## Implementation and evidence limits

The ALSA control uses a serialized callback. Live DAC transactions and stock bridge commands share a mutex. The live success path writes only attenuation registers 15 and 16. It does not touch clock, routing, DMA or headphone gain registers. Range validation and the same register-allowance function used by native MMIO are exercised in the offline tests.

The driver reads both channels back, preserves unowned register bits and attempts rollback after a failed write. If rollback cannot be verified, it attempts DAC soft mute, latches the error and does not claim a successful update. This is failure containment, not a guarantee that unresponsive hardware accepted mute.

The tests cover legacy startup/restoration, injected transfer failures, dropped writes, live adjustment, range endpoints, register restrictions, ALSA callback serialization, stale UI writes, device-matched preferences and dB mapping. They cannot establish analog performance, absence of audible transients, reboot/suspend reliability or compatibility with untested cards/kernels. Those claims require physical validation of the candidate.

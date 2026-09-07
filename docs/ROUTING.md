FIRST-PARTY DEVELOPMENT CONTEXT

Owner / operator: Kyle Monti — Aegiris Systems
Route: none
Scope: Portable description of the recovered routing topology, without machine-specific volumes or card numbers.
Activity: ordinary software development — configuration packaging.
Terminology: quantum is the PipeWire processing block size; a source supplies audio and a target receives it.
Projects in scope: optional user-owned audio routing.
Out of scope: automatically importing private mixer state, setting amplifier gain or modifying unrelated sound cards.

# Recovered topology

```text
Workstation analog output → X-Fi line input → PipeWeaver Workstation source ─┐
Local System source ──────────────────────────────────────────────────────┤
Local Browser source ─────────────────────────────────────────────────────┤
                                                                         ↓
                                                   Headphones target → AE-5 Direct sink
Microphone source → Chat Mic target
```

The recovered system used quantum 2048. It also retained VAIO-Main, VAIO-Aux, VAIO-3 and B1-Record virtual buses. Those are optional routing choices, not required driver hardware. The complete original configuration remains in the owner's private snapshot; the public package does not distribute its device identities or volume values.

Use PipeWeaver's UI to attach your own physical input and headphone target. Its `attach-physical-node` command uses a **current numeric node ID**, not a stable device name; resolve that ID from the current device inventory when scripting. Do not reuse another machine's numeric IDs.

Keep user-owned PipeWire configuration in `~/.config/pipewire/pipewire.conf.d/`, not solely in `/run`. The Direct integration copies user configuration into its runtime overlay. The shipped `pipeweaver.service` supplies the unit required by the recovered service but absent from the upstream package used during recovery.

The current service checks System-to-headphone connectivity. Keep the ordinary PipeWeaver System source name expected by the service (`pipeweaver_system`). This coupling is retained and documented; standalone lifecycle support is future work.

For an optional virtual playback bus, a user can create a named PipeWire null sink using the example in `examples/virtual-bus.conf`. Select unique names and route it deliberately. Do not copy the example over existing configuration without reviewing it.

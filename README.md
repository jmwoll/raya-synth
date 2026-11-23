# raya

raya is a modular synth environment implemented in **C99** using **raylib**.

Compiled to WebAssembly, it runs as a web app. At the same time it can run
natively, and the door stays open to turn it into a VST plugin.

## Philosophy

- **Minimalist.** Everything lives in a single file: `raya.c`.
- **Ambient only.** Everything is built to produce beautiful ambient sounds that
  slowly change over time.
- **Performance-centric.** Someone must be able to actually perform a whole song,
  with lots of modulation, so it never gets boring.
- **Every module sounds good alone.** A component produces a nice ambient sound
  on its own, yet can still be connected with other ambient components.
- **Few clicks to something interesting.** Getting to an interesting ambient sound
  takes as few clicks as possible. Cable connections between modules are still
  drawn explicitly, by hand.

## Modules

| Module | What it does |
| --- | --- |
| CLOCK | Tempo in BPM; pulses on 1/16, beat, bar and every 4 bars. |
| SEQUENCER | 16 steps, each with on/off and a value. Outputs GATE and CV. Length, chance (random step skipping) and glide. Runs on its own clock when unpatched. |
| CHORDS | 92 common progressions (e.g. `Fmaj7 Am C G`). A new chord every 1x/2x/4x/8x clock pulses. Outputs four voiced chord notes, a bass note and an ARP note picked by the ARP input. |
| 3X OSC | Three detuned oscillator pairs (sine → triangle → saw), filter, swell envelope, stereo. Pitches from P1–P3, swells on GATE, a drone when GATE is unpatched. |
| LFO | Slow sine, triangle, smooth drift and sample-and-hold for modulation. |
| REVERB | Large, modulated 8-line feedback delay network. |
| OUTPUT | Final destination with soft limiter, scope and meters. |

Pitch cables carry octaves relative to C4; gates and modulation carry 0..1. Several cables into one input are summed.

**Playing:** raya starts with a patch that already sounds: chords from CHORDS into 3X OSC, through REVERB to OUTPUT, with the LFO on the filter and the sequencer morphing the waveform. Click once to unlock audio in the browser. Drag from jack to jack to patch. Right-click a jack to unplug it. Right-click empty space to add a module. Drag empty space to pan. Drag knobs vertically (Shift = fine), use the mouse wheel, double-click to reset.

## Preset rack

Any configuration can be stored as a preset: a single module, a voice, or a whole patch. A preset holds the selected modules, their knob settings, sequencer steps and the cables running between them.

- **Select:** click a module header, Shift-click to add or remove one, Shift-drag on empty space to draw a selection box, Cmd/Ctrl+A to select everything. Dragging a selected header moves the whole group.
- **Save:** press **S** (or the *Save … as preset* button) and give it a name. It is stored in the lowest free numbered slot, e.g. `#11-my-pad-oscillator`.
- **Load:** press **P** (or the *Preset rack* button) and type to search by name, slot number or module. Enter or a click loads it next to the existing modules. Use *Preset rack…* in the right-click menu to load it at that spot instead.
- **Delete:** hover a row and click its **x** twice.

The rack holds up to 2048 presets (`PRESET_MAX`). Each one is a small text file, `0011-my-pad-oscillator.raya`. In the browser they persist in IndexedDB; natively they go in `raya-data/presets/`. A set of factory presets is installed on first start; preset #1 is the startup patch.

## Build

```bash
make deps   # once: fetch Emscripten + raylib into deps/
make serve  # build and serve at http://localhost:8080
```

## Style

The default visual style is described in [STYLE.md](STYLE.md).

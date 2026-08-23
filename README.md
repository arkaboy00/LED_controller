# LED Mask Controller (v2.8)

A multi‑layer LED controller for addressable strips, featuring layer masks, dynamic effects, and preset/ animation storage in LittleFS. Designed for ESP32‑based boards using FastLED.

## Development Note

This project was developed with the assistance of AI‑assisted coding tools (code generation, refactoring, and documentation). The system architecture, design decisions, and debugging were performed manually.  
At the current stage, **not all** AI‑generated code has been fully reviewed, tested, or adapted to the project’s final requirements.

## Features

- **Layer system** – up to 20 layers with effects: `RAINBOW`, `MONO`, `DOT`, `COMET`, `INTERPOLATE`
- **Masks** – apply patterns (`checker`, `even`, `third`, `all`) per layer
- **State management** – shared `state[16]` array for dynamic parameters (position, hue, etc.)
- **Keyframe animation** – supports fixed‑FPS (`anim_fixed`) and variable‑delay (`anim_var`) sequences
- **Preset storage** – binary `.bin` files (version 2) in LittleFS; commands to load, save, list, delete
- **Full‑featured editor** – add/insert/delete/replace frames, change layer count, adjust delays, set phase, and more
- **Serial command interface** – all control and editing via USB serial (115200 baud)

---

## Layer System

Each layer consists of:

- **Effect type** – one of: `RAINBOW`, `MONO`, `DOT`, `COMET`, `INTERPOLATE`
- **Mask** – optional: `checker`, `even`, `third`, `all` (applied per pixel)
- **Bounds** – `start` and `end` positions on the strip (inclusive)
- **Delay** – update interval in ms (per layer)
- **Parameters** – effect‑specific: hue step, speed step, colour, palette, interpolation points, etc.

Layers are rendered sequentially; later layers overwrite earlier ones. Masked pixels are skipped when the mask returns `false`.

---

## State and Animation

A shared `state[16]` array holds dynamic values:
- `state[0]` – position (for DOT, COMET, RAINBOW offset)
- `state[1]` – hue offset (for RAINBOW)
- others can be used by custom effects.

Animations are stored as sequences of frames. Two animation types are supported:

- **`anim_fixed`** – fixed frame rate (FPS)
- **`anim_var`** – each frame has its own delay (in ms)

Frames can be edited, inserted, deleted, and replaced via the built‑in editor.

---

## Preset Storage

All data is stored in LittleFS as binary files (`.bin`). File format version is 2.

Preset types:
- `static` – a single frame (no animation)
- `anim_fixed` – animation with constant FPS
- `anim_var` – animation with per‑frame delays

Commands:
- `load <name>` – load a preset/animation
- `save <name>` – save current layers as a static preset
- `list` – show all `.bin` files
- `deletef <name>` – delete a file
- `info` – show preset details
- `info full` – show complete frame data

---

## Serial Commands

All commands are entered via the Serial Monitor (115200 baud). Type `s` for help at any time.

### Basic control

| Command | Description |
|---------|-------------|
| `l <0-255>` | Set global brightness |
| `+` / `-` | Increase / decrease brightness by 5 |
| `o` | Clear all layers (turn off strip) |
| `freeze` | Pause / resume effect animation (state stops updating) |
| `st` | Show current layers |
| `se` / `effects` | List all effects and their parameters |
| `clear` | Remove all layers |

### Layer management (outside editor)

| Command | Description |
|---------|-------------|
| `add <type> <start> <end> <delay> [params]` | Add a layer with given effect |
| `replace <idx> <type> <start> <end> <delay> [params]` | Replace layer at index |
| `rep` | Alias for `replace` |
| `remove <idx>` / `delete <idx>` | Delete layer |
| `mask <idx> <name>` | Set mask (`checker`, `even`, `third`, `all`) |
| `setpoints <idx> <pos R G B ...>` | Set interpolation points for `INTERPOLATE` layer |
| `set <idx> <param> <value>` | Change layer parameter: `hueoffset`, `speedstep`, `huestep`, `step`, `color R G B`, `delay`, `phase`, `start`, `end` |

### Effects and their creation parameters

- **`RAINBOW`** – `add rainbow start end delay [hueOffset] [hueStep] [speedStep]`
  - `hueOffset` – starting hue (default 0)
  - `hueStep` – hue increment per LED (default 1)
  - `speedStep` – step per update (default 1)

- **`MONO`** – `add mono start end delay [R G B]`
  - colour as three integers (0‑255), default black

- **`DOT`** – `add dot start end delay [startPos]`
  - `startPos` – initial position (default = start)

- **`COMET`** – `add comet start end delay [step] [palette]`
  - `step` – movement step (default 1)
  - `palette` – `fire` or `ice` (default fire)

- **`INTERPOLATE`** – `add interpolate start end delay` (then use `setpoints` to add points)

### Animation playback

| Command | Description |
|---------|-------------|
| `play` | Start animation playback |
| `stop` | Stop playback |
| `pause` | Toggle pause |
| `next` | Jump to next frame |
| `prev` | Jump to previous frame |
| `goto <num>` | Go to frame number |

---

## Editor Mode

Enter editor with `edit`. This allows you to modify frames, layer count, and animation properties.

While in editor, the following commands are available:

### Frame management
- `nf` – add a new frame (copies current layers)
- `if <idx>` – insert a new frame before index
- `df <idx>` – delete frame
- `rf <idx>` – replace frame with current layers
- `sf` – save current layers into the current frame
- `next`, `prev`, `goto <num>` – navigate frames

### Layer management (editor)
- `add`, `replace`, `insert <idx> <type> ...` – same as outside editor, but affects current frame
- `delete <idx>` / `remove <idx>` – delete layer from current frame
- `mask`, `setpoints`, `set` – same as outside, but changes current frame's layer

### Animation properties
- `type <0|1|2>` – change preset type (0=static, 1=fixed, 2=var)
- `framerate <fps>` – set FPS for `fixed` type
- `loop <0|1>` – enable/disable looping
- `delays set <idx> <ms>` – set delay for a frame (var type)
- `delays replace <d1> <d2> ...` – replace all delays at once
- `phase <layerIdx> <value>` – set phase for a layer (`-1` = auto)

### Structural changes
- `change layer count <n>` – change number of layers in all frames
- `increase layer count <n>` – add layers

### Save/exit
- `save` – save current animation to the same file
- `saveas <name>` – save animation under a new name
- `exit` – exit editor (auto‑saves if dirty)

---

## Installation

1. Open the `.ino` file in Arduino IDE or use PlatformIO.
2. Install required libraries:
   - FastLED
   - LittleFS (built‑in for ESP32)
3. Configure pins and LED count at the top of the sketch:
   ```cpp
   #define NUM_LEDS 288
   #define DATA_PIN 16
4. Upload to ESP32


## License
This project is distributed under the `Creative Commons Attribution‑NonCommercial‑ShareAlike 4.0 International License`. See the `LICENSE` file for details.


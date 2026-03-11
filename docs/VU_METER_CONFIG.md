# Digital VU Meter Configuration Guide

The VU meter uses a digital/stepped approach with six discrete brightness
levels and a dead zone for silence.  All values below reflect the current
implementation in `calculate_vu_level()` and `smooth_vu_level()` in
`main/echoes.c`.  Earlier versions of this document described different
thresholds; the figures here are authoritative.

---

## How It Works

### RMS Pipeline

1. Each 512-sample I2S buffer is normalised to −1.0 … +1.0.
2. RMS is computed and scaled by a sensitivity multiplier (`rms * 5.0f`).
3. The scaled level is mapped to one of six discrete output values (see table).
4. `smooth_vu_level()` applies hysteresis to prevent single-frame flicker.

### Stepped Brightness Levels

| Scaled RMS level | LED brightness | Description |
|---|---|---|
| < 0.15 | **OFF (0%)** | Dead zone — silence / very quiet |
| 0.15 – 0.30 | **20%** | Level 1 — Low |
| 0.30 – 0.50 | **40%** | Level 2 — Medium-low |
| 0.50 – 0.70 | **60%** | Level 3 — Medium |
| 0.70 – 0.90 | **80%** | Level 4 — Medium-high |
| ≥ 0.90 | **`vu_max_brightness`** (default 75%) | Level 5 — High/loud |

`vu_max_brightness` is remotely configurable via the dashboard
(`VU_MAX_BRIGHTNESS`, default `0.75`).

### Smoothing Behaviour

`smooth_vu_level()` has no `smooth_factor` parameter — the blend constant
`FAST_SMOOTH = 0.3f` is compiled in.  The two special cases are fast paths:

- **Target is 0 (silence)** → output drops to 0 immediately.  No trailing
  glow in the dead zone.
- **Current is 0, target > 0** → output jumps to target immediately.  The
  first beat is never missed.

For all other transitions the blend is:
```
output = current * 0.3  +  target * 0.7
```
This gives fast-but-not-instantaneous transitions between adjacent stepped
levels, preventing the LED from flickering between two levels on a single
audio frame.

---

## Tuning

### Sensitivity Multiplier

In `calculate_vu_level()`:

```c
float level = rms * 5.0f;  // ← change this
```

| Value | Effect |
|---|---|
| Higher (e.g. `10.0f`) | More sensitive — responds to quieter sounds |
| Lower (e.g. `2.0f`) | Less sensitive — only louder sounds trigger the meter |

### Dead Zone Threshold

```c
if (level < 0.15f) {   // ← adjust this
    return 0.0f;
```

| Value | Effect |
|---|---|
| Higher (e.g. `0.20f`) | Larger dead zone — LED stays off in noisy rooms |
| Lower (e.g. `0.08f`) | Smaller dead zone — responds to whispers |

### Level Step Thresholds and Brightness

```c
if (level < 0.15f) {
    return 0.0f;         // Dead zone
} else if (level < 0.30f) {
    return 0.20f;        // Level 1
} else if (level < 0.50f) {
    return 0.40f;        // Level 2
} else if (level < 0.70f) {
    return 0.60f;        // Level 3
} else if (level < 0.90f) {
    return 0.80f;        // Level 4
} else {
    return remote_config_get()->vu_max_brightness;  // Level 5
}
```

Adjust either the threshold values (step boundaries) or the return values
(brightness at each step) independently.

### Transition Speed

The blend constant in `smooth_vu_level()`:

```c
const float FAST_SMOOTH = 0.3f;
```

| Value | Effect |
|---|---|
| Higher (e.g. `0.7f`) | Slower, heavier smoothing |
| Lower (e.g. `0.1f`) | Faster, more immediate changes |

Setting `FAST_SMOOTH = 0.0f` makes every level change instant (no smoothing).

---

## Behaviour Examples

### Quiet Room
```
Background hiss  →  LED OFF  (below 0.15 dead zone)
Whisper          →  LED OFF  (below 0.15 dead zone)
```

### Normal Speech
```
Conversational voice  →  LED at 40% (Level 2)
Raised voice          →  LED at 60% (Level 3)
Stops speaking        →  LED OFF immediately
```

### Music / Loud Sounds
```
Moderate music  →  LED cycling 60–80% (Levels 3–4)
Loud clap       →  LED at 75% (Level 5 / vu_max_brightness)
Music stops     →  LED OFF immediately
```

---

## Advanced: Custom Level Mappings

### Binary (On/Off only)

```c
if (level < 0.15f) {
    return 0.0f;
} else {
    return remote_config_get()->vu_max_brightness;
}
```

### Three levels

```c
if (level < 0.15f) {
    return 0.0f;
} else if (level < 0.50f) {
    return 0.35f;
} else if (level < 0.80f) {
    return 0.55f;
} else {
    return remote_config_get()->vu_max_brightness;
}
```

### Exponential response

```c
if (level < 0.15f) {
    return 0.0f;
} else {
    float norm = (level - 0.15f) / 0.85f;   // 0.0 → 1.0
    float max  = remote_config_get()->vu_max_brightness;
    return norm * norm * max;                // square for exponential feel
}
```

---

## Troubleshooting

**LED never turns on**
- Increase sensitivity: `float level = rms * 15.0f;`
- Lower dead zone threshold: `if (level < 0.05f)`

**LED always on even when quiet**
- Decrease sensitivity: `float level = rms * 2.0f;`
- Raise dead zone threshold: `if (level < 0.25f)`

**LED flickers between two levels**
- Increase `FAST_SMOOTH` (e.g. `0.5f`) for heavier smoothing.

**Level 5 too bright or too dim**
- Adjust `VU_MAX_BRIGHTNESS` on the dashboard (no reflash needed).

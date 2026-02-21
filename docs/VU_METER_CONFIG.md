# Digital VU Meter Configuration Guide

The VU meter now uses a digital/stepped approach with discrete brightness levels and a dead zone for silence.

## How It Works

### Stepped Brightness Levels

The LED has 6 discrete states:

| Audio Level | LED Brightness | Description |
|-------------|----------------|-------------|
| < 0.05 | **OFF (0%)** | Dead zone - silence/very quiet |
| 0.05 - 0.15 | **15%** | Level 1 - Low volume |
| 0.15 - 0.30 | **30%** | Level 2 - Medium-low |
| 0.30 - 0.50 | **50%** | Level 3 - Medium |
| 0.50 - 0.70 | **70%** | Level 4 - Medium-high |
| > 0.70 | **75%** (max) | Level 5 - High/loud |

### Features

✅ **Dead Zone** - LED stays completely OFF when quiet (no ghosting)
✅ **Discrete Steps** - Clear visual levels (not continuously varying)
✅ **Fast Response** - Immediate turn-on when sound detected
✅ **Instant Off** - LED turns off immediately when sound stops
✅ **Hysteresis** - Smooth transitions between levels (no rapid flickering)

## Tuning the VU Meter

### Adjust Sensitivity

Edit `calculate_vu_level()` in `echoes.c`:

```c
// Scale to reasonable range
float level = rms * 5.0f;  // ← Change this multiplier
```

**Higher value** (e.g., 10.0f): More sensitive, responds to quieter sounds
**Lower value** (e.g., 2.0f): Less sensitive, only responds to louder sounds

### Adjust Dead Zone Threshold

```c
if (level < 0.05f) {  // ← Adjust this threshold
    // Dead zone - LED off
    return 0.0f;
}
```

**Higher threshold** (e.g., 0.10f): LED stays off longer (less sensitive)
**Lower threshold** (e.g., 0.02f): LED turns on with quieter sounds

### Adjust Level Steps

Customize the brightness levels:

```c
} else if (level < 0.15f) {
    return 0.15f;  // ← Adjust brightness for Level 1
} else if (level < 0.30f) {
    return 0.30f;  // ← Adjust brightness for Level 2
}
// ... etc
```

### Adjust Transition Speed

Edit `smooth_vu_level()` in `echoes.c`:

```c
const float FAST_SMOOTH = 0.3f;  // ← Adjust (0.0 - 1.0)
```

**Higher value** (e.g., 0.7f): Slower, smoother transitions between levels
**Lower value** (e.g., 0.1f): Faster, more immediate level changes

Note: the `smooth_factor` parameter in the function signature is currently unused — `FAST_SMOOTH` is the active constant.

## Behavior Examples

### Quiet Room
```
Silence → LED OFF
Whisper → LED OFF (below dead zone)
```

### Normal Speech
```
Speaking → LED at 30% (Level 2)
Louder speech → LED at 50% (Level 3)
Stop speaking → LED OFF (immediate)
```

### Music/Loud Sounds
```
Music playing → LED cycling between 50-75% (Levels 3-5)
Loud clap → LED at 75% (Level 5)
Music stops → LED OFF (immediate)
```

### Visual Response
```
Sound detected: OFF → 15% (instant)
Sound increases: 15% → 30% → 50% (smooth steps)
Sound stops: 50% → OFF (instant)
```

## Default Configuration

The default settings are optimized for:
- Clear visual feedback without being distracting
- Responsive to normal conversation volume
- Dead zone eliminates ghosting from background noise
- 5 discrete levels provide good resolution

## Advanced: Custom Level Mapping

You can create any level pattern you want:

### Example 1: Binary (On/Off Only)
```c
if (level < 0.05f) {
    return 0.0f;  // Off
} else {
    return 0.5f;  // On at 50%
}
```

### Example 2: Three Levels
```c
if (level < 0.05f) {
    return 0.0f;   // Off
} else if (level < 0.30f) {
    return 0.25f;  // Low
} else if (level < 0.60f) {
    return 0.50f;  // Medium
} else {
    return 0.75f;  // High
}
```

### Example 3: Exponential Response
```c
if (level < 0.05f) {
    return 0.0f;
} else {
    // Exponential mapping
    float normalized = (level - 0.05f) / 0.95f;  // 0.0 to 1.0
    return normalized * normalized * 0.75f;  // Square for exponential
}
```

## Troubleshooting

### LED never turns on
- Increase sensitivity: `float level = rms * 30.0f;`
- Lower dead zone: `if (level < 0.02f)`

### LED always on (even when quiet)
- Decrease sensitivity: `float level = rms * 10.0f;`
- Raise dead zone: `if (level < 0.10f)`

### LED flickers rapidly
- Already optimized with hysteresis, but you can slow transitions:
  `const float FAST_SMOOTH = 0.5f;`

### Not enough levels visible
- Add more steps in `calculate_vu_level()`
- Make brightness differences larger

### Too sensitive to background noise
- Increase dead zone threshold
- Decrease overall sensitivity multiplier

## Summary

The digital VU meter provides:
- ✅ Clear visual feedback with discrete levels
- ✅ No ghosting or flickering
- ✅ Fast response to sound
- ✅ Immediate turn-off when quiet
- ✅ Easy to customize for your preferences

Perfect for minimal hardware systems without audio output!

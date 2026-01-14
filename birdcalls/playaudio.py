# birdcall_playback_8kHz_fade.py
# 8 kHz mono 8-bit unsigned raw PCM → PWM on GPIO25 with fade-in/out
# MicroPython v1.27 on ESP32 - January 2026

import machine
import time
import gc

print("SCRIPT STARTED - line 1 after imports")
print("Imports successful")

# Force early visibility
time.sleep_ms(300)  # give REPL time to flush output
print("After sleep - still alive")

# ── Configuration ────────────────────────────────────────────────────────
if 'FILENAME' not in globals():
    FILENAME     = 'XC1053509-cgpt.raw'
SAMPLE_RATE      = 8000
SAMPLE_PERIOD_US = 125                   # 1_000_000 // 8000 = exactly 125 µs
#CHUNK_SIZE       = 1024                  # Bytes per read (~128 ms buffer)
CHUNK_SIZE       = 4096                  # Bytes per read (~128 ms buffer)

#FADE_SAMPLES     = 32                    # ~4 ms fade @8kHz - smooth & quick
FADE_SAMPLES     = 64                    # ~4 ms fade @8kHz - smooth & quick
QUIESCENT        = 32768                 # Mid-point = "silent" for 16-bit duty

# ── PWM setup ────────────────────────────────────────────────────────
pwm = machine.PWM(machine.Pin(32))
pwm.freq(200000)          # High carrier freq (160–250 kHz recommended) → requires RC low-pass filter!
pwm.duty_u16(QUIESCENT)   # Start silent (mid-point for unsigned 8-bit audio)

# ── Main playback ────────────────────────────────────────────────────────
print("Free memory before start:", gc.mem_free())

# ── Play with accurate tick-based timing ─────────────────────────────
try:
    with open(FILENAME, 'rb') as f:
        print("Starting playback (ticks-based timing)...")
        start = time.ticks_us()
        total_samples = 0

        while True:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break

            # Prepare next sample deadline (first sample immediate)
            next_sample_time = time.ticks_us()

            for byte in chunk:
                # Convert 8-bit unsigned → 16-bit PWM duty
                duty = byte << 8              # fast equivalent of byte * 257
                pwm.duty_u16(duty)

                # Wait until the next sample time is reached
                while time.ticks_diff(time.ticks_us(), next_sample_time) < 0:
                    pass   # spin-wait (tight loop, low overhead)

                # Schedule the NEXT sample (add period each time)
                next_sample_time = time.ticks_add(next_sample_time, SAMPLE_PERIOD_US)

                total_samples += 1

        duration_s = time.ticks_diff(time.ticks_us(), start) / 1_000_000
        print(f"Finished! Played {total_samples} samples in {duration_s:.2f} seconds")
        print(f"Effective sample rate: {total_samples / duration_s:.0f} Hz")

except OSError as e:
    print("File error:", e)
except Exception as e:
    print("Playback error:", e)

# ── Cleanup ──────────────────────────────────────────────────────────────
print("Cleaning up...")
for _ in range(10):                     # Gentle settle to mid-point
    pwm.duty_u16(QUIESCENT)
    time.sleep_ms(5)

#pwm.deinit()
time.sleep_ms(100)
gc.collect()

print("Done. Final free memory:", gc.mem_free())

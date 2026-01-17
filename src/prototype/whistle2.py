from machine import I2S, Pin
import time
from math import cos, pi

# =====================
# Configuration
# =====================
SAMPLE_RATE = 16000
BITS = 16
CHANNELS = 1
BUFFER_SIZE = 1024

GAIN = 16.0

LED = Pin(13, Pin.OUT)

WHISTLE_FREQ = 2000  # Hz, typical whistle frequency
VOICE_FREQ = 200     # Hz, typical voice fundamental frequency

THRESH_WHISTLE = 4000.0    # Catches your 5k–25k peaks, requires only mild sustain
THRESH_VOICE   = 4500.0    # Catches almost all your speech bursts (many >>20k–100k)

BLINK_INTERVAL = 0.5  # seconds

WHISTLE_CONFIRM = 1
VOICE_CONFIRM   = 1
whistle_count = 0
voice_count = 0

# Precompute coefficients
coeff_whistle = 2 * cos(2 * pi * WHISTLE_FREQ / SAMPLE_RATE)
coeff_voice = 2 * cos(2 * pi * VOICE_FREQ / SAMPLE_RATE)

# =====================
# In-place Gain
# =====================
def apply_gain_inplace(buf, nbytes, gain):
    mv = memoryview(buf)
    for i in range(0, nbytes, 2):
        v = mv[i] | (mv[i + 1] << 8)
        if v & 0x8000:
            v -= 0x10000

        v = int(v * gain)

        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768

        mv[i] = v & 0xFF
        mv[i + 1] = (v >> 8) & 0xFF

# =====================
# Compute Goertzel
# =====================
def compute_goertzel(buf, n):
    mv = memoryview(buf)
    q1_w = q2_w = 0.0
    q1_v = q2_v = 0.0
    for i in range(0, n, 2):
        v = mv[i] | (mv[i + 1] << 8)
        if v & 0x8000:
            v -= 0x10000
        v = float(v)

        q0_w = coeff_whistle * q1_w - q2_w + v
        q2_w = q1_w
        q1_w = q0_w

        q0_v = coeff_voice * q1_v - q2_v + v
        q2_v = q1_v
        q1_v = q0_v

    mag_sq_w = q1_w**2 + q2_w**2 - coeff_whistle * q1_w * q2_w
    mag_sq_v = q1_v**2 + q2_v**2 - coeff_voice * q1_v * q2_v

    return (mag_sq_w ** 0.5, mag_sq_v ** 0.5)

# =====================
# I2S Microphone
# =====================
mic = I2S(
    0,
    sck=Pin(26),
    ws=Pin(25),
    sd=Pin(35),
    mode=I2S.RX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=BUFFER_SIZE * 4,
)
# Get rid of the transient caused by the mic initialisation
time.sleep(0.20)

# =====================
# Listen Continuously
# =====================
print("Listening...")
buf = bytearray(BUFFER_SIZE)
last_blink = time.time()
blink_state = 0

while True:
    time.sleep_ms(0)
    n = mic.readinto(buf)
    if n:
        apply_gain_inplace(buf, n, GAIN)
        mag_w, mag_v = compute_goertzel(buf, n)

        # Optional: Print magnitudes for tuning thresholds
        if mag_w > 5000 or mag_v > 5000:  # only print louder events
            print(f"Wh mag: {mag_w:6.0f}  Vo mag: {mag_v:6.0f}  cnt_w:{whistle_count} cnt_v:{voice_count}")

        if mag_w > THRESH_WHISTLE:
            whistle_count += 1
            if whistle_count >= WHISTLE_CONFIRM:
                LED.on()
                voice_count = 0  # priority to whistle
            else:
                whistle_count = min(whistle_count, WHISTLE_CONFIRM)  # cap
        elif mag_v > THRESH_VOICE:
            voice_count += 1
            if voice_count >= VOICE_CONFIRM:
                now = time.time()
                if now - last_blink > BLINK_INTERVAL:
                    blink_state = 1 - blink_state
                    LED.value(blink_state)
                    last_blink = now
            else:
                voice_count = min(voice_count, VOICE_CONFIRM)
        else:
            whistle_count = 0
            voice_count = 0
            LED.off()

# =====================
# Cleanup (unreachable, but for completeness)
# =====================
mic.deinit()

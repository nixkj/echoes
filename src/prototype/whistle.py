from machine import I2S, Pin, PWM
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

# LED = Pin(13, Pin.OUT)  # Old digital
LED = PWM(Pin(13))      # PWM for brightness control
LED.freq(1000)          # Set PWM frequency

WHISTLE_FREQ = 2000  # Hz, typical whistle frequency
VOICE_FREQ = 200     # Hz, typical voice fundamental frequency

THRESH_WHISTLE = 4000.0
THRESH_VOICE   = 8000.0

RATIO_THRESH = 5.0   # For discriminating whistle vs clap

BLINK_INTERVAL = 0.5  # seconds (not used now, but kept)

WHISTLE_CONFIRM = 1
VOICE_CONFIRM   = 2
CLAP_CONFIRM    = 1   # Claps are shorter impulses

CLAP_COOLDOWN_BUFFERS = 5   # ~0.25 s at 1024 samples / 16 kHz
clap_cooldown = 0

whistle_count = 0
voice_count = 0
clap_count = 0

current_state = 'idle'

# Brightness levels (0.0 to 1.0)
BRIGHT_OFF = 0.0
BRIGHT_LOW = 0.2    # Voice
BRIGHT_MID = 0.5    # Clap
BRIGHT_FULL = 1.0   # Whistle

# Precompute coefficients
coeff_whistle = 2 * cos(2 * pi * WHISTLE_FREQ / SAMPLE_RATE)
coeff_voice = 2 * cos(2 * pi * VOICE_FREQ / SAMPLE_RATE)

# =====================
# Set LED Brightness
# =====================
def set_led_brightness(level):
    LED.duty_u16(int(65535 * level))

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

set_led_brightness(BRIGHT_OFF)  # Start off

while True:
    time.sleep_ms(0)
    n = mic.readinto(buf)
    if n:
        apply_gain_inplace(buf, n, GAIN)
        mag_w, mag_v = compute_goertzel(buf, n)

        # Optional: Print magnitudes for tuning thresholds
        #if mag_w > 0.9*THRESH_WHISTLE or mag_v > 0.9*THRESH_VOICE:
        #    print(f"Whistle mag: {mag_w:.1f}  Voice mag: {mag_v:.1f}")

        state = 'idle'

        # Needed this because clap would typically trigger whistle after init impulse
        if clap_cooldown > 0:
            clap_cooldown -= 1
            whistle_count = 0
            voice_count = 0
            clap_count = 0
            set_led_brightness(BRIGHT_OFF)  # or keep mid briefly if you prefer
        else:
            # Clap first: both channels excited strongly (broadband)
            if mag_w > 4*THRESH_WHISTLE*4 and mag_v > 1.5*THRESH_VOICE:
                clap_count += 1
                whistle_count = 0
                voice_count = 0
                if clap_count >= CLAP_CONFIRM:
                    state = 'clap'
                    set_led_brightness(BRIGHT_MID)
                    clap_count = CLAP_CONFIRM
                    clap_cooldown = CLAP_COOLDOWN_BUFFERS  # start cooldown

            elif mag_w > THRESH_WHISTLE and (mag_w / (mag_v + 1)) > RATIO_THRESH:
                whistle_count += 1
                clap_count = 0
                voice_count = 0
                if whistle_count >= WHISTLE_CONFIRM:
                    state = 'whistle'
                    set_led_brightness(BRIGHT_FULL)
                    whistle_count = WHISTLE_CONFIRM

            elif mag_v > THRESH_VOICE:
                voice_count += 1
                whistle_count = 0
                clap_count = 0
                if voice_count >= VOICE_CONFIRM:
                    state = 'voice'
                    set_led_brightness(BRIGHT_LOW)
                    voice_count = VOICE_CONFIRM

            else:
                whistle_count = 0
                voice_count = 0
                clap_count = 0
                set_led_brightness(BRIGHT_OFF)

        if state != 'idle':
            print(state)
        
# =====================
# Cleanup (unreachable, but for completeness)
# =====================
mic.deinit()
LED.deinit()

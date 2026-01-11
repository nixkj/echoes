| Device | ESP32 Pin | Component Pin | Notes |
|------|----------|---------------|------|
| BH1750 Light Sensor | 21 | SDA | I2C data line. Labeled as GPIO21 / V_SPI HD / SDA on your board. |
| BH1750 Light Sensor | 22 | SCL | I2C clock line. Labeled as GPIO22 / V_SPI WP / SCL / RTS0. |
| BH1750 Light Sensor | 3V3 | VCC | 3.3V power (use the pin labeled 3.3V). |
| BH1750 Light Sensor | GND | GND | Ground. Connect ADDR to GND if using default address (0x23). |
| ICS-43434 Microphone | 26 | SCK | I2S bit clock (BCK). Labeled as GPIO26. Shared with amp. |
| ICS-43434 Microphone | 25 | WS | I2S word select (LRCLK). Labeled as GPIO25. Shared with amp. |
| ICS-43434 Microphone | 33 | SD | I2S data out (from mic to ESP32). Labeled as GPIO33 (input-only, suitable). |
| ICS-43434 Microphone | GND | SEL | Ground for left channel (mono). Tie to 3.3V for right if preferred. |
| ICS-43434 Microphone | 3V3 | VDD | 3.3V power. |
| ICS-43434 Microphone | GND | GND | Ground. |
| MAX98357A Amplifier | 26 | BCLK | I2S bit clock. Shared with mic. |
| MAX98357A Amplifier | 25 | LRC | I2S word select. Shared with mic. |
| MAX98357A Amplifier | 27 | DIN | I2S data in (from ESP32 to amp). Labeled as GPIO27. |
| MAX98357A Amplifier | 3V3 | VIN | 3.3V power (amp supports up to 5V, but use 3.3V for consistency). |
| MAX98357A Amplifier | GND | GND | Ground. Leave SD (shutdown) floating for always-on, or connect to a spare GPIO (e.g., 4) for mute control if desired. GAIN can be left open for default. |
| LED-05-BWC | 5 | Anode (+) | Via a 220–470Ω resistor to limit current. Labeled as GPIO5 / V_SPI CS0 / SS. |
| LED-05-BWC | GND | Cathode (-) | Ground. |

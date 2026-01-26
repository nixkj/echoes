# Components

## Major

- ESP32 Board: https://www.robotics.org.za/ESP32-DEV
  - Note: blue led connected to GPIO2
- Microphone: https://www.adafruit.com/product/6049
- Amplifier: https://www.adafruit.com/product/3006
- Digital Light Sensor: https://wiki.dfrobot.com/Light_Sensor__SKU_SEN0097_
- Analogue Light Sensor: https://www.adafruit.com/product/2748

## Auxiliary

- Speaker: https://www.robotics.org.za/W14595
- Capacitor: https://www.mantech.co.za/ProductInfo.aspx?Item=82M0489
- Protoboard: https://www.robotics.org.za/MR777
- Bright LED: https://www.robotics.org.za/LED-05-BWC
- Header pins:
  - https://www.robotics.org.za/HFST-05-TH254
  - https://www.robotics.org.za/HFST-06-TH254
  - https://www.robotics.org.za/HFST-08-TH254
  - https://www.robotics.org.za/HF-15P-SP-254
- PSU connection: https://www.mantech.co.za/ProductInfo.aspx?Item=14M2110

## Power

- PSU: https://www.mantech.co.za/ProductInfo.aspx?Item=372M1661
  - Will need at least three
- Cabling: https://www.mantech.co.za/ProductInfo.aspx?Item=352M0339
  - Will need 150 m

## Additional

- Raspberry Pi
- Wi-Fi Access Point

# Resources

## Keywords

- MicroPython v1.27.0 on 2025-12-09; Generic ESP32 module with ESP32
  - Wroom32
- Adafruit MAX98357A I2S 3W Class D Amplifier Breakout
- Adafruit ICS-43434 I2S MEMS Microphone Breakout
- SEN0097 LIGHT SENSOR-BH1750 or ALS-PT19 Analogue Light Sensor

# Pins

| Device | ESP32 Pin | Component Pin | Notes |
|------|----------|---------------|------|
| SEN0097 BH1750 Light Sensor | 21 | SDA | I2C data line. Labeled as GPIO21 / V_SPI HD / SDA on your board. |
| SEN0097 BH1750 Light Sensor | 22 | SCL | I2C clock line. Labeled as GPIO22 / V_SPI WP / SCL / RTS0. |
| SEN0097 BH1750 Light Sensor | 3V3 | VCC | 3.3V power (use the pin labeled 3.3V). |
| SEN0097 BH1750 Light Sensor | GND | GND | Ground. Connect ADDR to GND if using default address (0x23). |
| ICS-43434 Microphone | 32 | BCLK/SCK | I2S bit clock (BCK). |
| ICS-43434 Microphone | 33 | LRCLK/WS | I2S word select (LRCLK). |
| ICS-43434 Microphone | 35 | DOUT/SD | I2S data out (from mic to ESP32). Labeled as GPIO35 (input-only). |
| ICS-43434 Microphone | GND | SEL | Ground for left channel (mono). Tie to 3.3V for right if preferred. |
| ICS-43434 Microphone | 3V3 | VDD | 3.3V power. |
| ICS-43434 Microphone | GND | GND | Ground. |
| MAX98357A Amplifier | 18 | BCLK/SCK | I2S bit clock. |
| MAX98357A Amplifier | 19 | LRCLK/WS | I2S word select. |
| MAX98357A Amplifier | 17 | DIN/SD | I2S data in (from ESP32 to amp). Labeled as GPIO27. |
| MAX98357A Amplifier | 3V3 | VIN | 3.3V power (amp supports up to 5V, but use 3.3V for consistency). |
| MAX98357A Amplifier | GND | GND | Ground. Leave SD (shutdown) floating for always-on, or connect to a spare GPIO (e.g., 4) for mute control if desired. GAIN can be left open for default. |
| LED-05-BWC | 13 | Anode (+) | Via a 220–470Ω resistor to limit current. |
| LED-05-BWC | GND | Cathode (-) | Ground. |
| ALS-PT19 Analogue Light Sensor | GND | - | Ground. |
| ALS-PT19 Analogue Light Sensor | 3V3 | + | 3.3V power. |
| ALS-PT19 Analogue Light Sensor | 34 | OUT | Analogue output. |

# Run Python code

Run Python code from REPL terminal
```python
exec(open('script.py').read())
```

# Birds

- Hadada Ibis (Bostrychia hagedash, also known as Hadeda): Search "Bostrychia hagedash" — Very common, lots of loud "haa-haa-de-dah" flight calls from SA suburbs and wetlands.
- Cape Robin-Chat (Cossypha caffra): Search "Cossypha caffra" — Sweet, melodic songs, many from Cape regions.
- Southern Boubou (Laniarius ferrugineus): Search "Laniarius ferrugineus" — Duets and calls, common in SA bushveld/gardens.
- African Pied Crow (Corvus albus): Search "Corvus albus" — Harsh cawing calls.
- Red-chested Cuckoo (Cuculus solitarius): Search "Cuculus solitarius" — Classic "Piet-my-vrou" call, migratory in SA.
- Cape Glossy Starling (Lamprotornis nitens): Search "Lamprotornis nitens" — Whistling and chattering sounds.
- Spotted Eagle-Owl (Bubo africanus): Search "Bubo africanus" — Hoots, common nocturnal calls.
- Fork-tailed Drongo (Dicrurus adsimilis): Search "Dicrurus adsimilis" — Mimicry and sharp calls.
- Cape Canary (Serinus canicollis): Search "Serinus canicollis" — Sweet twittering songs.
- Red-eyed Dove (Streptopelia semitorquata): Search "Streptopelia semitorquata" - pigeon-like coo that's more repetitive and "declarative".
- Southern Masked Weaver (Ploceus velatus): Search "Ploceus velatus" - 

## Audio samples

- Download bird calls: https://xeno-canto.org
  - Attribution vital, but must also pay attention to the license

## Samples

All Creative Commons Attribution-NonCommercial-ShareAlike 4.0

- Hadada Ibis - https://xeno-canto.org/660390
- Cossypha caffra - https://xeno-canto.org/416973
- Laniarius ferrugineus - https://xeno-canto.org/267222
- Corvus albus - https://xeno-canto.org/292589
- Cuculus solitarius - https://xeno-canto.org/1044888
- * Lamprotornis nitens - https://xeno-canto.org/186140  
- Bubo africanus - https://xeno-canto.org/444328
- * Dicrurus adsimilis - https://xeno-canto.org/715717
- Serinus canicollis - https://xeno-canto.org/428884
- Streptopelia semitorquata - https://xeno-canto.org/1053509
- Ploceus velatus - https://xeno-canto.org/288872



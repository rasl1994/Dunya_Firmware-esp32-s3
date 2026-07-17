# Display Controller — ESP-IDF

Initial safe skeleton for:

- ESP32-S3-WROOM-1-N8R8
- TOP028RGB480V3, 480x480, ST7701S
- future direct startup animation playback
- future LVGL application UI
- future touch and device control

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## First expected log

```text
I (...) board: Board layer initialized in safe mode
I (...) app: ESP-IDF project started
I (...) app: Current state: BOOT
```

## Milestones

1. Verify all PCB GPIO mappings and active levels.
2. Add display power, reset and backlight control.
3. Add ST7701S 3-wire SPI initialization.
4. Add ESP-IDF RGB LCD peripheral and test colors.
5. Add double-buffered startup animation player.
6. Add LVGL and touch input.
7. Add device state machine and safety layer.

The initial project deliberately does not switch on the display or heater.

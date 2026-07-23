# Bench Rev A Parts List

Bench is a Wi-Fi USB-host upload dock for Arduino Uno R3 projects.

## Major Components

| Function | Part | Notes |
|---|---|---|
| Main controller | ESP32-S3-WROOM-1-N8R8 | Wi-Fi + USB host-capable module |
| USB power switch | TPS2553DBV | Current-limited USB-A VBUS switch |
| 3.3V buck regulator | TLV62569DBVR | 5V to 3.3V regulator |
| USB ESD protection | USBLC6-2SC6 | Protects USB-A D+/D− |
| Input TVS diode | TSD05 | Protects 5V input rail |
| USB-C input | USB-C receptacle | Power-only 5V input |
| USB-A output | USB-A receptacle | Target Arduino Uno R3 connection |

## Important Passive Values

| Reference | Value | Purpose |
|---|---:|---|
| R1, R2 | 5.1k | USB-C CC pulldowns |
| R3 | 453k | Buck feedback top resistor |
| R5 | 100k | Buck feedback bottom resistor |
| R8 | 59k | TPS2553 current-limit resistor for ~500mA target |
| R16, R17 | 22R | USB D+/D− series resistors |
| C8 | 120uF | USB-A VBUS bulk capacitance |

## Notes

- USB-C is power-only.
- USB-A acts as the host output port for Arduino Uno R3.
- R_ILIM = 59k for an approximate 500mA USB-A current-limit target.
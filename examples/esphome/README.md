# ESPHome Fujitsu AC

Minimal ESPHome climate component for Fujitsu air conditioners using the UTY-TFSXW1 UART protocol.

## Component layout

The ESPHome component in `components/fujitsu_ac/` wraps the existing Arduino protocol in `src/` directly (no duplicated `.cpp` files). `climate.py` adds `-I<repo>/src` at compile time; `fujitsu_climate.cpp` includes the protocol translation units from there.

## Build and flash

```bash
cd examples/esphome
cp secrets.yaml.example secrets.yaml   # add WiFi credentials
esphome compile fujitsu-ac.yaml
esphome run fujitsu-ac.yaml            # compile + upload
```

## Hardware

- ESP32 connected to the indoor unit UART (9600 baud, inverted TX/RX)
- Default pins: RX GPIO16, TX GPIO17 (adjust for your board)
- Same wiring as the [Arduino Controller example](../../examples/Controller/Controller.ino)

## PoC scope

- Power on/off
- Modes: auto, cool, heat, dry, fan only
- Target temperature (18–30 °C, 0.5 °C steps)
- Fan speed: auto, quiet, low, medium, high
- Current room temperature

Not yet implemented: swing, presets, switches (powerful, economy, etc.).

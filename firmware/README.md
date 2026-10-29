# test firmware

shows dim red, green and blue stripes and checks the four neighbor links

## build

requires Pico SDK 2.3.1 with its TinyUSB submodule, CMake, Ninja and the Arm GNU toolchain. from the repo root:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S firmware -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

power the tile from 12 V. hold BOOT, press and release RESET, then release BOOT. copy `build/rgb_tiles.uf2` to `RPI-RP2`

## check

expect three red columns, two green and three blue. brightness is 8 out of 255

flash the same firmware onto two tiles and connect opposite edges with both boards facing the same way. follow the power instructions in the main README. open USB serial at 115200 baud

`edge RX: N=none E=seen S=none W=none` means the east port recently received the west marker from its neighbor. check the other tile too. markers are sent every 250 ms; a port returns to `none` after one second without a matching byte

north uses GPIO 0/1, east 4/5, south 8/9 and west 12/13 for TX/RX. GPIO16 drives the LEDs; GPIO17 disables the edge transmitters when high

serial PIO adapted from Raspberry Pi examples. license: [pio/LICENSE](pio/LICENSE)

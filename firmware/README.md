# tile firmware

one build for every tile. the board needs external 12 V power even when USB is connected

## build

use Pico SDK 2.3.1, CMake, Ninja and the Arm GNU toolchain. initialise the SDK's TinyUSB submodule, then run these commands from the repository root:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S firmware -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

hold BOOT, press and release RESET, then release BOOT. copy `build/rgb_tiles.uf2` onto the `RPI-RP2` drive. repeat for every tile

## pins

| function | GPIO | peripheral |
| --- | --- | --- |
| north TX / RX | 0 / 1 | UART0 |
| east TX / RX | 4 / 5 | UART1 |
| south TX / RX | 8 / 9 | PIO0 SM0 / SM1 |
| west TX / RX | 12 / 13 | PIO0 SM2 / SM3 |
| LED data | 16 | PIO1 SM0, DMA |
| edge TX disable | 17 | high disables all four outputs |
| USB VBUS detect | 24 | low means VBUS is present |

neighbor links run at 1 Mbaud, 8N1. GPIO17 stays high until the transmitters have been configured. pixels use GRB order at 800 kbit/s, with at least 350 microseconds of low time between frames. image pixels enter in row order and are rearranged for the serpentine LED chain

## finding the layout

each tile identifies itself using the flash's unique ID and advertises its four neighbors. the map only uses links reported by both ends. coordinates follow the edge directions, so every tile must face the same way. loops and holes are allowed. inconsistent coordinates stop frame uploads

a tile with an open USB serial connection takes priority as coordinator. if more than one is connected, the lowest ID wins. without an open USB connection, the lowest ID in the connected layout wins. the coordinator is at tile coordinate 0,0

the map holds up to 128 tiles. north decreases y, east increases x. missing direct links expire after 1.7 seconds; old advertisements expire after 6.5 seconds. allow up to about 10 seconds for discovery after a reboot or a layout change

packets have COBS framing, CRC16 checks and a hop limit. pixel data follows a route to its destination and gets an acknowledgement. display commands are acknowledged by every tile and retried if needed. buffers are bounded; queue and reply failures return an error to the USB host. a coordinator change clears the displayed and staged frames

## USB commands

commands are ASCII lines ending in a newline. the browser controller sends them through USB CDC. 115200 baud is suitable in a serial terminal; USB does not use that as a physical baud rate

| command | reply |
| --- | --- |
| `STATUS` | tile ID, coordinator ID, tile count, conflict and capacity flags, dropped/bad packet counts, last displayed frame |
| `LIST` | one `TILE id x y` line per reachable tile, then `END` |
| `FRAME id x y rgbhex` | `OK FRAME id` after that tile acknowledges the pixels |
| `SHOW id brightness` | `OK SHOW id` after all mapped tiles acknowledge the display command |
| `DEMO id brightness` | display a coordinate-based test pattern |
| `OFF id brightness` | clear the display |

`rgbhex` is exactly 384 hexadecimal characters: 64 RGB pixels in row order, starting at the top left. stage the same frame ID on every tile before sending SHOW. frame IDs are unsigned 32-bit counters and must increase after each display command. brightness is 0 to 255. STATUS reports the last displayed ID so a new host can continue the sequence

the initial display is black. no frame rate is guaranteed for large layouts. USB uploads wait for each tile's reply, and links near the coordinator carry traffic for the tiles beyond them. on a timeout, find the layout again before retrying the image

this firmware has been compiled for RP2040 and checked with host tests. it has not been run on an assembled tile

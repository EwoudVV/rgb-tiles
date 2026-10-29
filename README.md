# RGB Tiles

modular 8 x 8 RGB LED tiles with an RP2040 on each board. connectors on all four edges let the tiles join into different shapes. all tiles face the same way

![front](images/tile-front.png)
![back](images/tile-back.png)

each tile is 99.5 mm square, with 64 WS2812C-V6 LEDs, 2 MB flash, USB-C and a four-layer PCB

open [rgb-tiles.kicad_pro](rgb-tiles.kicad_pro) in KiCad 10. [manufacturing/](manufacturing/) contains the Gerbers, drill files, BOM and purchase list. fabrication: four layers, 1.6 mm FR-4, JLC04161H-7628 stackup, 1 oz outer / 0.5 oz inner copper, lead-free HASL

power comes from a regulated 12 V supply at J7. USB is for data only. allow 1 A per tile and use a separate feed to each tile for full brightness. leave both edge power shunts out between separately fed sections. support the boards with a frame

[test firmware](firmware/README.md) displays dim red, green and blue stripes and reports received neighbor markers over USB serial

[Hack Club Half Life, week 4](https://halflife.hackclub.com/projects/c01og00redvbnb7qoo1i9yo8)

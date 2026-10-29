#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "links.pio.h"
#include "pixels.pio.h"

#define BAUD 115200u
#define LINK_DISABLE 17

/* Edge order: north, east, south, west. Same firmware on every tile. */
static const uint8_t expected[4] = {'S', 'W', 'N', 'E'};
static const uint8_t marker[4] = {'N', 'E', 'S', 'W'};

static void init_links(void) {
    gpio_init(LINK_DISABLE);
    gpio_put(LINK_DISABLE, 1);
    gpio_set_dir(LINK_DISABLE, GPIO_OUT);
    for (unsigned p = 0; p < 2; p++) {
        uart_inst_t *u = p ? uart1 : uart0;
        uart_init(u, BAUD);
        uart_set_format(u, 8, 1, UART_PARITY_NONE);
        uart_set_hw_flow(u, false, false);
        uart_set_fifo_enabled(u, true);
        gpio_set_function(p * 4, GPIO_FUNC_UART);
        gpio_set_function(p * 4 + 1, GPIO_FUNC_UART);
    }
    uint tx = pio_add_program(pio0, &link_tx_program);
    uint rx = pio_add_program(pio0, &link_rx_program);
    for (unsigned p = 2; p < 4; p++) {
        uint pin = p * 4, sm = (p - 2) * 2;
        pio_sm_claim(pio0, sm);
        pio_sm_claim(pio0, sm + 1);
        pio_sm_set_pins_with_mask(pio0, sm, 1u << pin, 1u << pin);
        pio_sm_set_consecutive_pindirs(pio0, sm, pin, 1, true);
        pio_gpio_init(pio0, pin);
        pio_gpio_init(pio0, pin + 1);
        gpio_pull_up(pin + 1);
        pio_sm_config c = link_tx_program_get_default_config(tx);
        sm_config_set_out_pins(&c, pin, 1);
        sm_config_set_sideset_pins(&c, pin);
        sm_config_set_out_shift(&c, true, false, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
        pio_sm_init(pio0, sm, tx, &c);
        pio_sm_set_enabled(pio0, sm, true);
        c = link_rx_program_get_default_config(rx);
        sm_config_set_in_pins(&c, pin + 1);
        sm_config_set_jmp_pin(&c, pin + 1);
        sm_config_set_in_shift(&c, true, false, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
        pio_sm_init(pio0, sm + 1, rx, &c);
        pio_sm_set_enabled(pio0, sm + 1, true);
    }
    gpio_put(LINK_DISABLE, 0);
}

static void show_stripes(void) {
    uint offset = pio_add_program(pio1, &pixels_program);
    pio_sm_claim(pio1, 0);
    pio_gpio_init(pio1, 16);
    pio_sm_set_consecutive_pindirs(pio1, 0, 16, 1, true);
    pio_sm_config c = pixels_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, 16);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 16000000.0f);
    pio_sm_init(pio1, 0, offset, &c);
    pio_sm_set_enabled(pio1, 0, true);
    sleep_us(350);
    for (unsigned i = 0; i < 64; i++) {
        unsigned row = i / 8, col = i % 8;
        unsigned x = row % 2 ? 7 - col : col;
        /* Low-brightness red, green and blue stripes. Wire order is GRB. */
        uint32_t rgb = x < 3 ? 8u << 16 : x < 5 ? 8u << 24 : 8u << 8;
        pio_sm_put_blocking(pio1, 0, rgb);
    }
    sleep_ms(3); /* Drain the last words, then allow the LED reset interval. */
}

int main(void) {
    uint32_t last_seen[4] = {0};
    bool seen[4] = {false};
    uint32_t next_send = 0, next_report = 0;
    init_links();
    stdio_init_all();
    show_stripes();
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        for (unsigned p = 0; p < 4; p++) {
            /* Bound each drain so a noisy input cannot stop the other edges. */
            for (unsigned n = 0; n < 16; n++) {
                int byte;
                if (p < 2) {
                    uart_inst_t *u = p ? uart1 : uart0;
                    if (!uart_is_readable(u)) break;
                    byte = uart_getc(u);
                } else {
                    uint sm = (p - 2) * 2 + 1;
                    if (pio_sm_is_rx_fifo_empty(pio0, sm)) break;
                    byte = (uint8_t)(pio_sm_get(pio0, sm) >> 24);
                }
                if (byte == expected[p]) {
                    seen[p] = true;
                    last_seen[p] = now;
                }
            }
        }
        if ((int32_t)(now - next_send) >= 0) {
            for (unsigned p = 0; p < 4; p++) {
                if (p < 2) uart_putc_raw(p ? uart1 : uart0, marker[p]);
                else if (!pio_sm_is_tx_fifo_full(pio0, (p - 2) * 2))
                    pio_sm_put(pio0, (p - 2) * 2, marker[p]);
            }
            next_send = now + 250;
        }
        if ((int32_t)(now - next_report) >= 0) {
            printf("edge RX:");
            for (unsigned p = 0; p < 4; p++)
                printf(" %c=%s", marker[p], seen[p] && now - last_seen[p] < 1000 ? "seen" : "none");
            printf("\n");
            next_report = now + 1000;
        }
        sleep_ms(1);
    }
}

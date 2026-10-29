#include "io.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "links.pio.h"
#include "pico/stdlib.h"
#include "pixels.pio.h"
#include <string.h>
#define QUEUE_SIZE 4096u
#define QUEUE_MASK (QUEUE_SIZE - 1u)
typedef struct {
  uint8_t bytes[QUEUE_SIZE];
  volatile uint32_t head, tail;
} Ring;
static Ring rx[4], tx[4];
static uint8_t wire_rx[4][WIRE_LIMIT];
static unsigned wire_size[4];
static bool discard[4];
static int led_dma;
static uint32_t led_words[64];
static uint64_t led_until;
uint32_t io_dropped, io_bad_packets;
static void rx_push(unsigned p, uint8_t c) {
  uint32_t h = rx[p].head;
  if (h - rx[p].tail >= QUEUE_SIZE) {
    io_dropped++;
    return;
  }
  rx[p].bytes[h & QUEUE_MASK] = c;
  __dmb();
  rx[p].head = h + 1;
}
static void uart0_rx(void) {
  while (uart_is_readable(uart0))
    rx_push(NORTH, (uint8_t)uart_getc(uart0));
}
static void uart1_rx(void) {
  while (uart_is_readable(uart1))
    rx_push(EAST, (uint8_t)uart_getc(uart1));
}
static void pio_rx(void) {
  for (unsigned p = SOUTH; p <= WEST; p++)
    while (!pio_sm_is_rx_fifo_empty(pio0, (p - 2) * 2 + 1))
      rx_push(p, (uint8_t)(pio_sm_get(pio0, (p - 2) * 2 + 1) >> 24));
}
void io_init(void) {
  gpio_init(LINK_DISABLE_PIN);
  gpio_put(LINK_DISABLE_PIN, 1);
  gpio_set_dir(LINK_DISABLE_PIN, GPIO_OUT);
  gpio_init(VBUS_N_PIN);
  gpio_set_dir(VBUS_N_PIN, GPIO_IN);
  for (unsigned p = 0; p < 2; p++) {
    uart_inst_t *u = p ? uart1 : uart0;
    unsigned pin = p * 4;
    uart_init(u, LINK_BAUD);
    uart_set_format(u, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(u, false, false);
    gpio_set_function(pin, GPIO_FUNC_UART);
    gpio_set_function(pin + 1, GPIO_FUNC_UART);
    irq_set_exclusive_handler(p ? UART1_IRQ : UART0_IRQ,
                              p ? uart1_rx : uart0_rx);
    irq_set_enabled(p ? UART1_IRQ : UART0_IRQ, true);
    uart_set_irq_enables(u, true, false);
  }
  uint tx_offset = pio_add_program(pio0, &link_tx_program),
       rx_offset = pio_add_program(pio0, &link_rx_program);
  for (unsigned p = 2; p < 4; p++) {
    unsigned pin = p * 4, sm = (p - 2) * 2;
    pio_sm_claim(pio0, sm);
    pio_sm_claim(pio0, sm + 1);
    pio_sm_set_pins_with_mask(pio0, sm, 1u << pin, 1u << pin);
    pio_sm_set_consecutive_pindirs(pio0, sm, pin, 1, true);
    pio_gpio_init(pio0, pin);
    pio_gpio_init(pio0, pin + 1);
    gpio_pull_up(pin + 1);
    pio_sm_config c = link_tx_program_get_default_config(tx_offset);
    sm_config_set_out_pins(&c, pin, 1);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * LINK_BAUD));
    pio_sm_init(pio0, sm, tx_offset, &c);
    pio_sm_set_enabled(pio0, sm, true);
    c = link_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&c, pin + 1);
    sm_config_set_jmp_pin(&c, pin + 1);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * LINK_BAUD));
    pio_sm_init(pio0, sm + 1, rx_offset, &c);
    pio_sm_set_enabled(pio0, sm + 1, true);
    pio_set_irq0_source_enabled(
        pio0, (enum pio_interrupt_source)(pis_sm0_rx_fifo_not_empty + sm + 1),
        true);
  }
  irq_set_exclusive_handler(PIO0_IRQ_0, pio_rx);
  irq_set_enabled(PIO0_IRQ_0, true);
  uint offset = pio_add_program(pio1, &pixels_program);
  pio_sm_claim(pio1, 0);
  pio_gpio_init(pio1, LED_PIN);
  pio_sm_set_consecutive_pindirs(pio1, 0, LED_PIN, 1, true);
  pio_sm_config c = pixels_program_get_default_config(offset);
  sm_config_set_sideset_pins(&c, LED_PIN);
  sm_config_set_out_shift(&c, false, true, 24);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 16000000.0f);
  pio_sm_init(pio1, 0, offset, &c);
  pio_sm_set_enabled(pio1, 0, true);
  led_dma = dma_claim_unused_channel(true);
  gpio_put(LINK_DISABLE_PIN, 0);
}
void io_service(void) {
  for (unsigned p = 0; p < 4; p++) {
    while (tx[p].tail != tx[p].head) {
      if (p < 2) {
        uart_inst_t *u = p ? uart1 : uart0;
        if (!uart_is_writable(u))
          break;
        uart_putc_raw(u, tx[p].bytes[tx[p].tail & QUEUE_MASK]);
      } else {
        unsigned sm = (p - 2) * 2;
        if (pio_sm_is_tx_fifo_full(pio0, sm))
          break;
        pio_sm_put(pio0, sm, tx[p].bytes[tx[p].tail & QUEUE_MASK]);
      }
      tx[p].tail++;
    }
  }
}
bool io_send(unsigned p, const Packet *packet) {
  if (p > 3)
    return false;
  uint8_t wire[WIRE_LIMIT];
  size_t n = packet_encode(packet, wire);
  if (!n || QUEUE_SIZE - (tx[p].head - tx[p].tail) < n) {
    io_dropped++;
    return false;
  }
  for (size_t i = 0; i < n; i++)
    tx[p].bytes[(tx[p].head + i) & QUEUE_MASK] = wire[i];
  tx[p].head += (uint32_t)n;
  return true;
}
bool io_receive(unsigned p, Packet *packet) {
  while (rx[p].tail != rx[p].head) {
    uint8_t c = rx[p].bytes[rx[p].tail & QUEUE_MASK];
    __dmb();
    rx[p].tail++;
    if (!c) {
      unsigned n = wire_size[p];
      wire_size[p] = 0;
      if (discard[p]) {
        discard[p] = false;
        continue;
      }
      if (!n)
        continue;
      if (packet_decode(wire_rx[p], n, packet))
        return true;
      io_bad_packets++;
    } else if (!discard[p]) {
      if (wire_size[p] >= WIRE_LIMIT) {
        discard[p] = true;
        wire_size[p] = 0;
        io_bad_packets++;
      } else
        wire_rx[p][wire_size[p]++] = c;
    }
  }
  return false;
}
bool pixels_ready(void) {
  return time_us_64() >= led_until && !dma_channel_is_busy(led_dma);
}
void pixels_show(const uint8_t *rgb, uint8_t brightness) {
  if (!pixels_ready())
    return;
  for (unsigned y = 0; y < 8; y++)
    for (unsigned x = 0; x < 8; x++) {
      unsigned i = (y * 8 + x) * 3;
      uint32_t r = rgb[i] * brightness / 255, g = rgb[i + 1] * brightness / 255,
               b = rgb[i + 2] * brightness / 255;
      led_words[led_index(x, y)] = (g << 24) | (r << 16) | (b << 8);
    }
  dma_channel_config c = dma_channel_get_default_config(led_dma);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, pio_get_dreq(pio1, 0, true));
  dma_channel_configure(led_dma, &c, &pio1->txf[0], led_words, 64, true);
  led_until = time_us_64() + 64 * 30 + 350;
}

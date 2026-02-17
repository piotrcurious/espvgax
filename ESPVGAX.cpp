#include "ESPVGAX.h"

volatile uint32_t ESPVGAX_ALIGN32 ESPVGAX::fbw[ESPVGAX_HEIGHT][ESPVGAX_WWIDTH];
volatile uint8_t ESPVGAX::uart_rx_buf[ESPVGAX_UART_RX_BUF_SIZE];
volatile uint16_t ESPVGAX::uart_rx_head = 0;
volatile uint16_t ESPVGAX::uart_rx_tail = 0;

static volatile uint32_t ESPVGAX_ALIGN32 empty[ESPVGAX_WWIDTH];
static volatile uint32_t *line;
static volatile int fby;
static volatile int vsync;
static volatile int running;

#ifdef ESPVGAX_EXTRA_COLORS
volatile uint8_t props[ESPVGAX_HEIGHT + 45];
#endif

volatile uint8_t *ESPVGAX::fbb=(volatile uint8_t*)&ESPVGAX::fbw[0];

#define UART_FIFO(i)            (0x60000000 + (i)*0x100)
#define UART_INT_ENA(i)         (0x60000000 + (i)*0x100 + 0x0C)
#define UART_STATUS(i)          (0x60000000 + (i)*0x100 + 0x1C)
#define UART_RXFIFO_CNT         0x000000FF
#define UART_RXFIFO_FULL_INT_ENA (BIT(0))
#define UART_RXFIFO_TOUT_INT_ENA (BIT(8))

#include "espvgax_hspi.h"

// wait a fixed numbers of CPU cycles
#define NOP_DELAY(N) asm volatile(".rept " #N "\n\t nop \n\t .endr \n\t":::)

#define US_TO_RTC_TIMER_TICKS(t) \
  ((t) ? \
   (((t) > 0x35A) ? \
    (((t)>>2) * ((APB_CLK_FREQ>>4)/250000) + \
     ((t)&0x3) * ((APB_CLK_FREQ>>4)/1000000)) : \
    (((t) *(APB_CLK_FREQ>>4)) / 1000000)) : \
   0)

static inline uint32_t getTicks() {
  uint32_t ccount;
  asm volatile ("rsr %0, ccount":"=a"(ccount));
  return ccount;
}
#define TICKS (getTicks())

void ICACHE_RAM_ATTR vga_handler() {
  noInterrupts();
#if ESPVGAX_TIMER==0
  // timer0 needs to be scheduled again immediately
  timer0_write(TICKS + 16 * US_TO_RTC_TIMER_TICKS(32));
#endif

  // --- BEGIN NEGATIVE HSYNC ---
  GPOC = 1 << ESPVGAX_HSYNC_PIN;

  // Determine if this is a visible line or blanking line
  bool is_visible = (fby < ESPVGAX_HEIGHT) && running;

#ifdef ESPVGAX_EXTRA_COLORS
  if (fby >= 0 && fby < (ESPVGAX_HEIGHT + 45)) {
    uint8_t pr = props[fby];
    if (pr & ESPVGAX_PROP_COLOR1) GP16O |= 1;
    else GP16O &= ~1;

    if (pr & ESPVGAX_PROP_COLOR2) GPOS = 1 << ESPVGAX_EXTRA_COLOR2_PIN;
    else GPOC = 1 << ESPVGAX_EXTRA_COLOR2_PIN;
  }
#endif

  // Prepare SPI/DMA FIFO during HSYNC low window
  if (is_visible) {
    HSPI_VGA_prepare();
  }

  // HSYNC Pulse Width Delay
#if F_CPU==80000000L
  NOP_DELAY(is_visible ? 50 : 80);
#else
  NOP_DELAY(is_visible ? 200 : 300);
#endif

  // --- END NEGATIVE HSYNC ---
  GPOS = 1 << ESPVGAX_HSYNC_PIN;

  // Pulse VSYNC based on signal state
  ESP8266_REG(vsync) = 1 << ESPVGAX_VSYNC_PIN;

  if (is_visible) {
    // START PIXELDATA transfer for visible lines
    HSPI_VGA_send();

    // Update line pointer for next cycle while current line is being shifted out
    fby++;
    line = (fby < ESPVGAX_HEIGHT) ? ESPVGAX::fbw[fby] : empty;
  } else {
    // --- OPTIMIZATION FOR BLANKING LINES ---
    // Instead of calling send() for 'empty' data and blocking the CPU,
    // we just update state and exit early to let UART interrupts fire.

    fby++;
    // Handle VSYNC state transitions
    switch (fby) {
      case 525:
        fby = 0;
        break;
      case 490:
        vsync = 0x308; // Begin negative VSYNC
        break;
      case 492:
        vsync = 0x304; // End negative VSYNC
        break;
    }

    line = (fby < ESPVGAX_HEIGHT) ? ESPVGAX::fbw[fby] : empty;
  }

  // Poll UART0 FIFO before re-enabling interrupts
  uint32_t uart_status = READ_PERI_REG(UART_STATUS(0));
  uint16_t rx_cnt = uart_status & 0xFF; // UART_RXFIFO_CNT is bits 0-7
  while (rx_cnt--) {
    uint8_t c = READ_PERI_REG(UART_FIFO(0)) & 0xFF;
    uint16_t next_head = (ESPVGAX::uart_rx_head + 1) % ESPVGAX_UART_RX_BUF_SIZE;
    if (next_head != ESPVGAX::uart_rx_tail) {
      ESPVGAX::uart_rx_buf[ESPVGAX::uart_rx_head] = c;
      ESPVGAX::uart_rx_head = next_head;
    }
  }

  interrupts();

  // Keep watchdog alive
  ESP.wdtFeed();
}
void ESPVGAX::begin() {
  pinMode(ESPVGAX_VSYNC_PIN, OUTPUT);
  pinMode(ESPVGAX_HSYNC_PIN, OUTPUT);
  pinMode(ESPVGAX_COLOR_PIN, OUTPUT);
#ifdef ESPVGAX_EXTRA_COLORS
  pinMode(ESPVGAX_EXTRA_COLOR1_PIN, OUTPUT);
  pinMode(ESPVGAX_EXTRA_COLOR2_PIN, OUTPUT);
#endif
  // prepare first line
  fby=0;
  line=fbw[0];
  // begin with positive VSYNC
  vsync=0x304;
  running=1;
  // setup HSPI to output PIXELDATA on D7 PIN 
  HSPI_VGA_init();
  // Disable UART RX interrupts to prevent jitter
  CLEAR_PERI_REG_MASK(UART_INT_ENA(0), UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA);
  // install vga_handler interrupt
  noInterrupts();
#if ESPVGAX_TIMER==0
  timer0_isr_init();
  timer0_attachInterrupt(vga_handler);
  timer0_write(TICKS+16*US_TO_RTC_TIMER_TICKS(32));
#else
  timer1_isr_init();
  timer1_attachInterrupt(vga_handler);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
  timer1_write(US_TO_RTC_TIMER_TICKS(32));
#endif
  interrupts();
}
void ESPVGAX::pause() {
  running=0;
}
void ESPVGAX::resume() {
  running=1;
}
void ESPVGAX::end() {
  // disable installed interrupt
  noInterrupts();
#if ESPVGAX_TIMER==0
  timer0_detachInterrupt();
#else
  timer1_detachInterrupt();
#endif
  interrupts();
}
int ESPVGAX::uart_available() {
  return (ESPVGAX_UART_RX_BUF_SIZE + uart_rx_head - uart_rx_tail) % ESPVGAX_UART_RX_BUF_SIZE;
}
int ESPVGAX::uart_read() {
  if (uart_rx_head == uart_rx_tail) return -1;
  uint8_t c = uart_rx_buf[uart_rx_tail];
  uart_rx_tail = (uart_rx_tail + 1) % ESPVGAX_UART_RX_BUF_SIZE;
  return c;
}
void ICACHE_RAM_ATTR ESPVGAX::delay(uint32_t msec) {
  // predict the CPU ticks to be awaited
  uint32_t us=msec*1000;
  uint32_t start=TICKS;
  uint32_t target=start+16*US_TO_RTC_TIMER_TICKS(us);
  uint32_t prev=start;
  int overflow=0;
  for (;;) {
    uint32_t now=TICKS;
    if (target<start) {
      // overflow will occur
      if (prev>now)
        // overflow is occurred
        overflow=1;
      else if (overflow && now>target)
        // end is reached
        break;
    } else if (now>target) {
      // end is reached
      break;
    }
    prev=now;
  }
}
static uint64_t rand_next=1;

uint32_t ESPVGAX::rand() {
  rand_next = rand_next * 1103515245ULL + 12345;
  return rand_next+((uint32_t)(rand_next / 65536) % 32768);
}
void ESPVGAX::srand(unsigned int seed) {
  rand_next = seed;
}
void ESPVGAX::setLinesProp(int y, int end, uint8_t prop) {
  while (y<end) 
    setLineProp(y++, prop);
}
void ESPVGAX::setLineProp(int y, uint8_t prop) {
#ifdef ESPVGAX_EXTRA_COLORS
  if (y>=ESPVGAX_HEIGHT) 
    return;
  props[y]=prop;
#else
#endif
}
uint8_t ESPVGAX::getLineProp(int y) {
#ifdef ESPVGAX_EXTRA_COLORS
  return props[y];
#else
  return 0;
#endif
}
//include blit methods, implemented via a bunch of macros
#include "espvgax_blit.h"

//include print methods, implemented via a bunch of macros
#include "espvgax_print.h"

//include draw primitives methods
#include "espvgax_draw.h"

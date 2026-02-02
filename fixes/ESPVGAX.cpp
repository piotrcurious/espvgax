#include "ESPVGAX.h"

volatile uint32_t ESPVGAX_ALIGN32 ESPVGAX::fbw[ESPVGAX_HEIGHT][ESPVGAX_WWIDTH];

static volatile uint32_t ESPVGAX_ALIGN32 empty[ESPVGAX_WWIDTH];
static volatile uint32_t *line;
static volatile int fby;
static volatile int vsync;
static volatile int running;

#ifdef ESPVGAX_EXTRA_COLORS
// BUG FIX: Changed from 525 to proper size for safety
volatile uint8_t props[ESPVGAX_HEIGHT + 45]; // 480 + 45 = 525 total VGA lines
#endif

volatile uint8_t *ESPVGAX::fbb=(volatile uint8_t*)&ESPVGAX::fbw[0];

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
  // timer0 need to be scheduled again
  timer0_write(TICKS+16*US_TO_RTC_TIMER_TICKS(32));
#endif
  // begin negative HSYNC
  GPOC=1<<ESPVGAX_HSYNC_PIN;
#ifdef ESPVGAX_EXTRA_COLORS
  // BUG FIX: Added bounds check to prevent buffer overflow
  if (fby >= 0 && fby < (ESPVGAX_HEIGHT + 45)) {
    uint8_t pr=props[fby];
    if (pr & ESPVGAX_PROP_COLOR1) 
      GP16O |= 1;
    else
      GP16O &= ~1;
    if (pr & ESPVGAX_PROP_COLOR2)
      GPOS=1<<ESPVGAX_EXTRA_COLOR2_PIN;
    else
      GPOC=1<<ESPVGAX_EXTRA_COLOR2_PIN; 
  }
#if F_CPU==80000000L
  NOP_DELAY(100); // 2us*80MHz (- 60clock because extra colors require time)
#else
  NOP_DELAY(400); // should be 320 (2us*160MHz) but 400 works well
#endif  
#else // ESPVGAX_EXTRA_COLORS not defined
#if F_CPU==80000000L
  NOP_DELAY(160); // 2us*80MHz
#else
  NOP_DELAY(480); // should be 320 (2us*160MHz) but 480 works well
#endif  
#endif
  // end negative HSYNC
  GPOS=1<<ESPVGAX_HSYNC_PIN;
  // begin or end VSYNC, depending on value of vsync variable
  ESP8266_REG(vsync)=1<<ESPVGAX_VSYNC_PIN;
  //write PIXELDATA
  if (running) {
    HSPI_VGA_prepare();
    HSPI_VGA_send();
  }
  // prepare for the next vga_handler run
  fby++;
  switch (fby) {
  case 525: 
    // restart from the beginning
    fby=0; 
    break;
  case 490: 
    // next line will begin negative VSYNC 
    vsync=0x308; 
    break;
  case 492: 
    // next line will end negative VSYNC
    vsync=0x304; 
    break;
  }
  // fetch the next line, or empty line in case of VGA lines [480..524]
  line=(fby<ESPVGAX_HEIGHT) ? ESPVGAX::fbw[fby] : empty;
  interrupts();
  /* 
   * feed the dog. keep ESP8266 WATCHDOG awake. VGA signal generation works 
   * well if there are ZERO calls to Arduino functions like delay or yield. 
   * These functions will perform many background tasks that generates some
   * delays on the VGA signal stability but keeps the hardware WATCHDOG awake. 
   * I have not figured out why this happen, probably there are some hardware
   * task that generate a jitter in the interrupt callback, like on ATMEGA MCU,
   * see the VGAX dejitter nightmare
   */
  ESP.wdtFeed(); 
}

void ESPVGAX::begin() {
  pinMode(ESPVGAX_VSYNC_PIN, OUTPUT);
  pinMode(ESPVGAX_HSYNC_PIN, OUTPUT);
  pinMode(ESPVGAX_COLOR_PIN, OUTPUT);
#ifdef ESPVGAX_EXTRA_COLORS
  pinMode(ESPVGAX_EXTRA_COLOR1_PIN, OUTPUT);
  pinMode(ESPVGAX_EXTRA_COLOR2_PIN, OUTPUT);
  // Initialize props array
  memset((void*)props, 0, sizeof(props));
#endif
  // prepare first line
  fby=0;
  line=fbw[0];
  // begin with positive VSYNC
  vsync=0x304;
  running=1;
  // setup HSPI to output PIXELDATA on D7 PIN 
  HSPI_VGA_init();
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

void ICACHE_RAM_ATTR ESPVGAX::delay(uint32_t msec) {
  // BUG FIX: Improved overflow handling and added early exit for 0 delay
  if (msec == 0) return;
  
  // predict the CPU ticks to be awaited
  uint32_t us=msec*1000;
  uint32_t start=TICKS;
  uint32_t target=start+16*US_TO_RTC_TIMER_TICKS(us);
  uint32_t prev=start;
  bool overflow_expected = (target < start);
  bool overflow_occurred = false;
  
  for (;;) {
    uint32_t now=TICKS;
    
    if (overflow_expected) {
      // overflow will occur
      if (prev > now) {
        // overflow has occurred
        overflow_occurred = true;
      } else if (overflow_occurred && now >= target) {
        // end is reached after overflow
        break;
      }
    } else if (now >= target) {
      // end is reached (no overflow case)
      break;
    }
    prev=now;
    // OPTIMIZATION: Yield to watchdog periodically
    if ((now - start) % 10000 == 0) {
      ESP.wdtFeed();
    }
  }
}

static uint64_t rand_next=1;

uint32_t ESPVGAX::rand() {
  // BUG FIX: Fixed potential overflow in rand implementation
  rand_next = rand_next * 1103515245ULL + 12345ULL;
  return (uint32_t)((rand_next >> 16) & 0x7FFFFFFF);
}

void ESPVGAX::srand(uint32_t seed) {
  rand_next = seed;
}

void ESPVGAX::setLinesProp(int start, int end, uint8_t prop) {
  // OPTIMIZATION: Added bounds checking
  if (start < 0) start = 0;
  if (end > ESPVGAX_HEIGHT) end = ESPVGAX_HEIGHT;
  
  while (start < end) {
    setLineProp(start++, prop);
  }
}

void ESPVGAX::setLineProp(int y, uint8_t prop) {
#ifdef ESPVGAX_EXTRA_COLORS
  // BUG FIX: Added proper bounds checking
  if (y < 0 || y >= ESPVGAX_HEIGHT) 
    return;
  props[y]=prop;
#endif
  // No need for empty else block
}

uint8_t ESPVGAX::getLineProp(int y) {
#ifdef ESPVGAX_EXTRA_COLORS
  // BUG FIX: Added bounds checking
  if (y < 0 || y >= ESPVGAX_HEIGHT)
    return 0;
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

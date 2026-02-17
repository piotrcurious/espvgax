/**
 * Optimized ESP8266 VGA ISR
 * * Logic:
 * 1. HSYNC/VSYNC pulses are still generated to maintain monitor sync.
 * 2. During visible lines (fby < ESPVGAX_HEIGHT), the CPU is held for HSPI timing.
 * 3. During blanking lines, we re-enable interrupts early and skip HSPI_VGA_send().
 * This allows UART interrupts to process during the ~15ms vertical porch.
 */

void ICACHE_RAM_ATTR vga_handler() {
  // Disable interrupts for critical HSYNC timing start
  noInterrupts();

#if ESPVGAX_TIMER==0
  // Timer0 needs to be scheduled again immediately
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
    
    // End of visible line timing
    interrupts();
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

    // Enable interrupts immediately so UART can use the rest of this scanline's time
    interrupts();
  }

  // Keep watchdog alive
  ESP.wdtFeed();
}


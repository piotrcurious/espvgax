void ICACHE_RAM_ATTR vga_handler() {
  noInterrupts();
#if ESPVGAX_TIMER==0
  // timer0 need to be scheduled again
  timer0_write(TICKS+16*US_TO_RTC_TIMER_TICKS(32));
#endif

  // begin negative HSYNC
  GPOC = 1 << ESPVGAX_HSYNC_PIN;

#ifdef ESPVGAX_EXTRA_COLORS
  // bounds check to prevent buffer overflow
  if (fby >= 0 && fby < (ESPVGAX_HEIGHT + 45)) {
    uint8_t pr = props[fby];
    if (pr & ESPVGAX_PROP_COLOR1)
      GP16O |= 1;
    else
      GP16O &= ~1;
    if (pr & ESPVGAX_PROP_COLOR2)
      GPOS = 1 << ESPVGAX_EXTRA_COLOR2_PIN;
    else
      GPOC = 1 << ESPVGAX_EXTRA_COLOR2_PIN;
  }

  // Prepare SPI/DMA FIFO while HSYNC is low (use HSYNC window for copy)
  if (running) {
    HSPI_VGA_prepare(); // <-- moved into HSYNC low window so copying happens during HSYNC
  }

  // remaining portion of HSYNC pulse (reduced — most work done by prepare)
#if F_CPU==80000000L
  NOP_DELAY(50); // reduced from 100
#else
  NOP_DELAY(200); // reduced from 400
#endif

#else // ESPVGAX_EXTRA_COLORS not defined

  // Prepare SPI/DMA FIFO while HSYNC is low
  if (running) {
    HSPI_VGA_prepare();
  }

#if F_CPU==80000000L
  NOP_DELAY(80);  // reduced from 160
#else
  NOP_DELAY(240); // reduced from 480
#endif

#endif
  // end negative HSYNC
  GPOS = 1 << ESPVGAX_HSYNC_PIN;

  // begin or end VSYNC, depending on value of vsync variable
  ESP8266_REG(vsync) = 1 << ESPVGAX_VSYNC_PIN;

  // start PIXELDATA transfer (DMA/send) right after HSYNC
  if (running) {
    HSPI_VGA_send();
  }

  // prepare for the next vga_handler run
  fby++;
  switch (fby) {
  case 525:
    // restart from the beginning
    fby = 0;
    break;
  case 490:
    // next line will begin negative VSYNC
    vsync = 0x308;
    break;
  case 492:
    // next line will end negative VSYNC
    vsync = 0x304;
    break;
  }

  // fetch the next line, or empty line in case of VGA lines [480..524]
  line = (fby < ESPVGAX_HEIGHT) ? ESPVGAX::fbw[fby] : empty;

  interrupts();

  // keep watchdog alive
  ESP.wdtFeed();
}

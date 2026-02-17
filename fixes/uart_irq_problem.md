You are absolutely right, and I appreciate the pushback. Your math is spot-on: the vga_handler only takes roughly 3 to 5 microseconds to execute, leaving about 26 to 28 microseconds of idle time per 31.7 µs VGA line. The CPU is not starved of clock cycles. The UART definitely has plenty of time to process data.
So why the conflict? I dug into the Xtensa LX106 architecture and the ESP8266 Arduino Core source code (HardwareSerial.cpp and uart.c), and the root cause isn't CPU starvation—it's Interrupt Priority Contention.
The Core Problem: Interrupt Level 1 Collision
The ESP8266 (Xtensa LX106) has a very flat interrupt architecture available to the user. It effectively relies on two levels:
 * Level 3 (NMI): Strictly reserved for the hardware Watchdog and the Wi-Fi MAC.
 * Level 1: Everything else (Timers, GPIO, SPI, I2C, and UART).
Here is the exact mechanism of the crash:
 * Same Priority, No Nesting: Both timer0/timer1 (which drive your vga_handler) and the hardware UART trigger Level 1 interrupts. The Xtensa core does not support nested interrupts at the same priority level.
 * The UART Interruption: When a byte arrives via UART, the hardware fires a Level 1 interrupt. The CPU jumps into the Arduino Core's uart_interrupt_handler to move the byte from the hardware FIFO into the software ring buffer.
 * The VGA Delay (Jitter): If your precise VGA timer expires while the CPU is busy executing the UART ISR, the timer interrupt is blocked. It gets queued and must wait until the UART ISR finishes executing.
 * The Result: The UART ISR only takes a few microseconds, but in the VGA world, a 2-3 microsecond delay means the vga_handler fires late. The HSYNC pulse is delayed by dozens of clock cycles, shifting the horizontal line on the monitor.  This continuous, random microsecond latency creates massive screen tearing or a total loss of sync.
The conflict isn't the UART missing data; it's the UART's interrupt randomly stealing precise start times from the VGA timer.
How to Arbitrate This at the Hardware Level
To fix this, we have to stop the UART from asynchronously interrupting the CPU, and instead handle the UART data synchronously when we know it's safe.
Here are two ways to hack the ESP8266 hardware registers to resolve this, bypassing the default Arduino HardwareSerial behavior:
1. Hardware FIFO Polling during VBLANK (Best for ≤ 57600 baud)
Instead of letting the Arduino Core attach an interrupt to the UART, we disable the UART RX interrupt entirely. The ESP8266 has a dedicated 128-byte hardware FIFO for UART0. Even with CPU interrupts disabled, the silicon will continue to collect incoming bytes automatically.
 * Disable the UART RX interrupt in setup() using direct register manipulation: CLEAR_PERI_REG_MASK(UART_INT_ENA(0), UART_RXFIFO_FULL_INT_ENA);.
 * Let the silicon buffer the incoming data while the screen is drawing (lines 0 to 480).
 * During the VBLANK period (when fby > 480), manually read from the UART_FIFO(0) register into your own buffer.
 * The Math: At 57,600 baud, 128 bytes takes about 22.2ms to arrive. The active display frame takes ~15.2ms. This guarantees the hardware FIFO will never overflow before you reach the VBLANK window to empty it.
2. Synchronous Line-by-Line Polling (Best for 115200 baud)
If you need high-speed UART, the 128-byte hardware FIFO will overflow during the 15.2ms active frame if you only check it during VBLANK. Instead, we poll the hardware FIFO at the very end of the vga_handler itself.
 * You still disable the Arduino Core UART interrupt.
 * Right before interrupts(); is called at the end of vga_handler, check the UART_STATUS register to see if the hardware FIFO has bytes.
 * If bytes are present, pull them out of UART_FIFO(0) and stuff them into a volatile circular buffer.
 * Because you are doing this synchronously inside the VGA timer, the UART is processed during that 26 µs idle window, and it can never preempt the exact start time of the next HSYNC pulse.
Would you like me to write the specific register-level code to detach the Arduino UART interrupts and implement the synchronous line-by-line FIFO polling inside your vga_handler?

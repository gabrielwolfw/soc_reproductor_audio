#include "sys/alt_stdio.h"
#include "alt_types.h"
#include "sys/alt_irq.h"


#define LEDS_BASE    0x4000
#define BUTTON_BASE  0x4010
#define TIMER_BASE   0x4020

unsigned int elapsed_ms;

void timer_ir_handler (void * context);

int main()
{
	volatile unsigned int * leds_ptr = (unsigned int *) LEDS_BASE;
	volatile unsigned int * button_ptr = (unsigned int *) BUTTON_BASE;
	volatile unsigned int * timer_status_ptr = (unsigned int *) TIMER_BASE; //offset 0
	volatile unsigned int * timer_ctr_ptr = timer_status_ptr + 1; //offset 1
	volatile unsigned int * timer_snapl_ptr = timer_status_ptr + 4; //offset 4

	alt_putstr("Hello from Nios II!\n");
	if (*timer_status_ptr != 0) {
		alt_printf("ERROR: status is not 0 -> %x\n", *timer_status_ptr);
		return 0;
	}
	alt_ic_isr_register(0x0,0x2, timer_ir_handler, 0x0, 0x0);

	alt_putstr("Turning on the timer\n");
	*timer_ctr_ptr = 0x7;
	while (*timer_status_ptr != 0x2);
	alt_putstr("Timer is running\n");
	elapsed_ms = 0;
	/* Event loop never exits. */
	  /* Event loop never exits. */
	  while (1) {
			  *leds_ptr = elapsed_ms;
			  if ((*leds_ptr) >= 1<< 10)
				return 0;
		  }
	  return 0;
}

	void timer_ir_handler (void * context) {
		  //Clear the interrupt so that we can keep counting
		  volatile int* timer_status_ptr = (int *) TIMER_BASE;
		  *timer_status_ptr = 0x0;
		  alt_printf("interrupt handler called.  clear status is %x\n", *timer_status_ptr);
		  elapsed_ms += 1;
	}

#include <stddef.h>
#include <stdint.h>

static void outb(uint16_t port, uint8_t value)
{
	asm("outb %0,%1"
		: /* empty */
		: "a"(value), "Nd"(port)
		: "memory");
}

void
	__attribute__((noreturn))
	__attribute__((section(".start")))
	_start(void)
{
	const char *p = "Hello, world!\n";
	outb(0xE9, p[0]);
	outb(0xE9, p[1]);
	outb(0xE9, p[2]);
	outb(0xE9, p[3]);
	outb(0xE9, p[4]);
	outb(0xE9, p[5]);
	outb(0xE9, p[6]);
	outb(0xE9, p[7]);
	outb(0xE9, p[8]);
	outb(0xE9, p[9]);
	outb(0xE9, p[10]);
	outb(0xE9, p[11]);
	outb(0xE9, p[12]);
	outb(0xE9, p[13]);
	outb(0xE9, p[14]);

	*(long *)0x400 = 42;

	for (;;)
		asm("hlt"
			: /* empty */
			: "a"(42)
			: "memory");
}

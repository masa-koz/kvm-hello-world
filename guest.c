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
#if 0
	const char *p;
	for (p = "Hello, world!\n"; *p; ++p)
		outb(0xE9, *p);
#else
	outb(0xE9, 'H');
	outb(0xE9, 'e');
	outb(0xE9, 'l');
	outb(0xE9, 'l');
	outb(0xE9, 'o');
	outb(0xE9, ',');
	outb(0xE9, ' ');
	outb(0xE9, 'W');
	outb(0xE9, 'o');
	outb(0xE9, 'r');
	outb(0xE9, 'l');
	outb(0xE9, 'd');
	outb(0xE9, '!');
	outb(0xE9, '\n');
#endif
	*(long *)0x400 = 42;

	for (;;)
		asm("hlt"
			: /* empty */
			: "a"(42)
			: "memory");
}

#include <stddef.h>
#include <stdint.h>

static void outb(uint16_t port, uint8_t value)
{
	asm("outb %0,%1"
		: /* empty */
		: "a"(value), "Nd"(port)
		: "memory");
}

inline unsigned char
inb (unsigned short port)
{
  unsigned char _v;

  __asm__ __volatile__ ("inb %w1,%0":"=a" (_v):"Nd" (port));
  return _v;
}

void
	__attribute__((noreturn))
	__attribute__((section(".start")))
	_start(void)
{
	long memval;
#if 1
	const char *p;
	int i = 0;

	inb(0xE9);
	for (p = "Hello, world!\n"; *p && i < 14; ++p)
	{
		outb(0xE9, *p);
		i++;
	}
#else
	const char *p = "Hello, world!\n";

	inb(0xE9);
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
#endif
	*(long *)0x400 = 42;
	memval = *(long *)0x400;
	outb(0xE9, *(uint8_t *)&memval);
	outb(0xE9, *(((uint8_t *)&memval) + 1));
	outb(0xE9, *(((uint8_t *)&memval) + 2));
	outb(0xE9, *(((uint8_t *)&memval) + 3));

	for (;;)
		asm("hlt"
			: /* empty */
			: "a"(42)
			: "memory");
}

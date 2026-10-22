// What the compiler may call for copies and clears of local arrays.
#include <stddef.h>

void* memcpy(void* dest, const void* src, size_t n) {
	unsigned char* d = dest;
	const unsigned char* s = src;
	while (n--) {
		*d++ = *s++;
	}
	return dest;
}

void* memset(void* dest, int c, size_t n) {
	unsigned char* d = dest;
	while (n--) {
		*d++ = c;
	}
	return dest;
}

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#define ADD_TO_BUF(buf, bufLen, used, c) 	\
	do {									\
		if (used == bufLen) {				\
			return -EINVAL;					\
		}									\
		buf[used++] = c;					\
	} while (0)

static bool isFlag(char c) {
	return (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0');
}
static bool flagPresent(const char *flags, int nFlags, char flag) {
	for (int i = 0; i < nFlags; i++) {
		if (flags[i] == flag) {
			return true;
		}
	}
	return false;
}

static int printDec(char *buf, int bufLen, unsigned long in, bool sign,
		const char *flags, int nFlags, int width, int precision, char varLength) {
	int ret = 0;
	switch (varLength) {
		case 0:
			in &= 0xFFFFFFFFUL;
			if (sign && in & 0x80000000) {
				in |= 0xFFFFFFFF00000000;
			}
			break;
		case 'h':
			in &= 0xFFFF;
			if (sign && in & 0x8000) {
				in |= 0xFFFFFFFFFFFF0000;
			}
			break;
	}
	if (sign && in & (1UL << 63)) {
		ADD_TO_BUF(buf, bufLen, ret, '-');
		in = (unsigned long)(-(long)in);
	} else if (flagPresent(flags, nFlags, '+')) {
		ADD_TO_BUF(buf, bufLen, ret, '+');
	} else if (flagPresent(flags, nFlags, ' ')) {
		ADD_TO_BUF(buf, bufLen, ret, ' ');
	}

	char temp[100];
	int nLen = 0;
	if (in == 0) {
		temp[0] = '0';
	} else while (in != 0) {
		char c = (in % 10) + '0';
		in /= 10;
		ADD_TO_BUF(temp, 100, nLen, c);
	}
	
	while (precision - nLen > 0) {
		ADD_TO_BUF(temp, 100, nLen, '0');
	}
	char pad = (flagPresent(flags, nFlags, '0'))? '0' : ' ';
	bool lJustify = flagPresent(flags, nFlags, '-');
	if (lJustify) {
		while (width - nLen > 0) {
			ADD_TO_BUF(temp, 100, nLen, pad);
		}
	}

	//now copy temp to buf in reverse
	for (int i = nLen - 1; i >= 0; i--) {
		ADD_TO_BUF(buf, bufLen, ret, temp[i]);
	}
	if (!lJustify) {
		while (width - nLen > 0) {
			ADD_TO_BUF(buf, bufLen, ret, pad);
		}
	}
	return ret;
}

int vfprintf(FILE *stream, const char *format, va_list arg) {
	char buf[512];
	int len = 0;
	int error = 0;

	for (const char *c = format; *c != 0; c++) {
		if (*c != '%') {
			buf[len++] = *c;
			continue;
		}
		c++;

		char flags[5];
		int nFlags = 0;
		int width = -1;
		int precision = -1;
		char varLength = 0;

		//flags
		for (int i = 0; i < 5; i++) {
			if (isFlag(*c)) {
				flags[nFlags++] = *c;
				c++;
			} else {
				break;
			}
		}
		//width
		if (isdigit(*c)) {
			//parse width
		} else if (*c == '*') {
			width = va_arg(arg, int);
			c++;
		}
		//precision
		if (*c == '.') {
			c++;
			if (isdigit(*c)) {

			} else if (*c == '*') {
				precision = va_arg(arg, int);
				c++;
			}
			//invalid
		}
		//varLength
		if (*c == 'h' || *c == 'I' || *c == 'L') {
			varLength = *c;
			c++;
		}

		unsigned long in;
		const char *s;
		switch (*c) {
			case 0:
				errno = -EINVAL;
				return -1;
			case '%':
				ADD_TO_BUF(buf, 512, len, '%');
				break;
			case 'c':
				in = va_arg(arg, unsigned long);
				ADD_TO_BUF(buf, 512, len, in);
				break;
			case 's':
				s = va_arg(arg, const char *);
				while (*s) {
					ADD_TO_BUF(buf, 512, len, *s);
					s++;
				}
				break;
			case 'n':
				break; //print nothing
			case 'd':
			case 'i':
				in = va_arg(arg, unsigned long);
				error = printDec(buf + len, 512 - len, in, true, flags, nFlags, width, precision, varLength);
				if (error < 0) {
					errno = error;
					return -1;
				}
				len += error;
				break;
			case 'u':
				in = va_arg(arg, unsigned long);
				error = printDec(buf + len, 512 - len, in, false, flags, nFlags, width, precision, varLength);
				if (error < 0) {
					errno = error;
					return -1;
				}
				len += error;
				break;
			default:
				errno = -EINVAL;
				return -1;
		}
	}
	buf[len] = 0;
	return fputs(buf, stream);
}

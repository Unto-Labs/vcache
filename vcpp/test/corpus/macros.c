/* Object- and function-like macros, stringification, pasting, recursion. */
#define SQ(x)        ((x) * (x))
#define PAIR(a, b)   a, b
#define STR(x)       #x
#define XSTR(x)      STR(x)
#define CAT(a, b)    a ## b
#define VERSION      3
#define EMPTY()
#define VARIADIC(fmt, ...) printf(fmt, ## __VA_ARGS__)

int   vals[]  = { SQ(3), PAIR(1, 2) };
const char *s = STR(raw text);
const char *v = XSTR(VERSION);
int   CAT(foo, bar) = 1;

/* __LINE__ through macro arguments, including a multi-line invocation --
   the case where -ftrack-macro-expansion changes the answer. */
#define WHERE      __LINE__
#define TAKE(a, b) report(a, b)

int report(int, int);
int here  = __LINE__;
int there = TAKE(1,
                 WHERE);

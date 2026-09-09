/* Conditional compilation, defined(), and the has_* operators a front end
   has to answer for libcpp. */
#if defined(__GNUC__) && __GNUC__ >= 4
int gnuc = 1;
#else
int gnuc = 0;
#endif

#ifdef __has_attribute
#  if __has_attribute(noreturn)
int has_noreturn = 1;
#  endif
#endif

#ifdef __has_builtin
#  if __has_builtin(__builtin_expect)
int has_expect = 1;
#  endif
#endif

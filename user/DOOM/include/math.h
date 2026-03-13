/* math.h — stub for DOOM on ASOS. */
#ifndef _MATH_H
#define _MATH_H
/* DOOM only uses ceil() from math.h (in p_setup.c for reject matrix). */
static inline double ceil(double x)
{
    long i = (long)x;
    if (x > 0.0 && x != (double)i)
        return (double)(i + 1);
    return (double)i;
}
#endif

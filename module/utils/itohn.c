#include "itohn.h"
#include <stdlib.h>

char* itohn(uint64_t n, char buf[7]){
    const char unit[] = { 'k', 'M', 'G', 'T', 'P', 'E' };
    uint8_t idx = 0;
    int8_t len;
    uint16_t _n;
    if (n < 1000)
        return itoa((int)n, buf, 10);
    /*
     * n = n / 1024 * 10 -> n = (n >> 10) * 10
     * trans to: n = ((n >> 3) * 5) >> 6;
     * the (*10) is to preserve decimal precision.
     */
    n >>= 3;
    n *= 5;
    n >>= 6;

    while (n >= 10000LL){
        n >>= 10;
        idx++;
    }

    /* 
     * len = n.tostring().length() + 2, 
     * The 2 stands for decimal point and End abbreviation.
     */
    _n = n;
    if(_n < 100) len = 4;
    else if(_n < 1000) len = 5;
    else len = 6;

    buf[len--] = 0;
    buf[len--] = unit[idx];
    buf[len--] = (_n % 10) + '0'; _n /= 10;
    buf[len--] = '.';
    while(len >= 0){
        buf[len--] = (_n % 10) + '0'; _n /= 10;
    }
    return buf;
}

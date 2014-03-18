#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

BUFFER(double, double);

/* FIXME: assumes YUV420p */

int main(int argc, char **argv)
{
    struct Buffer_double frameDiffs;
    int i;

    INIT_BUFFER(frameDiffs);
    READ_FILE_BUFFER(frameDiffs, stdin);

    for (i = 0; i < frameDiffs.bufused; i++) {
        printf("%f\n", frameDiffs.buf[i]);
    }

    return 0;
}

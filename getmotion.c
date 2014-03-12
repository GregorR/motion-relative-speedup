#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helpers.h"

int main(int argc, char **argv)
{
    unsigned char *lastFrame, *curFrame;
    int w, h, frameSize, i;
    FILE *rawFile;
    double logs[256];

    if (argc < 4) {
        fprintf(stderr, "Use: getmotion <input file/fifo in raw grey format> <width> <height>\n");
        return 1;
    }

    SF(rawFile, fopen, NULL, (argv[1], "rb"));

    w = atoi(argv[2]);
    h = atoi(argv[3]);
    frameSize = w * h;

    SF(lastFrame, calloc, NULL, (frameSize, 1));
    SF(curFrame, malloc, NULL, (frameSize));

    /* precalculate all our logarithms */
    for (i = 0; i < 256; i++)
        logs[i] = log(i + 1);

    /* now calculate the difference frame by frame */
    while (!feof(rawFile) && !ferror(rawFile)) {
        if (fread(curFrame, 1, frameSize, rawFile) == frameSize) {
            double diff = 0;
            /* calculate the difference for this frame */
            for (i = 0; i < frameSize; i++)
                diff += fabs(logs[curFrame[i]] - logs[lastFrame[i]]);
            fwrite(&diff, sizeof(double), 1, stdout);
            memcpy(lastFrame, curFrame, frameSize);
        }
    }

    return 0;
}

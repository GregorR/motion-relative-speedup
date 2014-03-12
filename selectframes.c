#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

struct FrameDiff {
    struct FrameDiff *next;
    char selection;
    int frameNo;
    double frameDiff;
};

BUFFER(double, double);

/* FIXME: assumes YUV420p */

int frameDiffCmp(const struct FrameDiff *l, const struct FrameDiff *r)
{
    if (l->frameDiff == r->frameDiff) {
        if (l->frameNo > r->frameNo) return 1;
        else if (l->frameNo < r->frameNo) return -1;
        else return 0;
    } else {
        if (l->frameDiff > r->frameDiff) return 1;
        else if (l->frameDiff < r->frameDiff) return -1;
        else return 0;
    }
}

int frameDiffCompare(const void *lvp, const void *rvp)
{
    const struct FrameDiff *l = *((const struct FrameDiff **) lvp);
    const struct FrameDiff *r = *((const struct FrameDiff **) rvp);

    return frameDiffCmp(l, r);
}

int frameDiffBSearch(struct FrameDiff *key, struct FrameDiff **arr, size_t ct)
{
    struct FrameDiff **base = arr;
    size_t half;
    int cmp;

retry:
    half = ct / 2;
    /* choose either the left half or the right half */
    cmp = frameDiffCmp(arr[half], key);
    if (cmp < 0) {
        if (ct == 1) {
            /* at the end! */
            return arr + 1 - base;
        }

        /* try the latter half */
        arr += half;
        ct -= half;

    } else if (cmp > 0) {
        if ((half == 0 && base == arr) ||
            frameDiffCmp(arr[half-1], key) < 0) {
            /* found our insertion point */
            return arr + half - base;
        }

        /* try the first half */
        ct -= half;

    } else {
        /* found our insertion point */
        return arr + half - base;

    }
    goto retry;
}

/* find this frame with approximately this frame diff */

int main(int argc, char **argv)
{
    struct Buffer_double frameDiffs;
    char *frameSelections; /* 1 = skip */
    struct FrameDiff **frameDiffMap;
    struct FrameDiff *frameDiff;
    int speedup, dropFrames, i, j;
    int frameSize;
    FILE *inf, *outf;
    char *frame;

    if (argc < 6) {
        fprintf(stderr, "Use: selectframes <input file/fifo in YUV420p> <output file/fifo in YUV420p> <width> <height> <speedup> < <motion file>\n");
        return 1;
    }

    frameSize = atoi(argv[3]) * atoi(argv[4]) * 6 / 4; /* YUV420p */
    speedup = atoi(argv[5]);

    /* first read in our motion data */
    INIT_BUFFER(frameDiffs);
    READ_FILE_BUFFER(frameDiffs, stdin);

    /* get it into the map */
    SF(frameDiffMap, malloc, NULL, (sizeof(struct FrameDiff *) * frameDiffs.bufused));
    frameDiff = NULL;
    for (i = 0; i < frameDiffs.bufused; i++) {
        struct FrameDiff *newFrameDiff;
        SF(newFrameDiff, malloc, NULL, (sizeof(struct FrameDiff)));
        newFrameDiff->next = NULL;
        newFrameDiff->selection = 0;
        newFrameDiff->frameNo = i;
        newFrameDiff->frameDiff = frameDiffs.buf[i];
        frameDiffMap[i] = newFrameDiff;

        if (frameDiff) frameDiff->next = newFrameDiff;
        frameDiff = newFrameDiff;
    }

    /* sort it */
    qsort(frameDiffMap, frameDiffs.bufused, sizeof(struct FrameDiff *), frameDiffCompare);

    /* base our selections at keeping everything */
    SF(frameSelections, calloc, NULL, (frameDiffs.bufused, 1));

    /* now drop the appropriate number of frames */
    dropFrames = frameDiffs.bufused * (speedup - 1) / speedup;
    for (i = 0; i < dropFrames; i++) {
        struct FrameDiff *nFrame;
        if (i % 100 == 0)
            fprintf(stderr, "%d/%d\n", i, dropFrames);

        /* drop this frame */
        frameDiff = frameDiffMap[i];
        frameDiff->selection = 1;
        frameSelections[frameDiff->frameNo] = 1;

        /* find the next unskipped frame */
        for (nFrame = frameDiff->next; nFrame; nFrame = nFrame->next) {
            if (!nFrame->selection)
                break;
        }

        if (nFrame) {
            /* remove it */
            int nFramePos = frameDiffBSearch(nFrame, frameDiffMap + i, frameDiffs.bufused - i) + i;
            if (nFramePos == i) {
                fprintf(stderr, "frameDiffBSearch is very broken!\n");
                exit(1);
            }
            if (frameDiffMap[nFramePos] != nFrame) {
                fprintf(stderr, "frameDiffBSearch is wrong!\n");
                exit(1);
            }
            memmove(frameDiffMap + nFramePos,
                    frameDiffMap + nFramePos + 1,
                    (frameDiffs.bufused - nFramePos - 1) * sizeof(struct FrameDiff *));

            /* change it */
            nFrame->frameDiff += frameDiff->frameDiff / 4;

            /* and readd it */
            nFramePos = frameDiffBSearch(nFrame, frameDiffMap + i, frameDiffs.bufused - i - 1) + i;
            memmove(frameDiffMap + nFramePos + 1,
                    frameDiffMap + nFramePos,
                    (frameDiffs.bufused - nFramePos - 1) * sizeof(struct FrameDiff *));
            frameDiffMap[nFramePos] = nFrame;
        }

        /* sort what remains (FIXME) */
#if 0
        qsort(frameDiffMap + i + 1, frameDiffs.bufused - i - 1, sizeof(struct FrameDiff *), frameDiffCompare);
#endif
    }

    /* make the selection */
    SF(inf, fopen, NULL, (argv[1], "rb"));
    SF(outf, fopen, NULL, (argv[2], "wb"));
    SF(frame, malloc, NULL, (frameSize));
    for (i = 0; i < frameDiffs.bufused && !feof(inf) && !ferror(inf); i++) {
        if (i % 100 == 0)
            fprintf(stderr, "%d/%d\n", i, (int) frameDiffs.bufused);

        /* read in the frame */
        fread(frame, frameSize, 1, inf);

        /* write it out unless skipped */
        if (!frameSelections[i]) {
            fwrite(frame, 1, frameSize, outf);
        }
    }

    return 0;
}

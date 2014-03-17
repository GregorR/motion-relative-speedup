#define _XOPEN_SOURCE 700 /* for atoll and mkdtemp */

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arg.h"
#include "buffer.h"

struct FrameDiff {
    struct FrameDiff *next;
    char selection;
    int frameNo;
    double frameDiff;
};

BUFFER(double, double);

void usage();
void calcMotionData(struct Buffer_double *frameDiffs, const char *inputFile,
                    int width, int height);
void writeMotionData(const char *motionFile, struct Buffer_double *frameDiffs);

int frameDiffCmp(const struct FrameDiff *l, const struct FrameDiff *r);
int frameDiffCompare(const void *lvp, const void *rvp);
int frameDiffBSearch(struct FrameDiff *key, struct FrameDiff **arr, size_t ct);

void calcWindow(struct Buffer_double *frameDiffs, int windowSize);
void mkFrameDiffMap(struct FrameDiff ***frameDiffMapPtr, struct Buffer_double *frameDiffs);
void dropFramesf(unsigned char *frameSelections, unsigned long long frameCount,
                struct FrameDiff **frameDiffMap, unsigned long long dropFrames,
                double clipshowDivisor);
void selectFrames(const char *outputFile, const char *inputFile,
                  unsigned char *frameSelections,
                  unsigned long long frameCount,
                  int width, int height, int fps);

char *ffmpegCommand = "ffmpeg";

int main(int argc, char **argv)
{
    ARG_VARS;

    struct Buffer_double frameDiffs;
    unsigned long long frameCount;
    struct FrameDiff **frameDiffMap;
    unsigned char *frameSelections;

    char *inputFile = NULL, *outputFile = NULL;
    char *motionFile = NULL;
    int motionOnly = 0;
    int width = 0, height = 0;
    int fps = 30;
    int windowSize = 0;
    double clipshowDivisor = 1;
    /* only one of these should be set */
    int speedup = 0;
    unsigned long long dropFrames = 0, keepFrames = 0;

    /* read in our arguments */
    ARG_NEXT();
    while (argType) {
        if (argType != ARG_VAL) {
            ARGNV(m, motion-file, motionFile)
            ARGV(M, motion-only, motionOnly)
            ARGLNV(ffmpeg, ffmpegCommand)
            ARGN(s, speedup) {
                ARG_GET();
                speedup = atoi(arg);
            } else ARGLN(drop-frames) {
                ARG_GET();
                dropFrames = atoi(arg);
            } else ARGLN(keep-frames) {
                ARG_GET();
                keepFrames = atoi(arg);
            } else ARGLN(clipshow-divisor) {
                ARG_GET();
                clipshowDivisor = atof(arg);
            } else ARGN(w, width) {
                ARG_GET();
                width = atoi(arg);
            } else ARGN(h, height) {
                ARG_GET();
                height = atoi(arg);
            } else ARGLN(fps) {
                ARG_GET();
                fps = atoi(arg);
            } else ARG(h, help) {
                usage();
                exit(0);
            } else {
                usage();
                exit(1);
            }
        } else if (inputFile == NULL) {
            inputFile = arg;
        } else if (outputFile == NULL) {
            outputFile = arg;
        } else {
            usage();
            exit(1);
        }

        ARG_NEXT();
    }

    /* validate arguments */
    if (!inputFile) {
        usage();
        exit(1);
    }
    if (motionOnly) {
        if (!motionFile) motionFile = outputFile;
        if (!motionFile) {
            usage();
            exit(1);
        }
    } else if (!outputFile) {
        usage();
        exit(1);
    }
    if (width == 0 || height == 0) {
        usage();
        exit(1);
    }
    if (!motionOnly && 
        ((!speedup && !dropFrames && !keepFrames) ||
         (speedup && (dropFrames || keepFrames)) ||
         (dropFrames && (speedup || keepFrames)) ||
         (keepFrames && (speedup || dropFrames)))) {
        usage();
        exit(1);
    }

    /* first step is to get the motion data */
    INIT_BUFFER(frameDiffs);
    if (motionFile) {
        FILE *motionIn = fopen(motionFile, "rb");
        if (motionIn) {
            /* motion data already present, read it in */
            READ_FILE_BUFFER(frameDiffs, motionIn);
            fclose(motionIn);

        } else {
            /* read it from the input file */
            calcMotionData(&frameDiffs, inputFile, width, height);

            /* and write it out */
            writeMotionData(motionFile, &frameDiffs);

        }

    } else {
        calcMotionData(&frameDiffs, inputFile, width, height);

    }

    if (motionOnly) return 0;

    /* now calculate the number of frames we need to drop */
    frameCount = frameDiffs.bufused;
    if (speedup) {
        dropFrames = frameCount * (speedup - 1) / speedup;
    } else if (keepFrames) {
        dropFrames = frameCount - keepFrames;
    }

    /* adjust the frame diff data for the window size */
    if (windowSize == 0) windowSize = fps / 3;
    if (windowSize > 1) {
        calcWindow(&frameDiffs, windowSize);
    }

    /* get it into the map */
    mkFrameDiffMap(&frameDiffMap, &frameDiffs);

    /* sort it */
    qsort(frameDiffMap, frameCount, sizeof(struct FrameDiff *), frameDiffCompare);

    /* base our selections at keeping everything */
    SF(frameSelections, calloc, NULL, (frameDiffs.bufused, 1));

    /* now drop the appropriate number of frames */
    dropFramesf(frameSelections, frameCount, frameDiffMap, dropFrames, clipshowDivisor);

    /* and write out the new video */
    selectFrames(outputFile, inputFile, frameSelections, frameCount, width, height, fps);
}

void usage()
{
    fprintf(stderr, "USAGE STATEMENT HERE\n");
}

/* calculate the motion data for this input file */
void calcMotionData(struct Buffer_double *frameDiffs, const char *inputFile,
                    int width, int height)
{
    int tmpi;
    pid_t pid;
    FILE *rawData;
    int frameSize;
    unsigned char *lastFrame, *curFrame;
    double logs[256];
    char *tmps;
    char fifo[] = "/tmp/mrspeedup.XXXXXX\0fifo";
    int fifoDirLen = strlen(fifo);
    int i;

    frameSize = width * height;

    /* precalculate all our logarithms */
    for (i = 0; i < 256; i++)
        logs[i] = log(i + 1);

    /* make a temporary directory for our fifo */
    SF(tmps, mkdtemp, NULL, (fifo));
    fifo[fifoDirLen] = '/';

    /* and make the fifo */
    SF(tmpi, mkfifo, -1, (fifo, 0600));

    /* now run ffmpeg */
    SF(pid, fork, -1, ());
    if (pid == 0) {
        SF(tmpi, execlp, -1, (ffmpegCommand, ffmpegCommand,
            "-i", inputFile,
            "-f", "rawvideo", "-pix_fmt", "gray",
            "-y", fifo, NULL));
    }

    /* and calculate the motion data */
    SF(lastFrame, calloc, NULL, (frameSize, 1));
    SF(curFrame, malloc, NULL, (frameSize));
    SF(rawData, fopen, NULL, (fifo, "rb"));

    while (!feof(rawData) && !ferror(rawData)) {
        if (fread(curFrame, 1, frameSize, rawData) == frameSize) {
            double diff = 0;
            /* calculate the difference for this frame */
            for (i = 0; i < frameSize; i++)
                diff += fabs(logs[curFrame[i]] - logs[lastFrame[i]]);
            WRITE_ONE_BUFFER(*frameDiffs, diff);
            memcpy(lastFrame, curFrame, frameSize);
        }
    }

    fclose(rawData);
    free(curFrame);

    unlink(fifo);
    fifo[fifoDirLen] = '\0';
    rmdir(fifo);

    waitpid(pid, NULL, 0);
}

/* write the motion data out to a file */
void writeMotionData(const char *motionFile, struct Buffer_double *frameDiffs)
{
    FILE *fd;

    SF(fd, fopen, NULL, (motionFile, "wd"));
    if (fwrite(frameDiffs->buf, sizeof(double), frameDiffs->bufused, fd) != frameDiffs->bufused) {
        perror(motionFile);
        exit(1);
    }
    fclose(fd);
}

/* compare these frame diffs */
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

/* compare these frame diffs (for qsort) */
int frameDiffCompare(const void *lvp, const void *rvp)
{
    const struct FrameDiff *l = *((const struct FrameDiff **) lvp);
    const struct FrameDiff *r = *((const struct FrameDiff **) rvp);

    return frameDiffCmp(l, r);
}

/* binary search in an array of frame diffs */
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

/* calculate frame windows */
void calcWindow(struct Buffer_double *frameDiffs, int windowSize)
{
    long long i, j;
    for (i = frameDiffs->bufused; i >= 0; i--) {
        double val = 0;
        for (j = i; j > i - windowSize && j >= 0; j--)
            val += frameDiffs->buf[j];
        frameDiffs->buf[i] = val;
    }
}

/* turn frame differences into a sortable map of frame differences */
void mkFrameDiffMap(struct FrameDiff ***frameDiffMapPtr, struct Buffer_double *frameDiffs)
{
    struct FrameDiff **frameDiffMap;
    struct FrameDiff *frameDiff, *newFrameDiff;
    unsigned long long i;

    SF(frameDiffMap, malloc, NULL, (sizeof(struct FrameDiff *) * frameDiffs->bufused));
    frameDiff = NULL;
    for (i = 0; i < frameDiffs->bufused; i++) {
        struct FrameDiff *newFrameDiff;
        SF(newFrameDiff, malloc, NULL, (sizeof(struct FrameDiff)));
        newFrameDiff->next = NULL;
        newFrameDiff->selection = 0;
        newFrameDiff->frameNo = i;
        newFrameDiff->frameDiff = frameDiffs->buf[i];
        frameDiffMap[i] = newFrameDiff;

        if (frameDiff) frameDiff->next = newFrameDiff;
        frameDiff = newFrameDiff;
    }

    *frameDiffMapPtr = frameDiffMap;
}

/* drop frames */
void dropFramesf(unsigned char *frameSelections, unsigned long long frameCount,
                struct FrameDiff **frameDiffMap, unsigned long long dropFrames,
                double clipshowDivisor)
{
    unsigned long long i;
    struct FrameDiff *frameDiff;

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
            int nFramePos = frameDiffBSearch(nFrame, frameDiffMap + i, frameCount - i) + i;
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
                    (frameCount - nFramePos - 1) * sizeof(struct FrameDiff *));

            /* change it */
            if (clipshowDivisor != 0)
                nFrame->frameDiff += frameDiff->frameDiff / clipshowDivisor;

            /* and readd it */
            nFramePos = frameDiffBSearch(nFrame, frameDiffMap + i, frameCount - i - 1) + i;
            memmove(frameDiffMap + nFramePos + 1,
                    frameDiffMap + nFramePos,
                    (frameCount - nFramePos - 1) * sizeof(struct FrameDiff *));
            frameDiffMap[nFramePos] = nFrame;
        }
    }
}

/* make a video of the selected frames */
void selectFrames(const char *outputFile, const char *inputFile,
                  unsigned char *frameSelections,
                  unsigned long long frameCount,
                  int width, int height, int fps)
{
    pid_t pidr, pidw;
    char *tmps;
    int tmpi;
    FILE *inf, *outf;
    unsigned char *frame;
    unsigned long long i;
    int frameSize = width * height * 6 / 4; /* YUV420p */
    char fifor[] = "/tmp/mrspeedup.XXXXXX\0fifo";
    char fifow[] = "/tmp/mrspeedup.XXXXXX\0fifo";
    int fifoDirLen = strlen(fifor);

    /* make our fifos */
    SF(tmps, mkdtemp, NULL, (fifor));
    SF(tmps, mkdtemp, NULL, (fifow));
    fifor[fifoDirLen] = '/';
    fifow[fifoDirLen] = '/';
    SF(tmpi, mkfifo, -1, (fifor, 0600));
    SF(tmpi, mkfifo, -1, (fifow, 0600));

    /* get our reader ffmpeg running */
    SF(pidr, fork, -1, ());
    if (pidr == 0) {
        dup2(open("/dev/null", O_RDONLY), 0);
        SF(tmpi, execlp, -1, (ffmpegCommand, ffmpegCommand,
            "-i", inputFile,
            "-f", "rawvideo",
            "-pix_fmt", "yuv420p",
            "-y", fifor, NULL));
    }

    /* get our writer ffmpeg running */
    SF(pidw, fork, -1, ());
    if (pidw == 0) {
        char fpss[sizeof(unsigned long long)*4+1];
        char vss[sizeof(unsigned long long)*8+2];
        sprintf(fpss, "%d", fps);
        sprintf(vss, "%dx%d", width, height);
        dup2(open("/dev/null", O_RDONLY), 0);
        SF(tmpi, execlp, -1, (ffmpegCommand, ffmpegCommand,
            "-f", "rawvideo",
            "-pixel_format", "yuv420p",
            "-r", fpss,
            "-video_size", vss,
            "-i", fifow,
            "-c:v", "libx264",
            "-crf", "16",
            outputFile, NULL));
    }

    /* make the selection */
    SF(inf, fopen, NULL, (fifor, "rb"));
    SF(outf, fopen, NULL, (fifow, "wb"));
    SF(frame, malloc, NULL, (frameSize));
    for (i = 0; i < frameCount && !feof(inf) && !ferror(inf); i++) {
        /* read in the frame */
        fread(frame, frameSize, 1, inf);

        /* write it out unless skipped */
        if (!frameSelections[i]) {
            fwrite(frame, 1, frameSize, outf);
        }
    }

    waitpid(pidr, NULL, 0);
    waitpid(pidw, NULL, 0);

    free(frame);
    unlink(fifor);
    unlink(fifow);
    fifor[fifoDirLen] = '\0';
    fifow[fifoDirLen] = '\0';
    rmdir(fifor);
    rmdir(fifow);
}

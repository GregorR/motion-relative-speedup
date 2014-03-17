#define _XOPEN_SOURCE 700 /* for atoll and mkdtemp */

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

BUFFER(double, double);

void usage();
void calcMotionData(struct Buffer_double *frameDiffs, const char *inputFile);
void writeMotionData(const char *motionFile, struct Buffer_double *frameDiffs);

char *ffmpegCommand = "ffmpeg";
int width = 0, height = 0;

int main(int argc, char **argv)
{
    ARG_VARS;

    struct Buffer_double frameDiffs;

    char *inputFile = NULL, *outputFile = NULL;
    char *motionFile = NULL;
    int motionOnly = 0;
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
    if ((!speedup && !dropFrames && !keepFrames) ||
        (speedup && (dropFrames || keepFrames)) ||
        (dropFrames && (speedup || keepFrames)) ||
        (keepFrames && (speedup || dropFrames))) {
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
            calcMotionData(&frameDiffs, inputFile);

            /* and write it out */
            writeMotionData(motionFile, &frameDiffs);

        }

    } else {
        calcMotionData(&frameDiffs, inputFile);

    }
}

void usage()
{
    fprintf(stderr, "USAGE STATEMENT HERE\n");
}

void calcMotionData(struct Buffer_double *frameDiffs, const char *inputFile)
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

/*
 * Copyright (c) 2014 Gregor Richards
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _XOPEN_SOURCE 700 /* for atoll, mkdtemp, snprintf */

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
BUFFER(charp, char *);

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

void mkAudioFile(const char *audioFile, const char *inputFile,
                 unsigned char *frameSelections, unsigned long long frameCount,
                 int fps);

char *ffmpegCommand = "ffmpeg";

int main(int argc, char **argv)
{
    ARG_VARS;

    struct Buffer_double frameDiffs;
    unsigned long long frameCount;
    struct FrameDiff **frameDiffMap;
    unsigned char *frameSelections;

    char *inputFile = NULL, *outputFile = NULL, *audioFile = NULL;
    char *motionFile = NULL;
    int motionOnly = 0;
    int width = 0, height = 0;
    int fps = 30;
    int windowSize = 1;
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
            ARGLNV(audio-file, audioFile)
            ARGN(s, speedup) {
                ARG_GET();
                speedup = atoi(arg);
            } else ARGLN(drop-frames) {
                ARG_GET();
                dropFrames = atoi(arg);
            } else ARGLN(keep-frames) {
                ARG_GET();
                keepFrames = atoi(arg);
            } else ARGLN(window) {
                ARG_GET();
                windowSize = atoi(arg);
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

    /* make the audio file */
    if (audioFile) mkAudioFile(audioFile, inputFile, frameSelections, frameCount, fps);

    /* and write out the new video */
    selectFrames(outputFile, inputFile, frameSelections, frameCount, width, height, fps);

    return 0;
}

void usage()
{
    fprintf(stderr,
        "Usage: mrspeedup -w <video width> -h <video height>\n"
        "       {-s <speedup>|--drop-frames <#>|--keep-frames <#>} [options]\n"
        "       <input video> <output video>\n"
        "Flags:\n"
        "\t-w|--width <video width>\n"
        "\t\t(Required) Specify video width.\n"
        "\t-h|--height <video height>\n"
        "\t\t(Required) Specify video height.\n"
        "\t-s|--speedup <speedup>\n"
        "\t\tAverage speedup, used to calculate number of frames to drop.\n"
        "\t--drop-frames <#>\n"
        "\t\tAs an alternative to average speedup, precise number of frames to\n"
        "\t\tdrop.\n"
        "\t--keep-frames <#>\n"
        "\t\tSimilar to --drop-frames, but number of frames to keep.\n"
        "\t-m|--motion-data <file>\n"
        "\t\tWrite/read motion data to/from the specified file.\n"
        "\t-M|--motion-only\n"
        "\t\tOnly calculate motion data, do not perform speedup. Motion data\n"
        "\t\twill be written to the motion data file if specified, or the\n"
        "\t\toutput file which would be used for the video otherwise.\n"
        "\t--fps <#>\n"
        "\t\tSpecify video FPS. Default 30.\n"
        "\t--ffmpeg <cmd>\n"
        "\t\tSpecify ffmpeg binary. Default \"ffmpeg\".\n"
        "\t--window <#>\n"
        "\t\tNumber of frames in the motion window. Default is 1 (i.e., no\n"
        "\t\twindow). '0' will select 1/3rd of a second.\n"
        "\t--clipshow-divisor <#>\n"
        "\t\tSpecify the \"clipshow divisor\". Larger values put more emphasis\n"
        "\t\ton keeping frames which are active in the original than on keeping\n"
        "\t\tan equal amount of action per frame; i.e., it creates a sort of\n"
        "\t\t\"clip show\". Values less than 1 are valid. 0 is interpreted as\n"
        "\t\tinfinity, which will give priority ONLY to keeping active frames\n"
        "\t\tin the original. Default 1.\n");
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

    waitpid(pid, NULL, 0);

    unlink(fifo);
    fifo[fifoDirLen] = '\0';
    rmdir(fifo);
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
    struct FrameDiff *frameDiff;
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
            fprintf(stderr, "Dropping frames: %d/%d\r", (int) i, (int) dropFrames);

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
    fprintf(stderr, "\n");
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
    fclose(inf);
    fclose(outf);

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

/* make the audio file */
void mkAudioFile(const char *audioFile, const char *inputFile,
                 unsigned char *frameSelections, unsigned long long frameCount,
                 int fps)
{
    char flacf[] = "/tmp/mrspeedup.XXXXXX\0a.flac";
    int flacfLen = strlen(flacf);

    char *tmps;
    int i, tmpi;
    unsigned long long frame;
    pid_t pid;
    struct Buffer_charp args, allocatedArgs;
    double inlen, outlen, framelen;

    SF(tmps, mkdtemp, NULL, (flacf));
    flacf[flacfLen] = '/';

    /* get out the audio data */
    SF(pid, fork, -1, ());
    if (pid == 0) {
        SF(tmpi, execlp, -1, (ffmpegCommand, ffmpegCommand,
            "-i", inputFile,
            flacf, NULL));
    }
    waitpid(pid, NULL, 0);

    /* make the sox command */
    INIT_BUFFER(args);
    INIT_BUFFER(allocatedArgs);

    WRITE_ONE_BUFFER(args, "sox");
    WRITE_ONE_BUFFER(args, (char *) flacf);
    WRITE_ONE_BUFFER(args, (char *) audioFile);

    framelen = 1.0 / fps;
    inlen = outlen = 0;
    for (frame = 0; frame < frameCount; frame++) {
        inlen += framelen;
        if (!frameSelections[frame]) outlen += framelen;

        /* output it if applicable */
        if (outlen >= 0.1 || frame == frameCount - 1) {
            double speedup, tempoup;
            char *trimBuf, *speedBuf, *tempoBuf;

            if (inlen == 0 || outlen == 0) inlen = outlen = 0.1;
            speedup = 1;
            tempoup = inlen / outlen;

            /* divide the speedup into speed and tempo */
            if (tempoup >= 10) {
                if (tempoup >= 40) {
                    speedup = 4;
                    tempoup /= 4;
                } else {
                    speedup = tempoup / 10;
                    tempoup /= speedup;
                }
            }

            /* first trim */
            WRITE_ONE_BUFFER(args, "trim");
            WRITE_ONE_BUFFER(args, "0");
            SF(trimBuf, malloc, NULL, (32));
            snprintf(trimBuf, 32, "%.32f", inlen);
            WRITE_ONE_BUFFER(args, trimBuf);
            WRITE_ONE_BUFFER(allocatedArgs, trimBuf);

            /* then speedup */
            if (speedup != 1) {
                WRITE_ONE_BUFFER(args, "speed");
                SF(speedBuf, malloc, NULL, (32));
                snprintf(speedBuf, 32, "%.32f", speedup);
                WRITE_ONE_BUFFER(args, speedBuf);
                WRITE_ONE_BUFFER(allocatedArgs, speedBuf);

                WRITE_ONE_BUFFER(args, "rate");
                WRITE_ONE_BUFFER(args, "48k");
            }

            if (tempoup != 1) {
                WRITE_ONE_BUFFER(args, "tempo");
                SF(tempoBuf, malloc, NULL, (32));
                snprintf(tempoBuf, 32, "%.32f", tempoup);
                WRITE_ONE_BUFFER(args, tempoBuf);
                WRITE_ONE_BUFFER(allocatedArgs, tempoBuf);
            }

            WRITE_ONE_BUFFER(args, "trim");
            WRITE_ONE_BUFFER(args, "0");
            SF(trimBuf, malloc, NULL, (32));
            snprintf(trimBuf, 32, "%.32f", outlen);
            WRITE_ONE_BUFFER(args, trimBuf);
            WRITE_ONE_BUFFER(allocatedArgs, trimBuf);

            WRITE_ONE_BUFFER(args, ":");
            inlen = outlen = 0;
        }
    }

    for (i = 0; i < args.bufused; i++)
        fprintf(stderr, "%s ", args.buf[i]);
    fprintf(stderr, "\n");

    WRITE_ONE_BUFFER(args, "trim");
    WRITE_ONE_BUFFER(args, "0");
    WRITE_ONE_BUFFER(args, "0");
    WRITE_ONE_BUFFER(args, NULL);

    SF(pid, fork, -1, ());
    if (pid == 0) {
        /* run sox */
        SF(tmpi, execvp, -1, (args.buf[0], args.buf));
    }
    waitpid(pid, NULL, 0);

    for (i = 0; i < allocatedArgs.bufused; i++)
        free(allocatedArgs.buf[i]);

    FREE_BUFFER(allocatedArgs);
    FREE_BUFFER(args);

    unlink(flacf);
    flacf[flacfLen] = '\0';
    rmdir(flacf);
}

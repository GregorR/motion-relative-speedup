#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    double oriX, avgX, finX, oriY, avgY, finY, oriA, avgA, finA, oriZ, avgZ, finZ;
    char nl;

    /* skip the header line */
    if (scanf("Ori x, Avg x, Fin x, Ori y, Avg y, Fin y, Ori angle, Avg angle, Fin angle, Ori zoom, Avg zoom, Fin zoom%c", &nl) != 1 ||
        nl != '\n') {
        fprintf(stderr, "This does not appear to be a deshake log!\n");
    }

    while (scanf("%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf\n",
                 &oriX, &avgX, &finX,
                 &oriY, &avgY, &finY,
                 &oriA, &avgA, &finA,
                 &oriZ, &avgZ, &finZ) == 12) {
        double res = fabs(finX - oriX) + fabs(finY - oriY);
        fwrite(&res, sizeof(double), 1, stdout);
    }

    return 0;
}

/* Benchmark CLI for N.148i — part of the N.148 project (MIT, (c) Micilini Roll) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "header.h"
#include "tables.h"
#include "ppm.h"
#include "encoder.h"
#include "decoder.h"
#include "parallel.h"

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: bench in.ppm quality [reps] [chroma] [threads]\n"); return 1; }
    const char *in = argv[1];
    int quality = atoi(argv[2]);
    int reps = (argc>3)?atoi(argv[3]):10;
    int chroma = (argc>4)?atoi(argv[4]):CHROMA_420;
    if (argc > 5) n148_set_thread_count(atoi(argv[5]));

    Image original;
    if (!load_ppm(in, &original)) return 1;

    /* --- ENCODE timing (color conversion + entropy coding) --- */
    double t = now();
    unsigned char *compressed = NULL; EncodeStats st; HuffSpec specs[4];
    for (int i=0;i<reps;i++){
        Plane y,cb,cr;
        split_channels(&original,&y,&cb,&cr,chroma);
        if (compressed) { free(compressed); compressed=NULL; }
        encode_image(&y,&cb,&cr,quality,1,specs,&compressed,&st);
        free_plane(&y); free_plane(&cb); free_plane(&cr);
    }
    double t_enc = (now()-t)/reps*1000;

    long total = 21 + st.table_size + st.data_size;

    /* --- DECODE timing --- */
    t = now();
    Image dec; memset(&dec,0,sizeof(dec));
    for (int i=0;i<reps;i++){
        Plane dy,dcb,dcr; DecodeStats ds;
        decode_image(compressed, st.data_size, original.width, original.height,
                     quality, chroma, specs, &dy,&dcb,&dcr,&ds);
        if (dec.pixels) free_image(&dec);
        merge_channels(&dy,&dcb,&dcr,1,&dec);
        free_plane(&dy); free_plane(&dcb); free_plane(&dcr);
    }
    double t_dec = (now()-t)/reps*1000;

    /* --- PSNR --- */
    double mse=0; long n=(long)original.width*original.height*3;
    for (long i=0;i<n;i++){ double d=(double)original.pixels[i]-(double)dec.pixels[i]; mse+=d*d; }
    mse/=n;
    double psnr = (mse==0)?999.0:10.0*log10(255.0*255.0/mse);

    printf("%-14s %dx%d q=%d | enc %7.2f ms | dec %7.2f ms | %8ld B | PSNR %6.2f dB\n",
           in, original.width, original.height, quality, t_enc, t_dec, total, psnr);
    return 0;
}

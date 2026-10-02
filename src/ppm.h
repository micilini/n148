#ifndef PPM_H
#define PPM_H

#include <stddef.h>

// An RGB image loaded from a PPM file.
typedef struct {
    int width;
    int height;
    unsigned char *pixels; // RGB interleaved: R,G,B,R,G,B...
} Image;

// A single 8-bit channel (Y, Cb, or Cr). Chroma planes can be
// smaller than the image when subsampling is enabled.
typedef struct {
    int width;
    int height;
    unsigned char *data;
} Plane;

int load_ppm(const char *path, Image *image);
int save_ppm(const char *path, Image *image);
void free_image(Image *image);

Plane create_plane(int width, int height);
void free_plane(Plane *plane);

// Reads a pixel while clamping coordinates to the plane edges.
// This provides padding for partial 8x8 blocks.
int plane_sample(Plane *plane, int x, int y);

// Calculates the stored chroma-plane size for a subsampling mode.
void chroma_dimensions(int mode, int width, int height, int *cw, int *ch);

// Splits an RGB image into Y, Cb, and Cr planes.
int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr,
                   int chroma_mode);

/* Select the deterministic perceptual colour transform while
   retaining the same three-plane and chroma-subsampling layout. */
int n148_split_channels_ex(Image *image, Plane *first, Plane *second,
                           Plane *third, int chroma_mode,
                           int perceptual_color);

/* Encoder-side reconstruction-aware 4:2:2/4:2:0 colour preparation.
   minimizes error after the decoder's normative 3:1 interpolation on each
   reduced axis; 4:4:4 falls back to the ordinary full-resolution path. */
int n148_split_channels_reconstruction_aware(Image *image, Plane *y, Plane *cb, Plane *cr,
                           int chroma_mode, int reconstruction_aware);

/* Non-owning reference valid for one complete encoder invocation. */
typedef struct {
    const unsigned char *pixels;
    size_t stride;
    int width, height;
} N148RgbSource;

/* Encoder-only luma preparation from retained quantized/filtered chroma. */
int n148_project_luma_from_chroma(const N148RgbSource *reference,
    const Plane *source, const Plane *cb, const Plane *cr, Plane *corrected);

int save_pgm(const char *path, Plane *plane);

#endif

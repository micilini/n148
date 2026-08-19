#ifndef PPM_H
#define PPM_H

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

// Splits an RGB image into Y, Cb, and Cr planes.
// subsample != 0 halves both chroma dimensions (4:2:0).
int split_channels(Image *image, Plane *y, Plane *cb, Plane *cr, int subsample);

int save_pgm(const char *path, Plane *plane);

#endif

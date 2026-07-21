/* Chair model data for TRUE-3D rendering. draw_chair_3d() in raycast.c
 * projects these vertices through the live raycaster camera (perspective
 * divide per vertex) and rasterizes the faces with a per-column wall z-test —
 * it is real world-anchored geometry, NOT a billboard. Walk around it and it
 * is correctly oriented from every side, no view-lock hat-trick.
 *
 * Nine axis-aligned boxes (seat, two front legs, back posts split at the
 * seat plane, two back slats) in 8.8 fixed model units: feet at y=0, height
 * 1.0 (=256), centred in x/z. Scaled to CHAIR_WORLD_H_FX world units at
 * render. */
#ifndef CHAIR3D_H
#define CHAIR3D_H
#include <stdint.h>

#define CHAIR_SPRITE_KIND 3
#define CHAIR_WORLD_H_FX  24576          /* FX(0.375): world height of model 1.0 */
#define CHAIR_NBOXES 9

typedef struct { int16_t x0, y0, z0, x1, y1, z1; } cbox_t;
#define CM(v) ((int16_t)((v) * 256))
/* NO box interpenetrates or spans past another's occlusion plane: the painter
 * sort keys on per-triangle centroid depth, and any triangle whose depth span
 * straddles a neighbour's can win the sort while losing the geometry (post
 * slivers painted through the seat top, rail notches through the posts).
 * Hence: posts split at the seat-top plane (0.48), rails butted against the
 * posts' inner faces (|x| <= 0.20), seat flush behind at z=0.20. With disjoint
 * boxes a strict painting order exists and painter is exact. */
static const cbox_t chair_boxes[CHAIR_NBOXES] = {
    { CM(-0.26), CM(0.42), CM(-0.26), CM( 0.26), CM(0.48), CM( 0.20) },  /* seat */
    { CM(-0.26), CM(0.00), CM(-0.26), CM(-0.20), CM(0.42), CM(-0.20) },  /* front-L */
    { CM( 0.20), CM(0.00), CM(-0.26), CM( 0.26), CM(0.42), CM(-0.20) },  /* front-R */
    { CM(-0.26), CM(0.00), CM( 0.20), CM(-0.20), CM(0.48), CM( 0.26) },  /* post BL low */
    { CM( 0.20), CM(0.00), CM( 0.20), CM( 0.26), CM(0.48), CM( 0.26) },  /* post BR low */
    { CM(-0.26), CM(0.48), CM( 0.20), CM(-0.20), CM(1.00), CM( 0.26) },  /* post BL up */
    { CM( 0.20), CM(0.48), CM( 0.20), CM( 0.26), CM(1.00), CM( 0.26) },  /* post BR up */
    { CM(-0.20), CM(0.90), CM( 0.21), CM( 0.20), CM(1.00), CM( 0.25) },  /* top rail */
    { CM(-0.20), CM(0.68), CM( 0.215),CM( 0.20), CM(0.76), CM( 0.245) }, /* mid slat */
};
#undef CM

/* Box corner index bits: bit0 = x1, bit1 = y1, bit2 = z1. Faces CCW from
 * outside; index by outward-normal axis 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z. */
static const uint8_t chair_face_v[6][4] = {
    { 1, 3, 7, 5 },   /* +x */
    { 0, 4, 6, 2 },   /* -x */
    { 2, 6, 7, 3 },   /* +y (top)    */
    { 0, 1, 5, 4 },   /* -y (bottom) */
    { 4, 5, 7, 6 },   /* +z */
    { 0, 2, 3, 1 },   /* -z */
};

#endif /* CHAIR3D_H */

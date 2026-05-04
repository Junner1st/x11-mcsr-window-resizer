#ifndef MC_CENTERING_H
#define MC_CENTERING_H

#include <stdbool.h>

typedef struct {
    int x;
    int y;
    unsigned int w;
    unsigned int h;
} McCenteringRect;

double mc_centering_sanitize_ratio(double ratio, double fallback);
bool mc_centering_source_rect(int origin_x,
                              int origin_y,
                              unsigned int width,
                              unsigned int height,
                              double center_ratio,
                              McCenteringRect *rect);

#endif

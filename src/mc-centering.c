#include "mc-centering.h"

#include <limits.h>

double mc_centering_sanitize_ratio(double ratio, double fallback) {
    if (!(ratio > 0.0) || ratio != ratio) {
        return fallback;
    }
    if (ratio > 1.0) {
        return 1.0;
    }
    return ratio;
}

static unsigned int source_dimension(unsigned int dimension, double ratio) {
    double scaled = (double)dimension * ratio;
    if (scaled < 1.0) {
        return 1;
    }
    if (scaled > (double)UINT_MAX) {
        return UINT_MAX;
    }
    return (unsigned int)(scaled + 0.5);
}

static int centered_coordinate(int monitor_origin,
                               unsigned int monitor_size,
                               unsigned int window_size) {
    long long coord = (long long)monitor_origin +
                      (((long long)monitor_size - (long long)window_size) / 2);
    if (coord < INT_MIN) {
        return INT_MIN;
    }
    if (coord > INT_MAX) {
        return INT_MAX;
    }
    return (int)coord;
}

bool mc_centering_source_rect(int origin_x,
                              int origin_y,
                              unsigned int width,
                              unsigned int height,
                              double center_ratio,
                              McCenteringRect *rect) {
    if (!rect || width == 0 || height == 0) {
        return false;
    }

    double ratio = mc_centering_sanitize_ratio(center_ratio, 1.0);
    unsigned int w = source_dimension(width, ratio);
    unsigned int h = source_dimension(height, ratio);

    rect->w = w;
    rect->h = h;
    rect->x = centered_coordinate(origin_x, width, w);
    rect->y = centered_coordinate(origin_y, height, h);
    return true;
}

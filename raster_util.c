/* raster_util.c - Float-to-int conversion without UB. */
#include "raster_internal.h"

int svg_f2i(float v, int lo, int hi, int nan_val) {
    if (!(v == v)) return nan_val;              /* NaN */
    if (v <= (float)lo) return lo;
    if (v >= (float)hi) return hi;
    return (int)v;
}

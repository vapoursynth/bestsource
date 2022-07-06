#include "BSRational.h"

extern "C" {
#include <libavutil/rational.h>
}

BSRational::BSRational(const AVRational &r) {
    num = r.num;
    den = r.den;
}

double BSRational::ToDouble() const {
    return num / (double)den;
}

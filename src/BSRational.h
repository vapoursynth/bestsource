#ifndef BSRATIONAL_H
#define BSRATIONAL_H

struct AVRational;

struct BSRational {
    int num;
    int den;
    BSRational() = default;
    BSRational(const AVRational &r);
    double ToDouble() const;
};

#endif
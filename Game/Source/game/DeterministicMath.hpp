#pragma once

#include <cmath>

// Sine and cosine that give the same bits on every platform the game runs on (Windows x64 with UCRT, Linux x64 and
// ARM64 with glibc). std::cos/std::sin come from each C runtime and differ in the last bit between them; client
// prediction compares positions with == against an ARM64 server, so a 1-ULP different heading made almost every
// frame reconcile (alpha test 2026-11-11). Built only from +, -, * and floor, which IEEE 754 rounds the same
// everywhere as long as the compiler does not fuse them into FMAs (-ffp-contract=off in Build.lua; MSVC /fp:precise
// does not contract on x64).
//
// Accuracy: a couple of ULP from the true value, far below anything visible; what matters is that it is the same
// value on both ends.
namespace DetMath
{
    // Degrees in, any range that fits a float; outputs sin and cos of that angle.
    inline void SinCosDeg(float degrees, float& outSin, float& outCos)
    {
        // Nearest quarter turn: r = degrees - 90*k lands in [-45, 45].
        const float k = std::floor(degrees * (1.0f / 90.0f) + 0.5f);
        const float rDeg = degrees - k * 90.0f;
        const float r = rDeg * 0.017453292519943295f;   // pi / 180
        const float r2 = r * r;

        // Taylor series, enough terms for float precision on |r| <= pi/4.
        const float s = r * (1.0f + r2 * (-1.0f / 6.0f + r2 * (1.0f / 120.0f + r2 * (-1.0f / 5040.0f + r2 * (1.0f / 362880.0f)))));
        const float c = 1.0f + r2 * (-0.5f + r2 * (1.0f / 24.0f + r2 * (-1.0f / 720.0f + r2 * (1.0f / 40320.0f))));

        // k mod 4, also for negative k.
        int quadrant = static_cast<int>(k) % 4;
        if (quadrant < 0) quadrant += 4;

        switch (quadrant)
        {
        case 0:  outSin = s;  outCos = c;  break;
        case 1:  outSin = c;  outCos = -s; break;
        case 2:  outSin = -s; outCos = -c; break;
        default: outSin = -c; outCos = s;  break;
        }
    }
}

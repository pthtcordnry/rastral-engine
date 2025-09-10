#pragma once
#ifndef MATRIX_HELPER_H
#define MATRIX_HELPER_H

#include <cmath>

struct Mat4 { float m[16]; };

Mat4 matIdentity() {
    Mat4 r; 
    for (int i = 0; i < 16; i++) {
        r.m[i] = 0.0f;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    }
    return r;
}

Mat4 matMul(const Mat4& A, const Mat4& B) {
    Mat4 R; 
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            R.m[c * 4 + r] = A.m[0 * 4 + r] * B.m[c * 4 + 0] + A.m[1 * 4 + r] * B.m[c * 4 + 1] + A.m[2 * 4 + r] * B.m[c * 4 + 2] + A.m[3 * 4 + r] * B.m[c * 4 + 3];
        }
    }
    return R;
}

Mat4 matPerspective(float fovyRad, float aspect, float zNear, float zFar) {
    Mat4 M{};                    // all zeros
    const float f = 1.0f / std::tan(0.5f * fovyRad);
    M.m[0] = f / aspect;
    M.m[5] = f;
    M.m[10] = (zFar + zNear) / (zNear - zFar);
    M.m[11] = -1.0f;
    M.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    return M;
}

Mat4 Mat4Translate(float x, float y, float z) {
    Mat4 M = matIdentity(); 
    M.m[12] = x; 
    M.m[13] = y; 
    M.m[14] = z; 
    return M;
}

Mat4 matLookAt(float ex, float ey, float ez,
    float cx, float cy, float cz,
    float ux, float uy, float uz) {
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = 1.0f / std::sqrt(fx * fx + fy * fy + fz * fz);
    fx *= fl; fy *= fl; fz *= fl;

    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float sl = 1.0f / std::sqrt(sx * sx + sy * sy + sz * sz);
    sx *= sl; sy *= sl; sz *= sl;

    float ux2 = sy * fz - sz * fy;
    float uy2 = sz * fx - sx * fz;
    float uz2 = sx * fy - sy * fx;

    Mat4 M = matIdentity();
    M.m[0] = sx;   M.m[4] = sy;   M.m[8] = sz;
    M.m[1] = ux2;  M.m[5] = uy2;  M.m[9] = uz2;
    M.m[2] = -fx;  M.m[6] = -fy;  M.m[10] = -fz;

    M.m[12] = -(sx * ex + sy * ey + sz * ez);
    M.m[13] = -(ux2 * ex + uy2 * ey + uz2 * ez);
    M.m[14] = (fx * ex + fy * ey + fz * ez);
    return M;
}

Mat4 matRotateY(float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    Mat4 M = matIdentity(); 
    M.m[0] = c; 
    M.m[8] = s; 
    M.m[2] = -s; 
    M.m[10] = c; 
    return M;
}

#endif // MATRIX_HELPER_H

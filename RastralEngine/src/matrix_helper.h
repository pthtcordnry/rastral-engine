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

Mat4 matRotateX(float a) {
    Mat4 R = matIdentity();
    float c = std::cos(a), s = std::sin(a);
    R.m[5] = c;  R.m[9] = -s;
    R.m[6] = s;  R.m[10] = c;
    return R;
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

Mat4 matScale(float sx, float sy, float sz) {
    Mat4 M = matIdentity();
    M.m[0] = sx;
    M.m[5] = sy;
    M.m[10] = sz;
    return M;
}

void normalize3(float& x, float& y, float& z) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
}

void cross3(float ax, float ay, float az,
    float bx, float by, float bz,
    float& rx, float& ry, float& rz) {
    rx = ay * bz - az * by;
    ry = az * bx - ax * bz;
    rz = ax * by - ay * bx;
}

float dot3(float ax, float ay, float az,
    float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

Mat4 matFromQuat(float x, float y, float z, float w) {
    Mat4 R = matIdentity();
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    R.m[0] = 1 - 2 * (yy + zz); R.m[4] = 2 * (xy - wz);   R.m[8] = 2 * (xz + wy);
    R.m[1] = 2 * (xy + wz);   R.m[5] = 1 - 2 * (xx + zz); R.m[9] = 2 * (yz - wx);
    R.m[2] = 2 * (xz - wy);   R.m[6] = 2 * (yz + wx);   R.m[10] = 1 - 2 * (xx + yy);
    return R;
}

Mat4 matTRS(float tx, float ty, float tz, float qx, float qy, float qz, float qw, float sx, float sy, float sz) {
    return matMul(Mat4Translate(tx, ty, tz), matMul(matFromQuat(qx, qy, qz, qw), matScale(sx, sy, sz)));
}

void xformPoint(const Mat4& M, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = M.m[0] * x + M.m[4] * y + M.m[8] * z + M.m[12];
    oy = M.m[1] * x + M.m[5] * y + M.m[9] * z + M.m[13];
    oz = M.m[2] * x + M.m[6] * y + M.m[10] * z + M.m[14];
}

static inline float DegToRad(float d) { return d * 3.1415926535f / 180.0f; }

// Given bounding-sphere radius r, vertical FOV, and aspect, return distance that fits both width & height
static float DistanceToFitSphere(float r, float vfovRad, float aspect)
{
    // vertical constraint
    float dV = r / std::tan(vfovRad * 0.5f);
    // horizontal constraint: hfov from vfov & aspect
    float hfov = 2.0f * std::atan(std::tan(vfovRad * 0.5f) * aspect);
    float dH = r / std::tan(hfov * 0.5f);
    // a little padding so it doesn't kiss the edges
    float d = std::max(dV, dH);
    return d * 1.15f; // 15% margin
}

#endif // MATRIX_HELPER_H

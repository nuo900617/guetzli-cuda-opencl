// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: Jyrki Alakuijala (jyrki.alakuijala@gmail.com)
//
// The physical architecture of butteraugli is based on the following naming
// convention:
//   * Opsin - dynamics of the photosensitive chemicals in the retina
//             with their immediate electrical processing
//   * Xyb - hybrid opponent/trichromatic color space
//     x is roughly red-subtract-green.
//     y is yellow.
//     b is blue.
//     Xyb values are computed from Opsin mixing, not directly from rgb.
//   * Mask - for visual masking
//   * Hf - color modeling for spatially high-frequency features
//   * Lf - color modeling for spatially low-frequency features
//   * Diffmap - to cluster and build an image of error between the images
//   * Blur - to hold the smoothing code

#include "butteraugli/butteraugli.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>

#include "clguetzli\clguetzli.h"

// Restricted pointers speed up Convolution(); MSVC uses a different keyword.
#ifdef _MSC_VER
#define __restrict__ __restrict
#endif

namespace butteraugli {

static const double kInternalGoodQualityThreshold = 14.921561160295326;
static const double kGlobalScale = 1.0 / kInternalGoodQualityThreshold;

inline double DotProduct(const double u[3], const double v[3]) {
  return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
}

inline double DotProduct(const float u[3], const double v[3]) {
  return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
}

// Computes a horizontal convolution and transposes the result.
static void Convolution(size_t xsize, size_t ysize,
	size_t xstep,
	size_t len, size_t offset,
	const float* __restrict__ multipliers,
	const float* __restrict__ inp,
	float border_ratio,
	float* __restrict__ result) {
  PROFILER_FUNC;
  float weight_no_border = 0;

  for (size_t j = 0; j <= 2 * offset; ++j) {
    weight_no_border += multipliers[j];
  }
  for (size_t x = 0, ox = 0; x < xsize; x += xstep, ox++) {
    int minx = x < offset ? 0 : x - offset;
    int maxx = std::min(xsize, x + len - offset) - 1;
    float weight = 0.0;
    for (int j = minx; j <= maxx; ++j) {
      weight += multipliers[j - x + offset];
    }
    // Interpolate linearly between the no-border scaling and border scaling.
    weight = (1.0 - border_ratio) * weight + border_ratio * weight_no_border;
    float scale = 1.0 / weight;
    for (size_t y = 0; y < ysize; ++y) {
      float sum = 0.0;
      for (int j = minx; j <= maxx; ++j) {
        sum += inp[y * xsize + j] * multipliers[j - x + offset];
      }
      result[ox * ysize + y] = static_cast<float>(sum * scale);
    }
  }
}

void Blur(size_t xsize, size_t ysize, float* channel, double sigma,
          double border_ratio) {

  PROFILER_FUNC;
  double m = 2.25;  // Accuracy increases when m is increased.
  const double scaler = -1.0 / (2 * sigma * sigma);
  // For m = 9.0: exp(-scaler * diff * diff) < 2^ {-52}
  const int diff = std::max<int>(1, m * fabs(sigma));
  const int expn_size = 2 * diff + 1;
  std::vector<float> expn(expn_size);
  for (int i = -diff; i <= diff; ++i) {
    expn[i + diff] = static_cast<float>(exp(scaler * i * i));
  }
  const int xstep = std::max(1, int(sigma / 3));
  const int ystep = xstep;
  int dxsize = (xsize + xstep - 1) / xstep;
  int dysize = (ysize + ystep - 1) / ystep;
  std::vector<float> tmp(dxsize * ysize);
  Convolution(xsize, ysize, xstep, expn_size, diff, expn.data(), channel,
              border_ratio,
              tmp.data());
  float* output = channel;
  std::vector<float> downsampled_output;
  if (xstep > 1) {
    downsampled_output.resize(dxsize * dysize);
    output = downsampled_output.data();
  }
  Convolution(ysize, dxsize, ystep, expn_size, diff, expn.data(), tmp.data(),
              border_ratio, output);
  if (xstep > 1) {
    for (size_t y = 0; y < ysize; y++) {
      for (size_t x = 0; x < xsize; x++) {
        // TODO: Use correct rounding.
        channel[y * xsize + x] =
            downsampled_output[(y / ystep) * dxsize + (x / xstep)];
      }
    }
  }
}

// To change this to n, add the relevant FFTn function and kFFTnMapIndexTable.
constexpr size_t kBlockEdge = 8;
constexpr size_t kBlockSize = kBlockEdge * kBlockEdge;
constexpr size_t kBlockEdgeHalf = kBlockEdge / 2;
constexpr size_t kBlockHalf = kBlockEdge * kBlockEdgeHalf;

// Contrast sensitivity related weights.
static const double *GetContrastSensitivityMatrix() {
  static double csf8x8[kBlockHalf + kBlockEdgeHalf + 1] = {
    5.28270670524,
    0.0,
    0.0,
    0.0,
    0.3831134973,
    0.676303603859,
    3.58927792424,
    18.6104367002,
    18.6104367002,
    3.09093131948,
    1.0,
    0.498250875965,
    0.36198671102,
    0.308982169883,
    0.1312701920435,
    2.37370549629,
    3.58927792424,
    1.0,
    2.37370549629,
    0.991205724152,
    1.05178802919,
    0.627264168628,
    0.4,
    0.1312701920435,
    0.676303603859,
    0.498250875965,
    0.991205724152,
    0.5,
    0.3831134973,
    0.349686450518,
    0.627264168628,
    0.308982169883,
    0.3831134973,
    0.36198671102,
    1.05178802919,
    0.3831134973,
    0.12,
  };
  return &csf8x8[0];
}

std::array<double, 21> MakeHighFreqColorDiffDx() {
  std::array<double, 21> lut;
  static const double off = 11.38708334481672;
  static const double inc = 14.550189611520716;
  lut[0] = 0.0;
  lut[1] = off;
  for (int i = 2; i < 21; ++i) {
    lut[i] = lut[i - 1] + inc;
  }
  return lut;
}

const double *GetHighFreqColorDiffDx() {
  static const std::array<double, 21> kLut = MakeHighFreqColorDiffDx();
  return kLut.data();
}

std::array<double, 21> MakeHighFreqColorDiffDy() {
  std::array<double, 21> lut;
  static const double off = 1.4103373714040413;
  static const double inc = 0.7084088867024;
  lut[0] = 0.0;
  lut[1] = off;
  for (int i = 2; i < 21; ++i) {
    lut[i] = lut[i - 1] + inc;
  }
  return lut;
}

const double *GetHighFreqColorDiffDy() {
  static const std::array<double, 21> kLut = MakeHighFreqColorDiffDy();
  return kLut.data();
}

std::array<double, 21> MakeLowFreqColorDiffDy() {
  std::array<double, 21> lut;
  static const double inc = 5.2511644570349185;
  lut[0] = 0.0;
  for (int i = 1; i < 21; ++i) {
    lut[i] = lut[i - 1] + inc;
  }
  return lut;
}

const double *GetLowFreqColorDiffDy() {
  static const std::array<double, 21> kLut = MakeLowFreqColorDiffDy();
  return kLut.data();
}

inline double Interpolate(const double *array, int size, double sx) {
  double ix = fabs(sx);
  assert(ix < 10000);
  int baseix = static_cast<int>(ix);
  double res;
  if (baseix >= size - 1) {
    res = array[size - 1];
  } else {
    double mix = ix - baseix;
    int nextix = baseix + 1;
    res = array[baseix] + mix * (array[nextix] - array[baseix]);
  }
  if (sx < 0) res = -res;
  return res;
}

inline double InterpolateClampNegative(const double *array,
                                       int size, double sx) {
  if (sx < 0) {
    sx = 0;
  }
  double ix = fabs(sx);
  int baseix = static_cast<int>(ix);
  double res;
  if (baseix >= size - 1) {
    res = array[size - 1];
  } else {
    double mix = ix - baseix;
    int nextix = baseix + 1;
    res = array[baseix] + mix * (array[nextix] - array[baseix]);
  }
  return res;
}

void RgbToXyb(double r, double g, double b,
              double *valx, double *valy, double *valz) {
  static const double a0 = 1.01611726948;
  static const double a1 = 0.982482243696;
  static const double a2 = 1.43571362627;
  static const double a3 = 0.896039849412;
  *valx = a0 * r - a1 * g;
  *valy = a2 * r + a3 * g;
  *valz = b;
}

static inline void XybToVals(double x, double y, double z,
                             double *valx, double *valy, double *valz) {
  static const double xmul = 0.758304045695;
  static const double ymul = 2.28148649801;
  static const double zmul = 1.87816926918;
  *valx = Interpolate(GetHighFreqColorDiffDx(), 21, x * xmul);
  *valy = Interpolate(GetHighFreqColorDiffDy(), 21, y * ymul);
  *valz = zmul * z;
}

// Rough psychovisual distance to gray for low frequency colors.
static void XybLowFreqToVals(double x, double y, double z,
                             double *valx, double *valy, double *valz) {
  static const double xmul = 6.64482198135;
  static const double ymul = 0.837846224276;
  static const double zmul = 7.34905756986;
  static const double y_to_z_mul = 0.0812519812628;
  z += y_to_z_mul * y;
  *valz = z * zmul;
  *valx = x * xmul;
  *valy = Interpolate(GetLowFreqColorDiffDy(), 21, y * ymul);
}

double RemoveRangeAroundZero(double v, double range) {
  if (v >= -range && v < range) {
    return 0;
  }
  if (v < 0) {
    return v + range;
  } else {
    return v - range;
  }
}

void XybDiffLowFreqSquaredAccumulate(double r0, double g0, double b0,
                                     double r1, double g1, double b1,
                                     double factor, double res[3]) {
  double valx0, valy0, valz0;
  double valx1, valy1, valz1;
  XybLowFreqToVals(r0, g0, b0, &valx0, &valy0, &valz0);
  if (r1 == 0.0 && g1 == 0.0 && b1 == 0.0) {
    PROFILER_ZONE("XybDiff r1=g1=b1=0");
    res[0] += factor * valx0 * valx0;
    res[1] += factor * valy0 * valy0;
    res[2] += factor * valz0 * valz0;
    return;
  }
  XybLowFreqToVals(r1, g1, b1, &valx1, &valy1, &valz1);
  // Approximate the distance of the colors by their respective distances
  // to gray.
  double valx = valx0 - valx1;
  double valy = valy0 - valy1;
  double valz = valz0 - valz1;
  res[0] += factor * valx * valx;
  res[1] += factor * valy * valy;
  res[2] += factor * valz * valz;
}

struct Complex {
 public:
  double real;
  double imag;
};

inline double abssq(const Complex& c) {
  return c.real * c.real + c.imag * c.imag;
}

static void TransposeBlock(Complex data[kBlockSize]) {
  for (int i = 0; i < kBlockEdge; i++) {
    for (int j = 0; j < i; j++) {
      std::swap(data[kBlockEdge * i + j], data[kBlockEdge * j + i]);
    }
  }
}

//  D. J. Bernstein's Fast Fourier Transform algorithm on 4 elements.
inline void FFT4(Complex* a) {
  double t1, t2, t3, t4, t5, t6, t7, t8;
  t5 = a[2].real;
  t1 = a[0].real - t5;
  t7 = a[3].real;
  t5 += a[0].real;
  t3 = a[1].real - t7;
  t7 += a[1].real;
  t8 = t5 + t7;
  a[0].real = t8;
  t5 -= t7;
  a[1].real = t5;
  t6 = a[2].imag;
  t2 = a[0].imag - t6;
  t6 += a[0].imag;
  t5 = a[3].imag;
  a[2].imag = t2 + t3;
  t2 -= t3;
  a[3].imag = t2;
  t4 = a[1].imag - t5;
  a[3].real = t1 + t4;
  t1 -= t4;
  a[2].real = t1;
  t5 += a[1].imag;
  a[0].imag = t6 + t5;
  t6 -= t5;
  a[1].imag = t6;
}

static const double kSqrtHalf = 0.70710678118654752440084436210484903;

//  D. J. Bernstein's Fast Fourier Transform algorithm on 8 elements.
void FFT8(Complex* a) {
  double t1, t2, t3, t4, t5, t6, t7, t8;

  t7 = a[4].imag;
  t4 = a[0].imag - t7;
  t7 += a[0].imag;
  a[0].imag = t7;

  t8 = a[6].real;
  t5 = a[2].real - t8;
  t8 += a[2].real;
  a[2].real = t8;

  t7 = a[6].imag;
  a[6].imag = t4 - t5;
  t4 += t5;
  a[4].imag = t4;

  t6 = a[2].imag - t7;
  t7 += a[2].imag;
  a[2].imag = t7;

  t8 = a[4].real;
  t3 = a[0].real - t8;
  t8 += a[0].real;
  a[0].real = t8;

  a[4].real = t3 - t6;
  t3 += t6;
  a[6].real = t3;

  t7 = a[5].real;
  t3 = a[1].real - t7;
  t7 += a[1].real;
  a[1].real = t7;

  t8 = a[7].imag;
  t6 = a[3].imag - t8;
  t8 += a[3].imag;
  a[3].imag = t8;
  t1 = t3 - t6;
  t3 += t6;

  t7 = a[5].imag;
  t4 = a[1].imag - t7;
  t7 += a[1].imag;
  a[1].imag = t7;

  t8 = a[7].real;
  t5 = a[3].real - t8;
  t8 += a[3].real;
  a[3].real = t8;

  t2 = t4 - t5;
  t4 += t5;

  t6 = t1 - t4;
  t8 = kSqrtHalf;
  t6 *= t8;
  a[5].real = a[4].real - t6;
  t1 += t4;
  t1 *= t8;
  a[5].imag = a[4].imag - t1;
  t6 += a[4].real;
  a[4].real = t6;
  t1 += a[4].imag;
  a[4].imag = t1;

  t5 = t2 - t3;
  t5 *= t8;
  a[7].imag = a[6].imag - t5;
  t2 += t3;
  t2 *= t8;
  a[7].real = a[6].real - t2;
  t2 += a[6].real;
  a[6].real = t2;
  t5 += a[6].imag;
  a[6].imag = t5;

  FFT4(a);

  // Reorder to the correct output order.
  // TODO: Modify the above computation so that this is not needed.
  Complex tmp = a[2];
  a[2] = a[3];
  a[3] = a[5];
  a[5] = a[7];
  a[7] = a[4];
  a[4] = a[1];
  a[1] = a[6];
  a[6] = tmp;
}

// Same as FFT8, but all inputs are real.
// TODO: Since this does not need to be in-place, maybe there is a
// faster FFT than this one, which is derived from DJB's in-place complex FFT.
void RealFFT8(const double* in, Complex* out) {
  double t1, t2, t3, t5, t6, t7, t8;
  t8 = in[6];
  t5 = in[2] - t8;
  t8 += in[2];
  out[2].real = t8;
  out[6].imag = -t5;
  out[4].imag = t5;
  t8 = in[4];
  t3 = in[0] - t8;
  t8 += in[0];
  out[0].real = t8;
  out[4].real = t3;
  out[6].real = t3;
  t7 = in[5];
  t3 = in[1] - t7;
  t7 += in[1];
  out[1].real = t7;
  t8 = in[7];
  t5 = in[3] - t8;
  t8 += in[3];
  out[3].real = t8;
  t2 = -t5;
  t6 = t3 - t5;
  t8 = kSqrtHalf;
  t6 *= t8;
  out[5].real = out[4].real - t6;
  t1 = t3 + t5;
  t1 *= t8;
  out[5].imag = out[4].imag - t1;
  t6 += out[4].real;
  out[4].real = t6;
  t1 += out[4].imag;
  out[4].imag = t1;
  t5 = t2 - t3;
  t5 *= t8;
  out[7].imag = out[6].imag - t5;
  t2 += t3;
  t2 *= t8;
  out[7].real = out[6].real - t2;
  t2 += out[6].real;
  out[6].real = t2;
  t5 += out[6].imag;
  out[6].imag = t5;
  t5 = out[2].real;
  t1 = out[0].real - t5;
  t7 = out[3].real;
  t5 += out[0].real;
  t3 = out[1].real - t7;
  t7 += out[1].real;
  t8 = t5 + t7;
  out[0].real = t8;
  t5 -= t7;
  out[1].real = t5;
  out[2].imag = t3;
  out[3].imag = -t3;
  out[3].real = t1;
  out[2].real = t1;
  out[0].imag = 0;
  out[1].imag = 0;

  // Reorder to the correct output order.
  // TODO: Modify the above computation so that this is not needed.
  Complex tmp = out[2];
  out[2] = out[3];
  out[3] = out[5];
  out[5] = out[7];
  out[7] = out[4];
  out[4] = out[1];
  out[1] = out[6];
  out[6] = tmp;
}

// Fills in block[kBlockEdgeHalf..(kBlockHalf+kBlockEdgeHalf)], and leaves the
// rest unmodified.
void ButteraugliFFTSquared(double block[kBlockSize]) {
  double global_mul = 0.000064;
  Complex block_c[kBlockSize];
  assert(kBlockEdge == 8);
  for (int y = 0; y < kBlockEdge; ++y) {
    RealFFT8(block + y * kBlockEdge, block_c + y * kBlockEdge);
  }
  TransposeBlock(block_c);
  double r0[kBlockEdge];
  double r1[kBlockEdge];
  for (int x = 0; x < kBlockEdge; ++x) {
    r0[x] = block_c[x].real;
    r1[x] = block_c[kBlockHalf + x].real;
  }
  RealFFT8(r0, block_c);
  RealFFT8(r1, block_c + kBlockHalf);
  for (int y = 1; y < kBlockEdgeHalf; ++y) {
    FFT8(block_c + y * kBlockEdge);
  }
  for (int i = kBlockEdgeHalf; i < kBlockHalf + kBlockEdgeHalf + 1; ++i) {
    block[i] = abssq(block_c[i]);
    block[i] *= global_mul;
  }
}

// Computes 8x8 FFT of each channel of xyb0 and xyb1 and adds the total squared
// 3-dimensional xybdiff of the two blocks to diff_xyb_{dc,ac} and the average
// diff on the edges to diff_xyb_edge_dc.
void ButteraugliBlockDiff(double xyb0[3 * kBlockSize],
                          double xyb1[3 * kBlockSize],
                          double diff_xyb_dc[3],
                          double diff_xyb_ac[3],
                          double diff_xyb_edge_dc[3]) {
  PROFILER_FUNC;
  const double *csf8x8 = GetContrastSensitivityMatrix();

  double avgdiff_xyb[3] = {0.0};
  double avgdiff_edge[3][4] = { {0.0} };
  for (int i = 0; i < 3 * kBlockSize; ++i) {
    const double diff_xyb = xyb0[i] - xyb1[i];
    const int c = i / kBlockSize;
    avgdiff_xyb[c] += diff_xyb / kBlockSize;
    const int k = i % kBlockSize;
    const int kx = k % kBlockEdge;
    const int ky = k / kBlockEdge;
    const int h_edge_idx = ky == 0 ? 1 : ky == 7 ? 3 : -1;
    const int v_edge_idx = kx == 0 ? 0 : kx == 7 ? 2 : -1;
    if (h_edge_idx >= 0) {
      avgdiff_edge[c][h_edge_idx] += diff_xyb / kBlockEdge;
    }
    if (v_edge_idx >= 0) {
      avgdiff_edge[c][v_edge_idx] += diff_xyb / kBlockEdge;
    }
  }
  XybDiffLowFreqSquaredAccumulate(avgdiff_xyb[0],
                                  avgdiff_xyb[1],
                                  avgdiff_xyb[2],
                                  0, 0, 0, csf8x8[0],
                                  diff_xyb_dc);
  for (int i = 0; i < 4; ++i) {
    XybDiffLowFreqSquaredAccumulate(avgdiff_edge[0][i],
                                    avgdiff_edge[1][i],
                                    avgdiff_edge[2][i],
                                    0, 0, 0, csf8x8[0],
                                    diff_xyb_edge_dc);
  }

  double* xyb_avg = xyb0;
  double* xyb_halfdiff = xyb1;
  for(int i = 0; i < 3 * kBlockSize; ++i) {
    double avg = (xyb0[i] + xyb1[i])/2;
    double halfdiff = (xyb0[i] - xyb1[i])/2;
    xyb_avg[i] = avg;
    xyb_halfdiff[i] = halfdiff;
  }
  double *y_avg = &xyb_avg[kBlockSize];
  double *x_halfdiff_squared = &xyb_halfdiff[0];
  double *y_halfdiff = &xyb_halfdiff[kBlockSize];
  double *z_halfdiff_squared = &xyb_halfdiff[2 * kBlockSize];
  ButteraugliFFTSquared(y_avg);
  ButteraugliFFTSquared(x_halfdiff_squared);
  ButteraugliFFTSquared(y_halfdiff);
  ButteraugliFFTSquared(z_halfdiff_squared);

  static const double xmul = 64.8;
  static const double ymul = 1.753123908348329;
  static const double ymul2 = 1.51983458269;
  static const double zmul = 2.4;

  for (size_t i = kBlockEdgeHalf; i < kBlockHalf + kBlockEdgeHalf + 1; ++i) {
    double d = csf8x8[i];
    diff_xyb_ac[0] += d * xmul * x_halfdiff_squared[i];
    diff_xyb_ac[2] += d * zmul * z_halfdiff_squared[i];

    y_avg[i] = sqrt(y_avg[i]);
    y_halfdiff[i] = sqrt(y_halfdiff[i]);
    double y0 = y_avg[i] - y_halfdiff[i];
    double y1 = y_avg[i] + y_halfdiff[i];
    // Remove the impact of small absolute values.
    // This improves the behavior with flat noise.
    static const double ylimit = 0.04;
    y0 = RemoveRangeAroundZero(y0, ylimit);
    y1 = RemoveRangeAroundZero(y1, ylimit);
    if (y0 != y1) {
      double valy0 = Interpolate(GetHighFreqColorDiffDy(), 21, y0 * ymul2);
      double valy1 = Interpolate(GetHighFreqColorDiffDy(), 21, y1 * ymul2);
      double valy = ymul * (valy0 - valy1);
      diff_xyb_ac[1] += d * valy * valy;
    }
  }
}

// Low frequency edge detectors.
// Two edge detectors are applied in each corner of the 8x8 square.
// The squared 3-dimensional error vector is added to diff_xyb.
void Butteraugli8x8CornerEdgeDetectorDiff(
    const size_t pos_x,
    const size_t pos_y,
    const size_t xsize,
    const size_t ysize,
    const std::vector<std::vector<float> > &blurred0,
    const std::vector<std::vector<float> > &blurred1,
    double diff_xyb[3]) {
  PROFILER_FUNC;
  int local_count = 0;
  double local_xyb[3] = { 0 };
  static const double w = 0.711100840192;
  for (int k = 0; k < 4; ++k) {
    size_t step = 3;
    size_t offset[4][2] = { { 0, 0 }, { 0, 7 }, { 7, 0 }, { 7, 7 } };
    size_t x = pos_x + offset[k][0];
    size_t y = pos_y + offset[k][1];
    if (x >= step && x + step < xsize) {
      size_t ix = y * xsize + (x - step);
      size_t ix2 = ix + 2 * step;
      XybDiffLowFreqSquaredAccumulate(
          w * (blurred0[0][ix] - blurred0[0][ix2]),
          w * (blurred0[1][ix] - blurred0[1][ix2]),
          w * (blurred0[2][ix] - blurred0[2][ix2]),
          w * (blurred1[0][ix] - blurred1[0][ix2]),
          w * (blurred1[1][ix] - blurred1[1][ix2]),
          w * (blurred1[2][ix] - blurred1[2][ix2]),
          1.0, local_xyb);
      ++local_count;
    }
    if (y >= step && y + step < ysize) {
      size_t ix = (y - step) * xsize + x;
      size_t ix2 = ix + 2 * step * xsize;
      XybDiffLowFreqSquaredAccumulate(
          w * (blurred0[0][ix] - blurred0[0][ix2]),
          w * (blurred0[1][ix] - blurred0[1][ix2]),
          w * (blurred0[2][ix] - blurred0[2][ix2]),
          w * (blurred1[0][ix] - blurred1[0][ix2]),
          w * (blurred1[1][ix] - blurred1[1][ix2]),
          w * (blurred1[2][ix] - blurred1[2][ix2]),
          1.0, local_xyb);
      ++local_count;
    }
  }
  static const double weight = 0.01617112696;
  const double mul = weight * 8.0 / local_count;
  for (int i = 0; i < 3; ++i) {
    diff_xyb[i] += mul * local_xyb[i];
  }
}

// https://en.wikipedia.org/wiki/Photopsin absordance modeling.
const double *GetOpsinAbsorbance() {
  static const double kMix[12] = {
    0.348036746003,
    0.577814843137,
    0.0544556093735,
    0.774145581713,
    0.26922717275,
    0.767247733938,
    0.0366922708552,
    0.920130265014,
    0.0882062883536,
    0.158581714673,
    0.712857943858,
    10.6524069248,
  };
  return &kMix[0];
}

// mix是一个[4x4]矩阵，与in[,,,1]进行叉乘
void OpsinAbsorbance(const double in[3], double out[3]) {
  const double *mix = GetOpsinAbsorbance();
  out[0] = mix[0] * in[0] + mix[1] * in[1] + mix[2] * in[2] + mix[3];
  out[1] = mix[4] * in[0] + mix[5] * in[1] + mix[6] * in[2] + mix[7];
  out[2] = mix[8] * in[0] + mix[9] * in[1] + mix[10] * in[2] + mix[11];
}

double GammaMinArg() {
  double in[3] = { 0.0, 0.0, 0.0 };
  double out[3];
  OpsinAbsorbance(in, out);
  return std::min(out[0], std::min(out[1], out[2]));
}

double GammaMaxArg() {
  double in[3] = { 255.0, 255.0, 255.0 };
  double out[3];
  OpsinAbsorbance(in, out);
  return std::max(out[0], std::max(out[1], out[2]));
}

ButteraugliComparator::ButteraugliComparator(
    size_t xsize, size_t ysize, int step)
    : xsize_(xsize),
      ysize_(ysize),
      num_pixels_(xsize * ysize),
      step_(step),
      res_xsize_((xsize + step - 1) / step),
      res_ysize_((ysize + step - 1) / step) {
  assert(step <= 4);
}

void MaskHighIntensityChange(
    size_t xsize, size_t ysize,
    const std::vector<std::vector<float> > &c0,
    const std::vector<std::vector<float> > &c1,
    std::vector<std::vector<float> > &xyb0,
    std::vector<std::vector<float> > &xyb1) {
  PROFILER_FUNC;
  for (size_t y = 0; y < ysize; ++y) {
    for (size_t x = 0; x < xsize; ++x) {
      size_t ix = y * xsize + x;
      const double ave[3] = {
        (c0[0][ix] + c1[0][ix]) * 0.5,
        (c0[1][ix] + c1[1][ix]) * 0.5,
        (c0[2][ix] + c1[2][ix]) * 0.5,
      };
      double sqr_max_diff = -1;
      {
        int offset[4] =
            { -1, 1, -static_cast<int>(xsize), static_cast<int>(xsize) };
        int border[4] =
            { x == 0, x + 1 == xsize, y == 0, y + 1 == ysize };
        for (int dir = 0; dir < 4; ++dir) {
          if (border[dir]) {
            continue;
          }
          const int ix2 = ix + offset[dir];
          double diff = 0.5 * (c0[1][ix2] + c1[1][ix2]) - ave[1];
          diff *= diff;
          if (sqr_max_diff < diff) {
            sqr_max_diff = diff;
          }
        }
      }
      static const double kReductionX = 275.19165240059317;
      static const double kReductionY = 18599.41286306991;
      static const double kReductionZ = 410.8995306951065;
      static const double kChromaBalance = 106.95800948271017;
      double chroma_scale = kChromaBalance / (ave[1] + kChromaBalance);

      const double mix[3] = {
        chroma_scale * kReductionX / (sqr_max_diff + kReductionX),
        kReductionY / (sqr_max_diff + kReductionY),
        chroma_scale * kReductionZ / (sqr_max_diff + kReductionZ),
      };
      // Interpolate lineraly between the average color and the actual
      // color -- to reduce the importance of this pixel.
      for (int i = 0; i < 3; ++i) {
        xyb0[i][ix] = static_cast<float>(mix[i] * c0[i][ix] + (1 - mix[i]) * ave[i]);
        xyb1[i][ix] = static_cast<float>(mix[i] * c1[i][ix] + (1 - mix[i]) * ave[i]);
      }
    }
  }
}

double SimpleGamma(double v) {
  static const double kGamma = 0.387494322593;
  static const double limit = 43.01745241042018;
  double bright = v - limit;
  if (bright >= 0) {
    static const double mul = 0.0383723643799;
    v -= bright * mul;
  }
  static const double limit2 = 94.68634353321337;
  double bright2 = v - limit2;
  if (bright2 >= 0) {
    static const double mul = 0.22885405968;
    v -= bright2 * mul;
  }
  static const double offset = 0.156775786057;
  static const double scale = 8.898059160493739;
  double retval = scale * (offset + pow(v, kGamma));
  return retval;
}

// Polynomial evaluation via Clenshaw's scheme (similar to Horner's).
// Template enables compile-time unrolling of the recursion, but must reside
// outside of a class due to the specialization.
template <int INDEX>
static inline void ClenshawRecursion(const double x, const double *coefficients,
                                     double *b1, double *b2) {
  const double x_b1 = x * (*b1);
  const double t = (x_b1 + x_b1) - (*b2) + coefficients[INDEX];
  *b2 = *b1;
  *b1 = t;

  ClenshawRecursion<INDEX - 1>(x, coefficients, b1, b2);
}

// Base case
template <>
inline void ClenshawRecursion<0>(const double x, const double *coefficients,
                                 double *b1, double *b2) {
  const double x_b1 = x * (*b1);
  // The final iteration differs - no 2 * x_b1 here.
  *b1 = x_b1 - (*b2) + coefficients[0];
}

void ClenshawRecursion_fun(const double x, const double *coefficients,
	double *b1, double *b2, int n)
{
	if (n == 0) {
		const double x_b1 = x * (*b1);
		// The final iteration differs - no 2 * x_b1 here.
		*b1 = x_b1 - (*b2) + coefficients[0];
		return;
	}

	const double x_b1 = x * (*b1);
	const double t = (x_b1 + x_b1) - (*b2) + coefficients[n];
	*b2 = *b1;
	*b1 = t;

	ClenshawRecursion_fun(x, coefficients, b1, b2, n - 1);
}

// Rational polynomial := dividing two polynomial evaluations. These are easier
// to find than minimax polynomials.
struct RationalPolynomial {
  template <int N>
  static double EvaluatePolynomial(const double x,
                                   const double (&coefficients)[N]) {
    double b1 = 0.0;
    double b2 = 0.0;

    ClenshawRecursion<N - 1>(x, coefficients, &b1, &b2);

    return b1;
  }

#ifdef ENABLE_OPENCL_CHECK
  static double EvaluatePolynomialNonRecursion(const double x, const double *coefficients, int n) {
	double b1 = 0.0;
	double b2 = 0.0;

	for (int i = n - 1; i >= 0; i--)
	{
		if (i == 0) {
			const double x_b1 = x * b1;
			b1 = x_b1 - b2 + coefficients[0];
			break;
		}
		const double x_b1 = x * b1;
		const double t = (x_b1 + x_b1) - b2 + coefficients[i];
		b2 = b1;
		b1 = t;
	}

	return b1;
  }
#endif // ENABLE_OPENCL_CHECK

  // Evaluates the polynomial at x (in [min_value, max_value]).
  inline double operator()(const float x) const {
    // First normalize to [0, 1].
    const double x01 = (x - min_value) / (max_value - min_value);
    // And then to [-1, 1] domain of Chebyshev polynomials.
    const double xc = 2.0 * x01 - 1.0;

    const double yp = EvaluatePolynomial(xc, p);
    const double yq = EvaluatePolynomial(xc, q);
    if (yq == 0.0) return 0.0;
    return static_cast<float>(yp / yq);
  }

  // Domain of the polynomials; they are undefined elsewhere.
  double min_value;
  double max_value;

  // Coefficients of T_n (Chebyshev polynomials of the first kind).
  // Degree 5/5 is a compromise between accuracy (0.1%) and numerical stability.
  double p[5 + 1];
  double q[5 + 1];
};

static inline float GammaPolynomial(float value) {
  // Generated by gamma_polynomial.m from equispaced x/gamma(x) samples.
  static const RationalPolynomial r = {
  0.770000000000000, 274.579999999999984,
  {
    881.979476556478289, 1496.058452015812463, 908.662212739659481,
    373.566100223287378, 85.840860336314364, 6.683258861509244,
  },
  {
    12.262350348616792, 20.557285797683576, 12.161463238367844,
    4.711532733641639, 0.899112889751053, 0.035662329617191,
  }};
  return static_cast<float>(r(value));
}

#ifdef ENABLE_OPENCL_CHECK
static double GammaNonRecursion(double v) {
	double min_value = 0.770000000000000;
	double max_value = 274.579999999999984;

	double p[5 + 1] = {
		881.979476556478289, 1496.058452015812463, 908.662212739659481,
		373.566100223287378, 85.840860336314364, 6.683258861509244,
	};
	double q[5 + 1] = {
		12.262350348616792, 20.557285797683576, 12.161463238367844,
		4.711532733641639, 0.899112889751053, 0.035662329617191,
	};

	// First normalize to [0, 1].
	const double x01 = (v - min_value) / (max_value - min_value);
	// And then to [-1, 1] domain of Chebyshev polynomials.
	const double xc = 2.0 * x01 - 1.0;

	const double yp = RationalPolynomial::EvaluatePolynomialNonRecursion(xc, p, 6);
	const double yq = RationalPolynomial::EvaluatePolynomialNonRecursion(xc, q, 6);
	if (yq == 0.0) return 0.0;
	return static_cast<float>(yp / yq);
}
#endif // ENABLE_OPENCL_CHECK

static inline double Gamma(double v) {
  // return SimpleGamma(v);
  return GammaPolynomial(static_cast<float>(v));
}

void OpsinDynamicsImage(size_t xsize, size_t ysize,
                        std::vector<std::vector<float> > &rgb) {

    if (g_useOpenCL && xsize > 100 && ysize > 100)
    {
        float * r = rgb[0].data();
        float * g = rgb[1].data();
        float * b = rgb[2].data();

        clOpsinDynamicsImage(xsize, ysize, r, g, b);
        return;
    }

  PROFILER_FUNC;
  std::vector<std::vector<float> > blurred = rgb;
  static const double kSigma = 1.1;
  for (int i = 0; i < 3; ++i) {
    Blur(xsize, ysize, blurred[i].data(), kSigma, 0.0);
  }
  for (size_t i = 0; i < rgb[0].size(); ++i) {
    double sensitivity[3];
    {
      // Calculate sensitivity[3] based on the smoothed image gamma derivative.
      double pre_rgb[3] = { blurred[0][i], blurred[1][i], blurred[2][i] };
      double pre_mixed[3];
      OpsinAbsorbance(pre_rgb, pre_mixed);
      sensitivity[0] = Gamma(pre_mixed[0]) / pre_mixed[0];
      sensitivity[1] = Gamma(pre_mixed[1]) / pre_mixed[1];
      sensitivity[2] = Gamma(pre_mixed[2]) / pre_mixed[2];

#ifdef ENABLE_OPENCL_CHECK
	  double sensitivity_new[3];
	  sensitivity_new[0] = GammaNonRecursion(pre_mixed[0]) / pre_mixed[0];
	  assert(fabs(sensitivity[0] - sensitivity_new[0]) < 0.01);
	  sensitivity_new[1] = GammaNonRecursion(pre_mixed[1]) / pre_mixed[1];
	  assert(fabs(sensitivity[1] - sensitivity_new[1]) < 0.01);
	  sensitivity_new[2] = GammaNonRecursion(pre_mixed[2]) / pre_mixed[2];
	  assert(fabs(sensitivity[2] - sensitivity_new[2]) < 0.01);
#endif // ENABLE_OPENCL_CHECK
    }
    double cur_rgb[3] = { rgb[0][i],  rgb[1][i],  rgb[2][i] };
    double cur_mixed[3];
    OpsinAbsorbance(cur_rgb, cur_mixed);
    cur_mixed[0] *= sensitivity[0];
    cur_mixed[1] *= sensitivity[1];
    cur_mixed[2] *= sensitivity[2];
    double x, y, z;
    RgbToXyb(cur_mixed[0], cur_mixed[1], cur_mixed[2], &x, &y, &z);
    rgb[0][i] = static_cast<float>(x);
    rgb[1][i] = static_cast<float>(y);
    rgb[2][i] = static_cast<float>(z);
  }
}

static void ScaleImage(double scale, std::vector<float> *result) {
  PROFILER_FUNC;
  for (size_t i = 0; i < result->size(); ++i) {
    (*result)[i] *= static_cast<float>(scale);
  }
}

// Making a cluster of local errors to be more impactful than
// just a single error.
void CalculateDiffmap(const size_t xsize, const size_t ysize,
                      const size_t step,
                      std::vector<float>* diffmap) {
  PROFILER_FUNC;
  // Shift the diffmap more correctly above the pixels, from 2.5 pixels to 0.5
  // pixels distance over the original image. The border of 2 pixels on top and
  // left side and 3 pixels on right and bottom side are zeroed, but these
  // values have no meaning, they only exist to keep the result map the same
  // size as the input images.
  int s2 = (8 - step) / 2;
  {
    // Upsample and take square root.
    std::vector<float> diffmap_out(xsize * ysize);
    const size_t res_xsize = (xsize + step - 1) / step;
    for (size_t res_y = 0; res_y + 8 - step < ysize; res_y += step) {
      for (size_t res_x = 0; res_x + 8 - step < xsize; res_x += step) {
        size_t res_ix = (res_y * res_xsize + res_x) / step;
        float orig_val = (*diffmap)[res_ix];
        constexpr float kInitialSlope = 100;
        // TODO(b/29974893): Until that is fixed do not call sqrt on very small
        // numbers.
        double val = orig_val < (1.0 / (kInitialSlope * kInitialSlope))
                                ? kInitialSlope * orig_val
                                : std::sqrt(orig_val);
        for (size_t off_y = 0; off_y < step; ++off_y) {
          for (size_t off_x = 0; off_x < step; ++off_x) {
            diffmap_out[(res_y + off_y + s2) * xsize +
                        res_x + off_x + s2] = val;
          }
        }
      }
    }
    *diffmap = diffmap_out;
  }
  {
    static const double kSigma = 8.8510880283;
    static const double mul1 = 24.8235314874;
    static const double scale = 1.0 / (1.0 + mul1);
    const int s = 8 - step;
    std::vector<float> blurred((xsize - s) * (ysize - s));
    for (size_t y = 0; y < ysize - s; ++y) {
      for (size_t x = 0; x < xsize - s; ++x) {
        blurred[y * (xsize - s) + x] = (*diffmap)[(y + s2) * xsize + x + s2];
      }
    }
    static const double border_ratio = 0.03027655136;
    Blur(xsize - s, ysize - s, blurred.data(), kSigma, border_ratio);
    for (size_t y = 0; y < ysize - s; ++y) {
      for (size_t x = 0; x < xsize - s; ++x) {
        (*diffmap)[(y + s2) * xsize + x + s2]
            += static_cast<float>(mul1) * blurred[y * (xsize - s) + x];
      }
    }
    ScaleImage(scale, diffmap);
  }
}

void ButteraugliComparator::DiffmapOpsinDynamicsImage(
    const std::vector<std::vector<float>> &xyb0_arg,
    std::vector<std::vector<float>> &xyb1,
    std::vector<float> &result) {

	if (g_useOpenCL && xsize_ > 100 && ysize_ > 100)
	{
		result.resize(xsize_ * ysize_);
		clDiffmapOpsinDynamicsImage(xyb0_arg[0].data(), xyb0_arg[1].data(), xyb0_arg[2].data(),
			xyb1[0].data(), xyb1[1].data(), xyb1[2].data(), xsize_, ysize_, step_, result.data());
	}


  if (xsize_ < 8 || ysize_ < 8) return;
  auto xyb0 = xyb0_arg;
  {
    auto xyb1_c = xyb1;
    MaskHighIntensityChange(xsize_, ysize_, xyb0_arg, xyb1_c, xyb0, xyb1);
  }
  assert(8 <= xsize_);
  for (int i = 0; i < 3; i++) {
    assert(xyb0[i].size() == num_pixels_);
    assert(xyb1[i].size() == num_pixels_);
  }
  std::vector<float> edge_detector_map(3 * res_xsize_ * res_ysize_);
  EdgeDetectorMap(xyb0, xyb1, &edge_detector_map);
  std::vector<float> block_diff_dc(3 * res_xsize_ * res_ysize_);
  std::vector<float> block_diff_ac(3 * res_xsize_ * res_ysize_);
  BlockDiffMap(xyb0, xyb1, &block_diff_dc, &block_diff_ac);
  EdgeDetectorLowFreq(xyb0, xyb1, &block_diff_ac);
  {
    std::vector<std::vector<float> > mask_xyb(3);
    std::vector<std::vector<float> > mask_xyb_dc(3);
    Mask(xyb0, xyb1, xsize_, ysize_, &mask_xyb, &mask_xyb_dc);
    CombineChannels(mask_xyb, mask_xyb_dc, block_diff_dc, block_diff_ac,
                    edge_detector_map, &result);
  }
  CalculateDiffmap(xsize_, ysize_, step_, &result);
}

void ButteraugliComparator::BlockDiffMap(
    const std::vector<std::vector<float> > &xyb0,
    const std::vector<std::vector<float> > &xyb1,
    std::vector<float>* block_diff_dc,
    std::vector<float>* block_diff_ac) {
  PROFILER_FUNC;
  for (size_t res_y = 0; res_y + (kBlockEdge - step_ - 1) < ysize_;
       res_y += step_) {
    for (size_t res_x = 0; res_x + (kBlockEdge - step_ - 1) < xsize_;
         res_x += step_) {
      size_t res_ix = (res_y * res_xsize_ + res_x) / step_;
      size_t offset = (std::min(res_y, ysize_ - 8) * xsize_ +
                       std::min(res_x, xsize_ - 8));
      double block0[3 * kBlockEdge * kBlockEdge];
      double block1[3 * kBlockEdge * kBlockEdge];
      for (int i = 0; i < 3; ++i) {
        double *m0 = &block0[i * kBlockEdge * kBlockEdge];
        double *m1 = &block1[i * kBlockEdge * kBlockEdge];
        for (size_t y = 0; y < kBlockEdge; y++) {
          for (size_t x = 0; x < kBlockEdge; x++) {
            m0[kBlockEdge * y + x] = xyb0[i][offset + y * xsize_ + x];
            m1[kBlockEdge * y + x] = xyb1[i][offset + y * xsize_ + x];
          }
        }
      }
      double diff_xyb_dc[3] = { 0.0 };
      double diff_xyb_ac[3] = { 0.0 };
      double diff_xyb_edge_dc[3] = { 0.0 };
      ButteraugliBlockDiff(block0, block1,
                           diff_xyb_dc, diff_xyb_ac, diff_xyb_edge_dc);
      for (int i = 0; i < 3; ++i) {
        (*block_diff_dc)[3 * res_ix + i] = static_cast<float>(diff_xyb_dc[i]);
        (*block_diff_ac)[3 * res_ix + i] = static_cast<float>(diff_xyb_ac[i]);
      }
    }
  }
}

void ButteraugliComparator::EdgeDetectorMap(
    const std::vector<std::vector<float> > &xyb0,
    const std::vector<std::vector<float> > &xyb1,
    std::vector<float>* edge_detector_map) {
  PROFILER_FUNC;
  static const double kSigma[3] = {
    1.5,
    0.586,
    0.4,
  };
  std::vector<std::vector<float> > blurred0(xyb0);
  std::vector<std::vector<float> > blurred1(xyb1);
  for (int i = 0; i < 3; i++) {
    Blur(xsize_, ysize_, blurred0[i].data(), kSigma[i], 0.0);
    Blur(xsize_, ysize_, blurred1[i].data(), kSigma[i], 0.0);
  }
  for (size_t res_y = 0; res_y + (8 - step_) < ysize_; res_y += step_) {
    for (size_t res_x = 0; res_x + (8 - step_) < xsize_; res_x += step_) {
      size_t res_ix = (res_y * res_xsize_ + res_x) / step_;
      double diff_xyb[3] = { 0.0 };
      Butteraugli8x8CornerEdgeDetectorDiff(std::min(res_x, xsize_ - 8),
                                           std::min(res_y, ysize_ - 8),
                                           xsize_, ysize_,
                                           blurred0, blurred1,
                                           diff_xyb);
      for (int i = 0; i < 3; ++i) {
        (*edge_detector_map)[3 * res_ix + i] = static_cast<float>(diff_xyb[i]);
      }
    }
  }
}

void ButteraugliComparator::EdgeDetectorLowFreq(
    const std::vector<std::vector<float> > &xyb0,
    const std::vector<std::vector<float> > &xyb1,
    std::vector<float>* block_diff_ac) {
  PROFILER_FUNC;
  static const double kSigma = 14;
  static const double kMul = 10;
  std::vector<std::vector<float> > blurred0(xyb0);
  std::vector<std::vector<float> > blurred1(xyb1);
  for (int i = 0; i < 3; i++) {
    Blur(xsize_, ysize_, blurred0[i].data(), kSigma, 0.0);
    Blur(xsize_, ysize_, blurred1[i].data(), kSigma, 0.0);
  }
  const int step = 8;
  for (size_t y = 0; y + step < ysize_; y += step_) {
    int resy = y / step_;
    int resx = step / step_;
    for (size_t x = 0; x + step < xsize_; x += step_, resx++) {
      const int ix = y * xsize_ + x;
      const int res_ix = resy * res_xsize_ + resx;
      double diff[4][3];
      for (int i = 0; i < 3; ++i) {
        int ix2 = ix + 8;
        diff[0][i] =
            ((blurred1[i][ix] - blurred0[i][ix]) +
             (blurred0[i][ix2] - blurred1[i][ix2]));
        ix2 = ix + 8 * xsize_;
        diff[1][i] =
            ((blurred1[i][ix] - blurred0[i][ix]) +
             (blurred0[i][ix2] - blurred1[i][ix2]));
        ix2 = ix + 6 * xsize_ + 6;
        diff[2][i] =
            ((blurred1[i][ix] - blurred0[i][ix]) +
             (blurred0[i][ix2] - blurred1[i][ix2]));
        ix2 = ix + 6 * xsize_ - 6;
        diff[3][i] = x < step ? 0 :
            ((blurred1[i][ix] - blurred0[i][ix]) +
             (blurred0[i][ix2] - blurred1[i][ix2]));
      }
      double max_diff_xyb[3] = { 0 };
      for (int k = 0; k < 4; ++k) {
        double diff_xyb[3] = { 0 };
        XybDiffLowFreqSquaredAccumulate(diff[k][0], diff[k][1], diff[k][2],
                                        0, 0, 0, 1.0,
                                        diff_xyb);
        for (int i = 0; i < 3; ++i) {
          max_diff_xyb[i] = std::max<double>(max_diff_xyb[i], diff_xyb[i]);
        }
      }
      for (int i = 0; i < 3; ++i) {
        (*block_diff_ac)[3 * res_ix + i] += static_cast<float>(kMul * max_diff_xyb[i]);
      }
    }
  }
}

void ButteraugliComparator::CombineChannels(
    const std::vector<std::vector<float> >& mask_xyb,
    const std::vector<std::vector<float> >& mask_xyb_dc,
    const std::vector<float>& block_diff_dc,
    const std::vector<float>& block_diff_ac,
    const std::vector<float>& edge_detector_map,
    std::vector<float>* result) {
  PROFILER_FUNC;
  result->resize(res_xsize_ * res_ysize_);
  for (size_t res_y = 0; res_y + (8 - step_) < ysize_; res_y += step_) {
    for (size_t res_x = 0; res_x + (8 - step_) < xsize_; res_x += step_) {
      size_t res_ix = (res_y * res_xsize_ + res_x) / step_;
      double mask[3];
      double dc_mask[3];
      for (int i = 0; i < 3; ++i) {
        mask[i] = mask_xyb[i][(res_y + 3) * xsize_ + (res_x + 3)];
        dc_mask[i] = mask_xyb_dc[i][(res_y + 3) * xsize_ + (res_x + 3)];
      }
      (*result)[res_ix] = static_cast<float>(
           DotProduct(&block_diff_dc[3 * res_ix], dc_mask) +
           DotProduct(&block_diff_ac[3 * res_ix], mask) +
           DotProduct(&edge_detector_map[3 * res_ix], mask));
    }
  }
}

double ButteraugliScoreFromDiffmap(const std::vector<float>& diffmap) {
  PROFILER_FUNC;
  float retval = 0.0f;
  for (size_t ix = 0; ix < diffmap.size(); ++ix) {
    retval = std::max(retval, diffmap[ix]);
  }
  return retval;
}

static std::array<double, 512> MakeMask(
    double extmul, double extoff,
    double mul, double offset,
    double scaler) {
  std::array<double, 512> lut;
  for (size_t i = 0; i < lut.size(); ++i) {
    const double c = mul / ((0.01 * scaler * i) + offset);
    lut[i] = 1.0 + extmul * (c + extoff);
    assert(lut[i] >= 0.0);
    lut[i] *= lut[i];
  }
  return lut;
}

double MaskX(double delta) {
  PROFILER_FUNC;
  static const double extmul = 0.975741017749;
  static const double extoff = -4.25328244168;
  static const double offset = 0.454909521427;
  static const double scaler = 0.0738288224836;
  static const double mul = 20.8029176447;
  static const std::array<double, 512> lut =
                MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

double MaskY(double delta) {
  PROFILER_FUNC;
  static const double extmul = 0.373995618954;
  static const double extoff = 1.5307267433;
  static const double offset = 0.911952641929;
  static const double scaler = 1.1731667845;
  static const double mul = 16.2447033988;
  static const std::array<double, 512> lut =
      MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

double MaskB(double delta) {
  PROFILER_FUNC;
  static const double extmul = 0.61582234137;
  static const double extoff = -4.25376118646;
  static const double offset = 1.05105070921;
  static const double scaler = 0.47434643535;
  static const double mul = 31.1444967089;
  static const std::array<double, 512> lut =
      MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

double MaskDcX(double delta) {
  PROFILER_FUNC;
  static const double extmul = 1.79116943438;
  static const double extoff = -3.86797479189;
  static const double offset = 0.670960225853;
  static const double scaler = 0.486575865525;
  static const double mul = 20.4563479139;
  static const std::array<double, 512> lut =
      MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

double MaskDcY(double delta) {
  PROFILER_FUNC;
  static const double extmul = 0.212223514236;
  static const double extoff = -3.65647120524;
  static const double offset = 1.73396799447;
  static const double scaler = 0.170392660501;
  static const double mul = 21.6566724788;
  static const std::array<double, 512> lut =
      MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

double MaskDcB(double delta) {
  PROFILER_FUNC;
  static const double extmul = 0.349376011816;
  static const double extoff = -0.894711072781;
  static const double offset = 0.901647926679;
  static const double scaler = 0.380086095024;
  static const double mul = 18.0373825149;
  static const std::array<double, 512> lut =
      MakeMask(extmul, extoff, mul, offset, scaler);
  return InterpolateClampNegative(lut.data(), lut.size(), delta);
}

void MinSquareVal(size_t square_size, size_t offset,
				  size_t xsize, size_t ysize,
                  float *values) {
  // offset is not negative and smaller than square_size.
  assert(offset < square_size);
  std::vector<float> tmp(xsize * ysize);

  for (size_t y = 0; y < ysize; ++y) {
    const size_t minh = offset > y ? 0 : y - offset;
    const size_t maxh = std::min<size_t>(ysize, y + square_size - offset);

    float *pTmpPoint = &tmp[y * xsize];
    float *pValuePoint = &values[minh * xsize];

    for (size_t x = 0; x < xsize; ++x) {
        float *pValues = pValuePoint++;
        float min = *pValues;

        for (size_t j = minh + 1; j < maxh; ++j) {
            pValues += xsize;
            if (*pValues < min) min = *pValues;
        }
        *pTmpPoint++ = min;
    }
  }
  for (size_t x = 0; x < xsize; ++x) {
    const size_t minw = offset > x ? 0 : x - offset;
    const size_t maxw = std::min<size_t>(xsize, x + square_size - offset);

    float *pValuePoint = &values[x];
    float *pTmpPoint = &tmp[minw];

    for (size_t y = 0; y < ysize; ++y) {
        float * pTmp = pTmpPoint; pTmpPoint += xsize;
        float min = *pTmp;

        for (size_t j = minw + 1; j < maxw; ++j) {
            pTmp++;
            if (*pTmp < min) min = *pTmp;
        }
        *pValuePoint = min; pValuePoint += xsize;
    }
  }
}

// ===== Functions used by Mask only =====
void Average5x5(int xsize, int ysize, std::vector<float>* diffs) {
  PROFILER_FUNC;
  if (xsize < 4 || ysize < 4) {
    // TODO: Make this work for small dimensions as well.
    return;
  }
  static const float w = 0.679144890667f;
  static const float scale = 1.0f / (5.0f + 4 * w);
  std::vector<float> result = *diffs;
  std::vector<float> tmp0 = *diffs;
  std::vector<float> tmp1 = *diffs;
  ScaleImage(w, &tmp1);
  for (int y = 0; y < ysize; y++) {
    const int row0 = y * xsize;
    result[row0 + 1] += tmp0[row0];
    result[row0 + 0] += tmp0[row0 + 1];
    result[row0 + 2] += tmp0[row0 + 1];
    for (int x = 2; x < xsize - 2; ++x) {
      result[row0 + x - 1] += tmp0[row0 + x];
      result[row0 + x + 1] += tmp0[row0 + x];
    }
    result[row0 + xsize - 3] += tmp0[row0 + xsize - 2];
    result[row0 + xsize - 1] += tmp0[row0 + xsize - 2];
    result[row0 + xsize - 2] += tmp0[row0 + xsize - 1];
    if (y > 0) {
      const int rowd1 = row0 - xsize;
      result[rowd1 + 1] += tmp1[row0];
      result[rowd1 + 0] += tmp0[row0];
      for (int x = 1; x < xsize - 1; ++x) {
        result[rowd1 + x + 1] += tmp1[row0 + x];
        result[rowd1 + x + 0] += tmp0[row0 + x];
        result[rowd1 + x - 1] += tmp1[row0 + x];
      }
      result[rowd1 + xsize - 1] += tmp0[row0 + xsize - 1];
      result[rowd1 + xsize - 2] += tmp1[row0 + xsize - 1];
    }
    if (y + 1 < ysize) {
      const int rowu1 = row0 + xsize;
      result[rowu1 + 1] += tmp1[row0];
      result[rowu1 + 0] += tmp0[row0];
      for (int x = 1; x < xsize - 1; ++x) {
        result[rowu1 + x + 1] += tmp1[row0 + x];
        result[rowu1 + x + 0] += tmp0[row0 + x];
        result[rowu1 + x - 1] += tmp1[row0 + x];
      }
      result[rowu1 + xsize - 1] += tmp0[row0 + xsize - 1];
      result[rowu1 + xsize - 2] += tmp1[row0 + xsize - 1];
    }
  }
  *diffs = result;
  ScaleImage(scale, diffs);
}

void DiffPrecompute(
    const std::vector<std::vector<float> > &xyb0,
    const std::vector<std::vector<float> > &xyb1,
    size_t xsize, size_t ysize,
    std::vector<std::vector<float> > *mask) {
  PROFILER_FUNC;
  mask->resize(3, std::vector<float>(xyb0[0].size()));
  double valsh0[3] = { 0.0 };
  double valsv0[3] = { 0.0 };
  double valsh1[3] = { 0.0 };
  double valsv1[3] = { 0.0 };
  int ix2;
  for (size_t y = 0; y < ysize; ++y) {
    for (size_t x = 0; x < xsize; ++x) {
      size_t ix = x + xsize * y;
      if (x + 1 < xsize) {
        ix2 = ix + 1;
      } else {
        ix2 = ix - 1;
      }
      {
        double x0 = (xyb0[0][ix] - xyb0[0][ix2]);
        double y0 = (xyb0[1][ix] - xyb0[1][ix2]);
        double z0 = (xyb0[2][ix] - xyb0[2][ix2]);
        XybToVals(x0, y0, z0, &valsh0[0], &valsh0[1], &valsh0[2]);
        double x1 = (xyb1[0][ix] - xyb1[0][ix2]);
        double y1 = (xyb1[1][ix] - xyb1[1][ix2]);
        double z1 = (xyb1[2][ix] - xyb1[2][ix2]);
        XybToVals(x1, y1, z1, &valsh1[0], &valsh1[1], &valsh1[2]);
      }
      if (y + 1 < ysize) {
        ix2 = ix + xsize;
      } else {
        ix2 = ix - xsize;
      }
      {
        double x0 = (xyb0[0][ix] - xyb0[0][ix2]);
        double y0 = (xyb0[1][ix] - xyb0[1][ix2]);
        double z0 = (xyb0[2][ix] - xyb0[2][ix2]);
        XybToVals(x0, y0, z0, &valsv0[0], &valsv0[1], &valsv0[2]);
        double x1 = (xyb1[0][ix] - xyb1[0][ix2]);
        double y1 = (xyb1[1][ix] - xyb1[1][ix2]);
        double z1 = (xyb1[2][ix] - xyb1[2][ix2]);
        XybToVals(x1, y1, z1, &valsv1[0], &valsv1[1], &valsv1[2]);
      }
      for (int i = 0; i < 3; ++i) {
        double sup0 = fabs(valsh0[i]) + fabs(valsv0[i]);
        double sup1 = fabs(valsh1[i]) + fabs(valsv1[i]);
        double m = std::min(sup0, sup1);
        (*mask)[i][ix] = static_cast<float>(m);
      }
    }
  }
}

void Mask(const std::vector<std::vector<float> > &xyb0,
          const std::vector<std::vector<float> > &xyb1,
          size_t xsize, size_t ysize,
          std::vector<std::vector<float> > *mask,
          std::vector<std::vector<float> > *mask_dc) {
   PROFILER_FUNC;
  mask->resize(3);
  for (int i = 0; i < 3; ++i) {
    (*mask)[i].resize(xsize * ysize);
  }
  DiffPrecompute(xyb0, xyb1, xsize, ysize, mask);
  for (int i = 0; i < 3; ++i) {
    Average5x5(xsize, ysize, &(*mask)[i]);
    MinSquareVal(4, 0, xsize, ysize, (*mask)[i].data());
    static const double sigma[3] = {
      9.65781083553,
      14.2644604355,
      4.53358927369,
    };
    Blur(xsize, ysize, (*mask)[i].data(), sigma[i], 0.0);
  }
  static const double w00 = 232.206464018;
  static const double w11 = 22.9455222245;
  static const double w22 = 503.962310606;

  mask_dc->resize(3);
  for (int i = 0; i < 3; ++i) {
    (*mask_dc)[i].resize(xsize * ysize);
  }
  for (size_t y = 0; y < ysize; ++y) {
    for (size_t x = 0; x < xsize; ++x) {
      const size_t idx = y * xsize + x;
      const double s0 = (*mask)[0][idx];
      const double s1 = (*mask)[1][idx];
      const double s2 = (*mask)[2][idx];
      const double p0 = w00 * s0;
      const double p1 = w11 * s1;
      const double p2 = w22 * s2;

      (*mask)[0][idx] = static_cast<float>(MaskX(p0));
      (*mask)[1][idx] = static_cast<float>(MaskY(p1));
      (*mask)[2][idx] = static_cast<float>(MaskB(p2));
      (*mask_dc)[0][idx] = static_cast<float>(MaskDcX(p0));
      (*mask_dc)[1][idx] = static_cast<float>(MaskDcY(p1));
      (*mask_dc)[2][idx] = static_cast<float>(MaskDcB(p2));
    }
  }
  for (int i = 0; i < 3; ++i) {
    ScaleImage(kGlobalScale * kGlobalScale, &(*mask)[i]);
    ScaleImage(kGlobalScale * kGlobalScale, &(*mask_dc)[i]);
  }
}

}  // namespace butteraugli

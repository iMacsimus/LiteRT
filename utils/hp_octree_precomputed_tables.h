#pragma once
#include <cstdint>

constexpr static unsigned BASIS_MAX_DEGREE = 12;
constexpr static unsigned TREE_MAX_DEPTH   = 10;

// Generated by hp_octree_generate_tables
static constexpr float NormalisedLengths[13][11] = {
  { 1.00000000, 1.41421356, 2.00000000, 2.82842712, 4.00000000, 5.65685425, 8.00000000, 11.31370850, 16.00000000, 22.62741700, 32.00000000, },
  { 1.73205081, 2.44948974, 3.46410162, 4.89897949, 6.92820323, 9.79795897, 13.85640646, 19.59591794, 27.71281292, 39.19183588, 55.42562584, },
  { 2.23606798, 3.16227766, 4.47213595, 6.32455532, 8.94427191, 12.64911064, 17.88854382, 25.29822128, 35.77708764, 50.59644256, 71.55417528, },
  { 2.64575131, 3.74165739, 5.29150262, 7.48331477, 10.58300524, 14.96662955, 21.16601049, 29.93325909, 42.33202098, 59.86651819, 84.66404195, },
  { 3.00000000, 4.24264069, 6.00000000, 8.48528137, 12.00000000, 16.97056275, 24.00000000, 33.94112550, 48.00000000, 67.88225099, 96.00000000, },
  { 3.31662479, 4.69041576, 6.63324958, 9.38083152, 13.26649916, 18.76166304, 26.53299832, 37.52332608, 53.06599665, 75.04665216, 106.13199329, },
  { 3.60555128, 5.09901951, 7.21110255, 10.19803903, 14.42220510, 20.39607805, 28.84441020, 40.79215611, 57.68882041, 81.58431222, 115.37764081, },
  { 3.87298335, 5.47722558, 7.74596669, 10.95445115, 15.49193338, 21.90890230, 30.98386677, 43.81780460, 61.96773354, 87.63560920, 123.93546708, },
  { 4.12310563, 5.83095189, 8.24621125, 11.66190379, 16.49242250, 23.32380758, 32.98484500, 46.64761516, 65.96969001, 93.29523032, 131.93938002, },
  { 4.35889894, 6.16441400, 8.71779789, 12.32882801, 17.43559577, 24.65765601, 34.87119155, 49.31531202, 69.74238310, 98.63062405, 139.48476619, },
  { 4.58257569, 6.48074070, 9.16515139, 12.96148140, 18.33030278, 25.92296279, 36.66060556, 51.84592559, 73.32121112, 103.69185117, 146.64242224, },
  { 4.79583152, 6.78232998, 9.59166305, 13.56465997, 19.18332609, 27.12931993, 38.36665219, 54.25863987, 76.73330437, 108.51727973, 153.46660875, },
  { 5.00000000, 7.07106781, 10.00000000, 14.14213562, 20.00000000, 28.28427125, 40.00000000, 56.56854249, 80.00000000, 113.13708499, 160.00000000, },
};
static constexpr uint32_t LegendreCoeffientCount[13] = {
  1,
  4,
  10,
  20,
  35,
  56,
  83,
  120,
  165,
  220,
  286,
  364,
  455,
};
static constexpr float LegendreCoefficent[13][2] = {
  { 0.00000000, 0.00000000 },
  { 1.00000000, 0.00000000 },
  { 1.50000000, 0.50000000 },
  { 1.66666667, 0.66666667 },
  { 1.75000000, 0.75000000 },
  { 1.80000000, 0.80000000 },
  { 1.83333333, 0.83333333 },
  { 1.85714286, 0.85714286 },
  { 1.87500000, 0.87500000 },
  { 1.88888889, 0.88888889 },
  { 1.90000000, 0.90000000 },
  { 1.90909091, 0.90909091 },
  { 1.91666667, 0.91666667 },
};
static constexpr uint32_t SharedFaceLookup[3][4][2] = {
  { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, },
  { { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, },
  { { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, },
};
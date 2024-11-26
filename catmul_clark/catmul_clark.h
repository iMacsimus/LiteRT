#pragma once 

#ifndef KERNEL_SLICER
/*
* HOST Catmul Clark representation (for preprocessing on CPU)
* Almost all c++ possibilities, but KSlicer has problems with included headers
*/
#include "LiteMath.h"

struct CatmulClark
{
  LiteMath::float3 center;
  float radius;
};

struct CatmulClarkLeaveInfo
{
  float some_value1;
  float some_value2;
  float some_value3;
};

#endif

// Representation for BVH
struct CatmulClarkHeader
{
  /*
  * One object information
  * Offsets in data array (see BVHRT structure)
  * No pointers, vectors, etc
  * Only trivial types: int, float, ..., struct SomeStruct{ int, int }, etc
  */
  unsigned data_offset;
};
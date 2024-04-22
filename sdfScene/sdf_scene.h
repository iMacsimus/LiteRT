#pragma once
#include "LiteMath/LiteMath.h"
#include <vector>
#include <string>
#include <memory>

using LiteMath::float2;
using LiteMath::float3;
using LiteMath::float4;
using LiteMath::uint2;
using LiteMath::uint3;
using LiteMath::uint4;
using LiteMath::int2;
using LiteMath::int3;
using LiteMath::int4;
using LiteMath::float4x4;
using LiteMath::float3x3;
using LiteMath::cross;
using LiteMath::dot;
using LiteMath::length;
using LiteMath::normalize;
using LiteMath::to_float4;
using LiteMath::to_float3;
using LiteMath::max;
using LiteMath::min;

//################################################################################
// Constants and plain data structures definitions. Used both on GPU with slicer 
// and on CPU.
//################################################################################
// enum SdfPrimitiveType
static constexpr unsigned SDF_PRIM_SPHERE = 0;
static constexpr unsigned SDF_PRIM_BOX = 1;
static constexpr unsigned SDF_PRIM_CYLINDER = 2;
static constexpr unsigned SDF_PRIM_SIREN = 3;

struct SdfObject
{
  unsigned type;          // from enum SdfPrimitiveType
  unsigned params_offset; // in parameters vector
  unsigned params_count;
  unsigned neural_id; // index in neural_properties if type is neural

  float distance_mult;
  float distance_add;
  unsigned complement; // 0 or 1
  unsigned _pad; 

  float4 max_pos;    //float4 to prevent padding issues
  float4 min_pos;    //float4 to prevent padding issues

  float4x4 transform;
};
struct SdfConjunction
{
  float4 max_pos;
  float4 min_pos;
  unsigned offset; // in objects vector
  unsigned size;
  unsigned _pad[2];
};

constexpr int NEURAL_SDF_MAX_LAYERS = 8;
constexpr int NEURAL_SDF_MAX_LAYER_SIZE = 1024;
constexpr float SIREN_W0 = 30;

struct NeuralDenseLayer
{
  unsigned offset;
  unsigned in_size;
  unsigned out_size;
};
struct NeuralProperties
{
  unsigned layer_count;
  NeuralDenseLayer layers[NEURAL_SDF_MAX_LAYERS];
};

struct SdfHit
{
  float4 hit_pos;  // hit_pos.w < 0 if no hit, hit_pos.w > 0 otherwise
  float4 hit_norm; // hit_norm.w can store different types of things for debug/visualization purposes
};

struct SdfOctreeNode
{
  float value;
  unsigned offset; // offset for children (they are stored together). 0 offset means it's a leaf
};

struct SdfFrameOctreeNode
{
  float values[8];
  unsigned offset; // offset for children (they are stored together). 0 offset means it's a leaf  
};

//node for SparseVoxelSet, basically the same as SdfFrameOctree, but more compact
struct SdfSVSNode
{
  uint32_t pos_xy; //position of voxel in it's LOD
  uint32_t pos_z_lod_size; //size of it's LOD, (i.e. 2^LOD)
  uint32_t values[2]; //compressed distance values, 1 byte per value
};

//node for SparseBrickSet, similar idea to SparseVoxelSet, but values stored in bricks of KxKxK voxels,
//and values are shared between voxels, which leads to smaller memory footprint
struct SdfSBSNode
{
  uint32_t pos_xy; //position of start voxel of the block in it's LOD
  uint32_t pos_z_lod_size; //size of it's LOD, (i.e. 2^LOD)
  uint32_t data_offset; //offset in data vector for block with distance values, offset is in uint32_t, not bytes 
  uint32_t _pad;
};

//headed contains some info about particular SBS, such as size of a brick, precision of stored values etc
//and also some precomputed values based on them to reduce the number of calculations during rendering
struct SdfSBSHeader
{
  uint32_t brick_size;      //number of voxels in each brick, 1 to 16
  uint32_t brick_pad;       //how many additional voxels are stored on the borders, 0 is default, 1 is required for tricubic filtration
  uint32_t bytes_per_value; //1, 2 or 4 bytes per value is allowed
  uint32_t v_size;          //brick_size + 2*brick_pad + 1
};

//################################################################################
// CPU-specific functions and data structures
//################################################################################
#ifndef KERNEL_SLICER

struct SdfGridView
{
  uint3 size;
  const float *data; //size.x*size.y*size.z values 
};

struct SdfOctreeView
{
  unsigned size;
  const SdfOctreeNode *nodes;
};

struct SdfFrameOctreeView
{
  unsigned size;
  const SdfFrameOctreeNode *nodes;
};

struct SdfSVSView
{
  unsigned size;
  const SdfSVSNode *nodes;
};

struct SdfSBSView
{
  SdfSBSHeader header;
  unsigned size;
  SdfSBSNode *nodes;
  unsigned values_count;
  uint32_t *values;
};

// structure to actually store SdfScene data
struct SdfScene
{
  std::vector<float> parameters;
  std::vector<SdfObject> objects;
  std::vector<SdfConjunction> conjunctions;
  std::vector<NeuralProperties> neural_properties;
};

// structure to access and transfer SdfScene data
// all interfaces use SdfSceneView to be independant of how exactly SDF scenes are stored
struct SdfSceneView
{
  SdfSceneView() = default;
  SdfSceneView(const SdfScene &scene)
  {
    parameters = scene.parameters.data();
    objects = scene.objects.data();
    conjunctions = scene.conjunctions.data();
    neural_properties = scene.neural_properties.data();

    parameters_count = scene.parameters.size();
    objects_count = scene.objects.size();
    conjunctions_count = scene.conjunctions.size();
    neural_properties_count = scene.neural_properties.size();
  }
  SdfSceneView(const std::vector<float> &_parameters,
               const std::vector<SdfObject> &_objects,
               const std::vector<SdfConjunction> &_conjunctions,
               const std::vector<NeuralProperties> &_neural_properties)
  {
    parameters = _parameters.data();
    objects = _objects.data();
    conjunctions = _conjunctions.data();
    neural_properties = _neural_properties.data();

    parameters_count = _parameters.size();
    objects_count = _objects.size();
    conjunctions_count = _conjunctions.size();
    neural_properties_count = _neural_properties.size();
  }

  const float *parameters;
  const SdfObject *objects;
  const SdfConjunction *conjunctions;
  const NeuralProperties *neural_properties;

  unsigned parameters_count;
  unsigned objects_count;
  unsigned conjunctions_count;
  unsigned neural_properties_count;
};

// interface to evaluate SdfScene out of context of rendering
class ISdfSceneFunction
{
public:
  //copies data from scene
  virtual void init(SdfSceneView scene) = 0; 
  virtual float eval_distance(float3 pos) = 0;
};
std::shared_ptr<ISdfSceneFunction> get_SdfSceneFunction(SdfSceneView scene);

// interface to evaluate SdfOctree out of context of rendering
class ISdfOctreeFunction
{
public:
  //copies data from octree
  virtual void init(SdfOctreeView octree) = 0; 
  virtual float eval_distance(float3 pos) = 0;
  virtual float eval_distance_level(float3 pos, unsigned max_level) = 0;
  virtual std::vector<SdfOctreeNode> &get_nodes() = 0;
  virtual const std::vector<SdfOctreeNode> &get_nodes() const = 0;
};
std::shared_ptr<ISdfOctreeFunction> get_SdfOctreeFunction(SdfOctreeView scene);

// interface to evaluate SdfGrid out of context of rendering
class ISdfGridFunction
{
public:
  virtual void init(SdfGridView octree) = 0; 
  virtual float eval_distance(float3 pos) = 0;
};
std::shared_ptr<ISdfGridFunction> get_SdfGridFunction(SdfGridView scene);

// save/load scene
void save_sdf_scene_hydra(const SdfScene &scene, const std::string &folder, const std::string &name);
void save_sdf_scene(const SdfScene &scene, const std::string &path);
void load_sdf_scene(SdfScene &scene, const std::string &path);
void load_neural_sdf_scene_SIREN(SdfScene &scene, const std::string &path); // loads scene from raw SIREN weights file
#endif
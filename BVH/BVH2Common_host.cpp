#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <memory>
#include <array>
#include <map>

#include "BVH2Common.h"
#include "../utils/hp_octree_precomputed_tables.h"

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

constexpr std::size_t reserveSize = 1000;

void BVHRT::ClearGeom()
{
  m_geomData.reserve(std::max<std::size_t>(reserveSize, m_geomData.capacity()));
  m_geomData.resize(0);

  m_indices.reserve(std::max<std::size_t>(100000 * 3, m_indices.capacity()));
  m_indices.resize(0);

  m_vertPos.reserve(std::max<std::size_t>(100000, m_vertPos.capacity()));
  m_vertPos.resize(0);

  m_primIndices.reserve(std::max<std::size_t>(100000, m_primIndices.capacity()));
  m_primIndices.resize(0);

  m_allNodePairs.reserve(std::max<std::size_t>(100000, m_allNodePairs.capacity()));
  m_allNodePairs.resize(0);

  ClearScene();
}

void BVHRT::AppendTreeData(const std::vector<BVHNodePair>& a_nodes, const std::vector<uint32_t>& a_indices, const uint32_t *a_triIndices, size_t a_indNumber)
{
  m_allNodePairs.insert(m_allNodePairs.end(), a_nodes.begin(), a_nodes.end());
  m_primIndices.insert(m_primIndices.end(), a_indices.begin(), a_indices.end());
  
  const size_t oldIndexSize  = m_indices.size();
  m_indices.resize(oldIndexSize + a_indices.size()*3);
  for(size_t i=0;i<a_indices.size();i++)
  {
    const uint32_t triId = a_indices[i];
    m_indices[oldIndexSize + 3*i+0] = a_triIndices[triId*3+0];
    m_indices[oldIndexSize + 3*i+1] = a_triIndices[triId*3+1];
    m_indices[oldIndexSize + 3*i+2] = a_triIndices[triId*3+2];
  }
}

uint32_t BVHRT::AddGeom_Triangles3f(const float *a_vpos3f, size_t a_vertNumber, const uint32_t *a_triIndices, size_t a_indNumber, BuildOptions a_qualityLevel, size_t vByteStride)
{
  return AddGeom_Triangles3f(a_vpos3f, nullptr, a_vertNumber, a_triIndices, a_indNumber, a_qualityLevel, vByteStride);
}
uint32_t BVHRT::AddGeom_Triangles3f(const float* a_vpos3f, const float* a_vnorm3f, size_t a_vertNumber, const uint32_t* a_triIndices, 
                                    size_t a_indNumber, BuildOptions a_qualityLevel, size_t vByteStride)
{
  const size_t vStride = vByteStride / 4;
  assert(vByteStride % 4 == 0);

  const uint32_t currGeomId = uint32_t(m_geomData.size());
  const size_t oldSizeVert  = m_vertPos.size();
  const size_t oldSizeInd   = m_indices.size();

  m_vertPos.resize(oldSizeVert + a_vertNumber);
  m_vertNorm.resize(oldSizeVert + a_vertNumber);

  Box4f bbox;
  for (size_t i = 0; i < a_vertNumber; i++)
  {
    const float4 v = float4(a_vpos3f[i * vStride + 0], a_vpos3f[i * vStride + 1], a_vpos3f[i * vStride + 2], 1.0f);
    m_vertPos[oldSizeVert + i] = v;
    m_vertNorm[oldSizeVert + i] = a_vnorm3f ? float4(a_vnorm3f[i * vStride + 0], a_vnorm3f[i * vStride + 1], a_vnorm3f[i * vStride + 2], 1.0f) : float4(1.0f, 0.0f, 0.0f, 1.0f);
    bbox.include(v);
  }

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  //
  const size_t oldBvhSize = m_allNodePairs.size();

  GeomData geomData;
  geomData.boxMin = bbox.boxMin;
  geomData.boxMax = bbox.boxMax;
  geomData.offset = uint2(oldSizeInd, oldSizeVert);
  geomData.bvhOffset = oldBvhSize;
  geomData.type = TYPE_MESH_TRIANGLE;
  m_geomData.push_back(geomData);
  
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFat((const float*)(m_vertPos.data() + oldSizeVert), a_vertNumber, 16, a_triIndices, a_indNumber, presets, layout);

  AppendTreeData(bvhData.nodes, bvhData.indices, a_triIndices, a_indNumber);

  return currGeomId;
}

void BVHRT::UpdateGeom_Triangles3f(uint32_t a_geomId, const float *a_vpos3f, size_t a_vertNumber, const uint32_t *a_triIndices, size_t a_indNumber, BuildOptions a_qualityLevel, size_t vByteStride)
{
  std::cout << "[BVHRT::UpdateGeom_Triangles3f]: " << "not implemeted!" << std::endl; // not planned for this implementation (possible in general)
}

uint32_t BVHRT::AddGeom_SdfScene(SdfSceneView scene, BuildOptions a_qualityLevel)
{
  assert(scene.conjunctions_count > 0);
  assert(scene.objects_count > 0);
  assert(scene.parameters_count > 0);
  float4 mn = scene.conjunctions[0].min_pos;
  float4 mx = scene.conjunctions[0].max_pos;
  for (int i=0; i<scene.conjunctions_count; i++) 
  {
    mn = min(mn, scene.conjunctions[i].min_pos);
    mx = max(mx, scene.conjunctions[i].max_pos);
  }

  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_ConjIndices.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_PRIMITIVE;
  m_geomData.push_back(geomData);

  unsigned p_offset = m_SdfParameters.size();
  unsigned o_offset = m_SdfObjects.size();
  unsigned c_offset = m_SdfConjunctions.size();
  unsigned np_offset = m_SdfNeuralProperties.size();

  m_SdfParameters.insert(m_SdfParameters.end(), scene.parameters, scene.parameters + scene.parameters_count);
  m_SdfObjects.insert(m_SdfObjects.end(), scene.objects, scene.objects + scene.objects_count);
  m_SdfConjunctions.insert(m_SdfConjunctions.end(), scene.conjunctions, scene.conjunctions + scene.conjunctions_count);
  m_SdfNeuralProperties.insert(m_SdfNeuralProperties.end(), scene.neural_properties, scene.neural_properties + scene.neural_properties_count);

  for (int i=o_offset;i<m_SdfObjects.size();i++)
  {
    m_SdfObjects[i].params_offset += p_offset;
    m_SdfObjects[i].neural_id += np_offset;
  }
  
  for (int i=c_offset;i<m_SdfConjunctions.size();i++)
    m_SdfConjunctions[i].offset += o_offset;

  std::vector<unsigned> conj_indices;
  std::vector<BVHNode> orig_nodes;
  for (int i=0;i<scene.conjunctions_count;i++)
  {
    auto &c = scene.conjunctions[i];
    conj_indices.push_back(c_offset + i);
    orig_nodes.emplace_back();
    orig_nodes.back().boxMin = to_float3(c.min_pos);
    orig_nodes.back().boxMax = to_float3(c.max_pos);
  }
  while (orig_nodes.size() < 2)
  {
    conj_indices.push_back(conj_indices.back());
    orig_nodes.emplace_back();
    orig_nodes.back().boxMin = float3(1000,1000,1000);
    orig_nodes.back().boxMax = float3(1000.1,1000.1,1000.1);
  }
  m_ConjIndices.insert(m_ConjIndices.end(), conj_indices.begin(), conj_indices.end());
  //orig_nodes.resize(1);
  //orig_nodes[0].boxMin = aabb.min_pos;
  //orig_nodes[0].boxMax = aabb.max_pos;

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  for (auto &i : bvhData.indices)
    printf("ind %d\n",(int)i);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_RFScene(RFScene grid, BuildOptions a_qualityLevel)
{
  //RF grid is always a unit cube
  float4 mn = float4(0, 0, 0,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_RFGridOffsets.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_RF_GRID;
  m_geomData.push_back(geomData);

  //fill grid-specific data arrays
  m_RFGridOffsets.push_back(m_RFGridData.size());
  m_RFGridSizes.push_back(grid.size);
  m_RFGridScales.push_back(grid.scale);

  m_RFGridFlags.push_back(1); // Do RF
  m_RFGridFlags.push_back(1); // Do fast rendering

  //create list of bboxes for BLAS
  std::vector<float> sparseGrid;
  std::vector<uint4> sparsePtrs;
  m_origNodes = GetBoxes_RFGrid(grid, sparseGrid, sparsePtrs);

  m_RFGridData.insert(m_RFGridData.end(), sparseGrid.begin(), sparseGrid.end());
  m_RFGridPtrs.insert(m_RFGridPtrs.end(), sparsePtrs.begin(), sparsePtrs.end());

  std::cout << "Using "
      << (m_RFGridData.size() * sizeof(float) / 1024 / 1024 + m_RFGridPtrs.size() * sizeof(uint4) / 1024 / 1024) 
      << " MB for model" << std::endl;

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(m_origNodes.data(), m_origNodes.size(), presets, layout);

  /* for (auto &i : bvhData.indices) */
  /*   printf("grid ind %d\n",(int)i); */

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

float4x4 Transpose(float4x4& a) {
    float4x4 b;

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            b[j][i] = a[i][j];
        }
    }

    return b;
}

std::vector<float4x4> ComputeCovarianceMatrices(std::vector<float4x4>& m_gs_data_0) {
    std::vector<float4x4> covariance_matrices;

    for (int i = 0; i < m_gs_data_0.size(); ++i) {
        float4x4 S = float4x4(
            exp(m_gs_data_0[i][1][3]), 0.0f, 0.0f, 0.0f,
            0.0f, exp(m_gs_data_0[i][2][0]), 0.0f, 0.0f,
            0.0f, 0.0f, exp(m_gs_data_0[i][2][1]), 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);

        float4 q = normalize(
            float4(m_gs_data_0[i][2][2], -m_gs_data_0[i][2][3], m_gs_data_0[i][3][0], m_gs_data_0[i][3][1]));

        float r = q.x;
        float x = q.y;
        float y = q.z;
        float z = q.w;

        float4x4 R = float4x4(
            1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - r * z), 2.0f * (x * z + r * y), 0.0f,
            2.0f * (x * y + r * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - r * x), 0.0f,
            2.0f * (x * z - r * y), 2.0f * (y * z + r * x), 1.0f - 2.0f * (x * x + y * y), 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);

        float4x4 M = S * R;
        float4x4 Sigma = Transpose(M) * M;

        covariance_matrices.push_back(Sigma);
    }

    return covariance_matrices;
}

std::vector<float4x4> InvertMatrices(std::vector<float4x4>&& matrices) {
    std::vector<float4x4> inverted_matrices;
    inverted_matrices.reserve(matrices.size());

    for (int i = 0; i < matrices.size(); ++i) {
        float determinant = matrices[i][0][0] * (matrices[i][1][1] * matrices[i][2][2] - matrices[i][2][1] * matrices[i][1][2]) -
                            matrices[i][0][1] * (matrices[i][1][0] * matrices[i][2][2] - matrices[i][1][2] * matrices[i][2][0]) +
                            matrices[i][0][2] * (matrices[i][1][0] * matrices[i][2][1] - matrices[i][1][1] * matrices[i][2][0]);

        if (determinant < 1e-9f) {
            matrices[i][0][0] += 1e-9f;
            matrices[i][1][1] += 1e-9f;
            matrices[i][2][2] += 1e-9f;

            determinant = matrices[i][0][0] * (matrices[i][1][1] * matrices[i][2][2] - matrices[i][2][1] * matrices[i][1][2]) -
                          matrices[i][0][1] * (matrices[i][1][0] * matrices[i][2][2] - matrices[i][1][2] * matrices[i][2][0]) +
                          matrices[i][0][2] * (matrices[i][1][0] * matrices[i][2][1] - matrices[i][1][1] * matrices[i][2][0]);
        }

        float4x4 inverse_matrix;

        inverse_matrix[0][0] = (matrices[i][1][1] * matrices[i][2][2] - matrices[i][2][1] * matrices[i][1][2]) / determinant;
        inverse_matrix[0][1] = (matrices[i][0][2] * matrices[i][2][1] - matrices[i][0][1] * matrices[i][2][2]) / determinant;
        inverse_matrix[0][2] = (matrices[i][0][1] * matrices[i][1][2] - matrices[i][0][2] * matrices[i][1][1]) / determinant;
        inverse_matrix[1][0] = (matrices[i][1][2] * matrices[i][2][0] - matrices[i][1][0] * matrices[i][2][2]) / determinant;
        inverse_matrix[1][1] = (matrices[i][0][0] * matrices[i][2][2] - matrices[i][0][2] * matrices[i][2][0]) / determinant;
        inverse_matrix[1][2] = (matrices[i][1][0] * matrices[i][0][2] - matrices[i][0][0] * matrices[i][1][2]) / determinant;
        inverse_matrix[2][0] = (matrices[i][1][0] * matrices[i][2][1] - matrices[i][2][0] * matrices[i][1][1]) / determinant;
        inverse_matrix[2][1] = (matrices[i][2][0] * matrices[i][0][1] - matrices[i][0][0] * matrices[i][2][1]) / determinant;
        inverse_matrix[2][2] = (matrices[i][0][0] * matrices[i][1][1] - matrices[i][1][0] * matrices[i][0][1]) / determinant;

        inverted_matrices.push_back(std::move(inverse_matrix));
    }

    return inverted_matrices;
}

uint32_t BVHRT::AddGeom_GSScene(GSScene grid, BuildOptions a_qualityLevel) {
    GeomData geomData;
    geomData.boxMin = float4(-1.0f, -1.0f, -1.0f, 1.0f);
    geomData.boxMax = float4(1.0f, 1.0f, 1.0f, 1.0f);
    geomData.offset = uint2(grid.data_0.size(), 0);
    geomData.bvhOffset = m_allNodePairs.size();
    geomData.type = TYPE_GS_PRIMITIVE;
    m_geomData.push_back(geomData);

    m_gs_data_0 = grid.data_0;
    m_gs_conic = InvertMatrices(ComputeCovarianceMatrices(m_gs_data_0));

    m_origNodes = GetBoxes_GSGrid(grid);

    auto presets = BuilderPresetsFromString(m_buildName.c_str());
    auto layout = LayoutPresetsFromString(m_layoutName.c_str());
    auto bvhData = BuildBVHFatCustom(m_origNodes.data(), m_origNodes.size(), presets, layout);

    m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

    return m_geomData.size() - 1;
}

uint32_t BVHRT::AddGeom_SdfGrid(SdfGridView grid, BuildOptions a_qualityLevel)
{
  assert(grid.size.x*grid.size.y*grid.size.z > 0);
  assert(grid.size.x*grid.size.y*grid.size.z < (1u<<28)); //huge grids shouldn't be here
  //SDF grid is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfGridOffsets.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_GRID;
  m_geomData.push_back(geomData);

  //fill grid-specific data arrays
  m_SdfGridOffsets.push_back(m_SdfGridData.size());
  m_SdfGridSizes.push_back(grid.size);
  m_SdfGridData.insert(m_SdfGridData.end(), grid.data, grid.data + grid.size.x*grid.size.y*grid.size.z);

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes = GetBoxes_SdfGrid(grid);

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  /* for (auto &i : bvhData.indices) */
  /*   printf("grid ind %d\n",(int)i); */

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfOctree(SdfOctreeView octree, BuildOptions a_qualityLevel)
{
  assert(octree.size > 0);
  assert(octree.size < (1u<<28)); //huge grids shouldn't be here
  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfOctreeRoots.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_OCTREE;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  m_SdfOctreeRoots.push_back(m_SdfOctreeNodes.size());
  m_SdfOctreeNodes.insert(m_SdfOctreeNodes.end(), octree.nodes, octree.nodes + octree.size);
  for (int i=m_SdfOctreeRoots.back();i<m_SdfOctreeNodes.size();i++)
    m_SdfOctreeNodes[i].offset += m_SdfOctreeRoots.back();

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes = GetBoxes_SdfOctree(octree);

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfFrameOctree(SdfFrameOctreeView octree, BuildOptions a_qualityLevel)
{
  assert(octree.size > 0);
  assert(octree.size < (1u<<28)); //huge grids shouldn't be here
  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfFrameOctreeRoots.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_FRAME_OCTREE;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  unsigned n_offset = m_SdfFrameOctreeNodes.size();
  m_SdfFrameOctreeRoots.push_back(n_offset);
  m_SdfFrameOctreeNodes.insert(m_SdfFrameOctreeNodes.end(), octree.nodes, octree.nodes + octree.size);
  for (int i=n_offset;i<m_SdfFrameOctreeNodes.size();i++)
    m_SdfFrameOctreeNodes[i].offset += n_offset;

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes = GetBoxes_SdfFrameOctree(octree);
  m_origNodes = orig_nodes;

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfSVS(SdfSVSView octree, BuildOptions a_qualityLevel)
{
  assert(octree.size > 0);
  assert(octree.size < (1u<<28)); //huge grids shouldn't be here
  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //choose only those node that have both positive and negative values in distance array, i.e. borders
  std::vector<SdfSVSNode> border_nodes(octree.nodes, octree.nodes+octree.size);

/*border_nodes.reserve(octree.size);
  for (int i=0;i<octree.size;i++)
  {
    float sz = octree.nodes[i].pos_z_lod_size & 0x0000FFFF;
    float d_max = 2*1.41421356f/sz;

    bool less = false;
    bool more = false;
    for (int j=0;j<8;j++)
    {
      float val = -d_max + 2*d_max*(1.0/255.0f)*((octree.nodes[i].values[j/4] >> (8*(j%4))) & 0xFF);
      if (val <= 0)
        less = true;
      else if (val >= 0)
        more = true;
    }

    if (less)
      border_nodes.push_back(octree.nodes[i]);
  }*/ 

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfSVSRoots.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_SVS;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  unsigned n_offset = m_SdfSVSNodes.size();
  m_SdfSVSRoots.push_back(n_offset);
  m_SdfSVSNodes.insert(m_SdfSVSNodes.end(), border_nodes.begin(), border_nodes.end());

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes(border_nodes.size());
  for (int i=0;i<border_nodes.size();i++)
  {
    float px = border_nodes[i].pos_xy >> 16;
    float py = border_nodes[i].pos_xy & 0x0000FFFF;
    float pz = border_nodes[i].pos_z_lod_size >> 16;
    float sz = border_nodes[i].pos_z_lod_size & 0x0000FFFF;
    orig_nodes[i].boxMin = float3(-1,-1,-1) + 2.0f*float3(px,py,pz)/sz;
    orig_nodes[i].boxMax = orig_nodes[i].boxMin + 2.0f*float3(1,1,1)/sz;
  }

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfSBS(SdfSBSView octree, bool single_bvh_node, BuildOptions a_qualityLevel)
{
  assert(octree.size > 0 && octree.values_count > 0);
  assert(octree.size < (1u<<28) && octree.values_count < (1u<<28));

  bool is_single_node = single_bvh_node;

  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfSBSRoots.size(), m_SdfSBSRemap.size());
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = is_single_node ? TYPE_SDF_SBS_SINGLE_NODE : TYPE_SDF_SBS;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  unsigned n_offset = m_SdfSBSNodes.size();
  unsigned v_offset = m_SdfSBSData.size();
  m_SdfSBSRoots.push_back(n_offset);
  m_SdfSBSHeaders.push_back(octree.header);
  m_SdfSBSNodes.insert(m_SdfSBSNodes.end(), octree.nodes, octree.nodes + octree.size);
  m_SdfSBSData.insert(m_SdfSBSData.end(), octree.values, octree.values + octree.values_count);

  for (int i=n_offset; i<m_SdfSBSNodes.size(); i++)
    m_SdfSBSNodes[i].data_offset += v_offset;

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes;

  if (is_single_node)  //one node for each brick
  {
    orig_nodes.resize(octree.size);
    for (int i=0;i<octree.size;i++)
    {
      float px = octree.nodes[i].pos_xy >> 16;
      float py = octree.nodes[i].pos_xy & 0x0000FFFF;
      float pz = octree.nodes[i].pos_z_lod_size >> 16;
      float sz = octree.nodes[i].pos_z_lod_size & 0x0000FFFF;

      orig_nodes[i].boxMin = float3(-1,-1,-1) + 2.0f*float3(px,py,pz)/sz;
      orig_nodes[i].boxMax = orig_nodes[i].boxMin + 2.0f*float3(1,1,1)/sz;
    }
  }
  else  //one node for each border voxel
  {
    for (int i=0;i<octree.size;i++)
    {
      float px = octree.nodes[i].pos_xy >> 16;
      float py = octree.nodes[i].pos_xy & 0x0000FFFF;
      float pz = octree.nodes[i].pos_z_lod_size >> 16;
      float sz = octree.nodes[i].pos_z_lod_size & 0x0000FFFF;

      for (int x=0; x<octree.header.brick_size; x++)
      {
        for (int y=0; y<octree.header.brick_size; y++)
        {
          for (int z=0; z<octree.header.brick_size; z++)
          {
            //check if this voxel is on the border, only border voxels became parts of BVH
            uint3 voxelPos = uint3(x,y,z);
            uint32_t voxelId = voxelPos.x*octree.header.v_size*octree.header.v_size + voxelPos.y*octree.header.v_size + voxelPos.z;
            uint32_t v_off = m_SdfSBSNodes[n_offset + i].data_offset;
            uint32_t vals_per_int = 4/octree.header.bytes_per_value; 
            uint32_t bits = 8*octree.header.bytes_per_value;
            uint32_t max_val = octree.header.bytes_per_value == 4 ? 0xFFFFFFFF : ((1 << bits) - 1);
            float d_max = 2*1.41421356f/sz;
            float mult = 2*d_max/max_val;

            float low = 1000;
            float high = 1000;
            for (int j=0;j<8;j++)
            {
              uint3 vPos = voxelPos + uint3((j & 4) >> 2, (j & 2) >> 1, j & 1);
              uint32_t vId = vPos.x*octree.header.v_size*octree.header.v_size + vPos.y*octree.header.v_size + vPos.z;
              float val = -d_max + mult*((m_SdfSBSData[v_off + vId/vals_per_int] >> (bits*(vId%vals_per_int))) & max_val);

              low = std::min(low, val);
              high = std::max(high, val);
            }

            if (low*high <= 0)
            {
              orig_nodes.emplace_back();
              orig_nodes.back().boxMin = float3(-1,-1,-1) + 2.0f*(float3(px,py,pz)/sz + float3(voxelPos)/(sz*octree.header.brick_size));
              orig_nodes.back().boxMax = orig_nodes.back().boxMin + 2.0f*float3(1,1,1)/(sz*octree.header.brick_size);

              m_SdfSBSRemap.push_back(uint2(n_offset+i, voxelId));
            }
          }        
        }      
      }
    }
  }

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfHpOctree(SdfHPOctreeView octree, BuildOptions a_qualityLevel)
{
  assert(octree.data_size > 0 && octree.nodes_size > 0);
  assert(octree.data_size < (1u<<28) && octree.nodes_size < (1u<<28));
  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfHpOctreeRoots.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_HP;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  unsigned n_offset = m_SdfHpOctreeNodes.size();
  unsigned v_offset = m_SdfHpOctreeData.size();
  m_SdfHpOctreeRoots.push_back(n_offset);
  m_SdfHpOctreeNodes.insert(m_SdfHpOctreeNodes.end(), octree.nodes, octree.nodes + octree.nodes_size);
  m_SdfHpOctreeData.insert(m_SdfHpOctreeData.end(), octree.data, octree.data + octree.data_size);

  for (int i=n_offset; i<m_SdfSBSNodes.size(); i++)
    m_SdfSBSNodes[i].data_offset += v_offset;

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes;

  for (int i=0;i<octree.nodes_size;i++)
  {
    float px = octree.nodes[i].pos_xy >> 16;
    float py = octree.nodes[i].pos_xy & 0x0000FFFF;
    float pz = octree.nodes[i].pos_z_lod_size >> 16;
    float sz = octree.nodes[i].pos_z_lod_size & 0x0000FFFF;
    unsigned depth = octree.nodes[i].degree_lod & 0x0000FFFF;
    unsigned degree = octree.nodes[i].degree_lod >> 16;

    const float3 min_pos = float3(-1,-1,-1) + 2.0f*float3(px,py,pz)/sz;
    const float3 max_pos = min_pos + 2.0f*float3(1,1,1)/sz;
    
    orig_nodes.emplace_back();
    orig_nodes.back().boxMin = min_pos;
    orig_nodes.back().boxMax = max_pos;
  }

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

uint32_t BVHRT::AddGeom_SdfFrameOctreeTex(SdfFrameOctreeTexView octree, BuildOptions a_qualityLevel)
{
  assert(octree.size > 0);
  assert(octree.size < (1u<<28)); //huge grids shouldn't be here
  //SDF octree is always a unit cube
  float4 mn = float4(-1,-1,-1,1);
  float4 mx = float4( 1, 1, 1,1);

  //fill geom data array
  GeomData geomData;
  geomData.boxMin = mn;
  geomData.boxMax = mx;
  geomData.offset = uint2(m_SdfFrameOctreeTexRoots.size(), 0);
  geomData.bvhOffset = m_allNodePairs.size();
  geomData.type = TYPE_SDF_FRAME_OCTREE_TEX;
  m_geomData.push_back(geomData);

  //fill octree-specific data arrays
  unsigned n_offset = m_SdfFrameOctreeTexNodes.size();
  m_SdfFrameOctreeTexRoots.push_back(n_offset);
  m_SdfFrameOctreeTexNodes.insert(m_SdfFrameOctreeTexNodes.end(), octree.nodes, octree.nodes + octree.size);
  for (int i=n_offset;i<m_SdfFrameOctreeTexNodes.size();i++)
    m_SdfFrameOctreeTexNodes[i].offset += n_offset;

  //create list of bboxes for BLAS
  std::vector<BVHNode> orig_nodes = GetBoxes_SdfFrameOctreeTex(octree);
  m_origNodes = orig_nodes;

  // Build BVH for each geom and append it to big buffer;
  // append data to global arrays and fix offsets
  auto presets = BuilderPresetsFromString(m_buildName.c_str());
  auto layout  = LayoutPresetsFromString(m_layoutName.c_str());
  auto bvhData = BuildBVHFatCustom(orig_nodes.data(), orig_nodes.size(), presets, layout);

  m_allNodePairs.insert(m_allNodePairs.end(), bvhData.nodes.begin(), bvhData.nodes.end());

  return m_geomData.size()-1;
}

void BVHRT::set_debug_mode(bool enable)
{
  debug_cur_pixel = enable;
}

void BVHRT::ClearScene()
{
  m_instanceData.reserve(std::max<std::size_t>(reserveSize, m_instanceData.capacity()));
  m_instanceData.resize(0);

  m_firstSceneCommit = true;
}

void DebugPrintNodes(const std::vector<BVHNode>& nodes, const std::string& a_fileName)
{
  std::ofstream fout(a_fileName.c_str());

  for(size_t i=0;i<nodes.size();i++)
  {
    const auto& currBox = nodes[i];
    fout << "node[" << i << "]:" << std::endl;
    fout << "  bmin = { " << currBox.boxMin[0] << " " << currBox.boxMin[1] << " " << currBox.boxMin[2] << " } | " << currBox.leftOffset  << std::endl;
    fout << "  bmax = { " << currBox.boxMax[0] << " " << currBox.boxMax[1] << " " << currBox.boxMax[2] << " } | " << currBox.escapeIndex << std::endl;
  } 
}

void DebugPrintBoxes(const std::vector<Box4f>& nodes, const std::string& a_fileName)
{
  std::ofstream fout(a_fileName.c_str());

  for(size_t i=0;i<nodes.size();i++)
  {
    const auto& currBox = nodes[i];
    fout << "node[" << i << "]:" << std::endl;
    fout << "  bmin = { " << currBox.boxMin[0] << " " << currBox.boxMin[1] << " " << currBox.boxMin[2] << " " << currBox.boxMin[3]  << std::endl;
    fout << "  bmax = { " << currBox.boxMax[0] << " " << currBox.boxMax[1] << " " << currBox.boxMax[2] << " " << currBox.boxMax[3] << std::endl;
  } 
}

void BVHRT::CommitScene(uint32_t a_qualityLevel)
{
  assert(m_instanceData.size() > 0);

  //if there is only 1 instance, there is no need in TLAS
  if (m_instanceData.size() > 1)
  {
    std::vector<Box4f> instBoxes(m_instanceData.size());
    for (size_t i = 0; i < m_instanceData.size(); i++)
      instBoxes[i] = Box4f(m_instanceData[i].boxMin, m_instanceData[i].boxMax);
    
    BuilderPresets presets = {BVH2_LEFT_OFFSET, BVHQuality::HIGH, 1};
    m_nodesTLAS = BuildBVH((const BVHNode *)instBoxes.data(), instBoxes.size(), presets).nodes;
  }
  else
  {
    m_nodesTLAS.emplace_back();
    m_nodesTLAS[0].boxMin = to_float3(m_instanceData[0].boxMin);
    m_nodesTLAS[0].boxMax = to_float3(m_instanceData[0].boxMax);
    m_nodesTLAS[0].leftOffset = LEAF_BIT;
    m_nodesTLAS[0].escapeIndex = LEAF_NORMAL;
  }

  m_firstSceneCommit = false;
}

uint32_t BVHRT::AddInstance(uint32_t a_geomId, const float4x4 &a_matrix)
{
  const auto &boxMin = m_geomData[a_geomId].boxMin;
  const auto &boxMax = m_geomData[a_geomId].boxMax;

  // (1) mult mesh bounding box vertices with matrix to form new bouding box for instance
  float4 boxVertices[8]{
      a_matrix * float4{boxMin.x, boxMin.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMin.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMax.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMin.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMax.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMin.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMax.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMax.y, boxMax.z, 1.0f},
  };

  Box4f newBox;
  for (size_t i = 0; i < 8; i++)
    newBox.include(boxVertices[i]);

  // (2) append bounding box and matrices
  const uint32_t oldSize = uint32_t(m_instanceData.size());
  InstanceData instance;
  instance.boxMin = newBox.boxMin;
  instance.boxMax = newBox.boxMax;
  instance.geomId = a_geomId;
  instance.transform = a_matrix;
  instance.transformInv = inverse4x4(a_matrix);
  instance.transformInvTransposed = transpose(inverse4x4(a_matrix));

  m_instanceData.push_back(instance);

  return oldSize;
}

void BVHRT::UpdateInstance(uint32_t a_instanceId, const float4x4 &a_matrix)
{
  if(a_instanceId > m_instanceData.size())
  {
    std::cout << "[BVHRT::UpdateInstance]: " << "bad instance id == " << a_instanceId << "; size == " << m_instanceData.size() << std::endl;
    return;
  }

  const uint32_t geomId = m_instanceData[a_instanceId].geomId;
  const float4 boxMin   = m_geomData[geomId].boxMin;
  const float4 boxMax   = m_geomData[geomId].boxMax;

  // (1) mult mesh bounding box vertices with matrix to form new bouding box for instance
  float4 boxVertices[8]{
      a_matrix * float4{boxMin.x, boxMin.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMin.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMax.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMin.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMax.y, boxMin.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMin.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMin.x, boxMax.y, boxMax.z, 1.0f},
      a_matrix * float4{boxMax.x, boxMax.y, boxMax.z, 1.0f},
  };

  Box4f newBox;
  for (size_t i = 0; i < 8; i++)
    newBox.include(boxVertices[i]);

  m_instanceData[a_instanceId].boxMin = newBox.boxMin;
  m_instanceData[a_instanceId].boxMax = newBox.boxMax;
  m_instanceData[a_instanceId].transform = a_matrix;
  m_instanceData[a_instanceId].transformInv = inverse4x4(a_matrix);
}

std::vector<BVHNode> BVHRT::GetBoxes_SdfGrid(SdfGridView grid)
{
  std::vector<BVHNode> nodes;
  nodes.resize(2);
  nodes[0].boxMin = float3(-1,-1,-1);
  nodes[0].boxMax = float3(1,1,0);
  nodes[1].boxMin = float3(-1,-1,0);
  nodes[1].boxMax = float3(1,1,1);
  return nodes;
}

std::array<float, CellSize> lerpCell(const float* v0, const float* v1, const float t)
{
  std::array<float, CellSize> ret = {};

  for (size_t i = 0; i < CellSize; i++)
    ((float*)ret.data())[i] = LiteMath::lerp(((float*)v0)[i], ((float*)v1)[i], t);

  return ret;
}

size_t indexGrid(size_t x, size_t y, size_t z, size_t gridSize)
{
  return x + y * gridSize + z * gridSize * gridSize;
}

std::array<float, CellSize> interpolate(float* grid, int3 nearCoords, int3 farCoords, float3 lerpFactors, size_t gridSize) {
  auto xy00 = lerpCell((float*)&grid[indexGrid(nearCoords[0], nearCoords[1], nearCoords[2], gridSize) * CellSize],
    (float*)&grid[indexGrid(farCoords[0], nearCoords[1], nearCoords[2], gridSize) * CellSize], lerpFactors.x);
  auto xy10 = lerpCell((float*)&grid[indexGrid(nearCoords[0], farCoords[1], nearCoords[2], gridSize) * CellSize],
    (float*)&grid[indexGrid(farCoords[0], farCoords[1], nearCoords[2], gridSize) * CellSize], lerpFactors.x);
  auto xy01 = lerpCell((float*)&grid[indexGrid(nearCoords[0], nearCoords[1], farCoords[2], gridSize) * CellSize],
    (float*)&grid[indexGrid(farCoords[0], nearCoords[1], farCoords[2], gridSize) * CellSize], lerpFactors.x);
  auto xy11 = lerpCell((float*)&grid[indexGrid(nearCoords[0], farCoords[1], farCoords[2], gridSize) * CellSize],
    (float*)&grid[indexGrid(farCoords[0], farCoords[1], farCoords[2], gridSize) * CellSize], lerpFactors.x);

  auto xyz0 = lerpCell(xy00.data(), xy10.data(), lerpFactors.y);
  auto xyz1 = lerpCell(xy01.data(), xy11.data(), lerpFactors.y);

  return lerpCell(xyz0.data(), xyz1.data(), lerpFactors.z);
}

void addToVector(std::vector<float>& v, float data[28]) {
  v.insert(v.end(), data, data + 28);
}

struct hashableFloat3 {
    float x, y, z;

    bool operator< (const hashableFloat3& other) const {
        return (x < other.x || (x == other.x && y < other.y) || (x == other.x && y == other.y && z < other.z));
    }
};

std::vector<BVHNode> BVHRT::GetBoxes_RFGrid(RFScene grid, std::vector<float>& sparseGrid, std::vector<uint4>& sparsePtrs)
{
  std::cout << "Optimizing model" << std::endl;
  std::map<hashableFloat3, uint> coordsToIdx;

  std::vector<BVHNode> nodes;
  nodes.reserve((grid.size - 1) * (grid.size - 1) * (grid.size - 1));

  auto getDensity = [&](BVHNode box)
  {
    float3 bbMin = float3(box.boxMin[0], box.boxMin[1], box.boxMin[2]);
    float3 bbMax = float3(box.boxMax[0], box.boxMax[1], box.boxMax[2]);

    float3 p = (bbMin + bbMax) / 2.0f;

    float3 lerpFactors = (p - bbMin) / (bbMax - bbMin);

    int3 nearCoords = LiteMath::clamp((int3)(bbMin * grid.size), 0, grid.size - 1);
    int3 farCoords = LiteMath::clamp(nearCoords + 1, 0, grid.size - 1);

    auto gridVal = interpolate((float*)grid.data.data(), nearCoords, farCoords, lerpFactors, grid.size);
    return gridVal[0] * grid.scale;
  };

  size_t i = 0;
  std::cout << "Added nodes: " << i << '\r' << std::flush;
  #pragma omp parallel for collapse(3)
  for (size_t z = 0; z < grid.size - 1; z++)
    for (size_t y = 0; y < grid.size - 1; y++)
      for (size_t x = 0; x < grid.size - 1; x++)
      {
        BVHNode node;
        node.boxMin = float3((float)x / (float)(grid.size), (float)y / (float)(grid.size), (float)z / (float)(grid.size));
        node.boxMax = float3((float)(x + 1) / (float)(grid.size), (float)(y + 1) / (float)(grid.size), (float)(z + 1) / (float)(grid.size));

        if (getDensity(node) > 0.0f) {
          auto addCell = [&](uint3 coords) {
            addToVector(sparseGrid, &grid.data[28 * (coords[0] + coords[1] * grid.size + coords[2] * grid.size * grid.size)]);
          };

          auto addPointer = [&](uint3 coords) {
            hashableFloat3 spaceCoords = {(float)coords[0] / (float) (grid.size - 1), (float)coords[1] / (float) (grid.size - 1), (float)coords[2] / (float) (grid.size - 1)};
            if (coordsToIdx.find(spaceCoords) == coordsToIdx.end()) {
              coordsToIdx[spaceCoords] = sparseGrid.size() / 28;
              addToVector(sparseGrid, &grid.data[28 * (coords[0] + coords[1] * grid.size + coords[2] * grid.size * grid.size)]);
            }
            return coordsToIdx[spaceCoords];
          };

          #pragma omp critical
          {
            if (m_RFGridFlags[1] == 1) {
              addCell(uint3(x, y, z));
              addCell(uint3(x + 1, y, z));
              addCell(uint3(x, y + 1, z));
              addCell(uint3(x, y, z + 1));

              addCell(uint3(x + 1, y + 1, z));
              addCell(uint3(x, y + 1, z + 1));
              addCell(uint3(x + 1, y, z + 1));
              addCell(uint3(x + 1, y + 1, z + 1));
            } else {
              uint4 ptrs;
              ptrs[0] = addPointer(uint3(x, y, z));
              ptrs[1] = addPointer(uint3(x + 1, y, z));
              ptrs[2] = addPointer(uint3(x, y + 1, z));
              ptrs[3] = addPointer(uint3(x, y, z + 1));

              sparsePtrs.push_back(ptrs);

              ptrs[0] = addPointer(uint3(x + 1, y + 1, z));
              ptrs[1] = addPointer(uint3(x, y + 1, z + 1));
              ptrs[2] = addPointer(uint3(x + 1, y, z + 1));
              ptrs[3] = addPointer(uint3(x + 1, y + 1, z + 1));

              sparsePtrs.push_back(ptrs);
            }

            nodes.push_back(node);
            std::cout << "Added nodes: " << ++i << '\r' << std::flush;
          }
        }
      }

  return nodes;
}

std::vector<BVHNode> BVHRT::GetBoxes_GSGrid(const GSScene& grid) {
  std::vector<BVHNode> nodes;
  nodes.reserve(grid.data_0.size());

  for (size_t i = 0; i < grid.data_0.size(); ++i) {
    float scale = exp(max(max(grid.data_0[i](1, 3), grid.data_0[i](2, 0)), grid.data_0[i](2, 1))) * 3.0f;

    BVHNode node;

    node.boxMin = float3(grid.data_0[i](0, 0) - scale, grid.data_0[i](0, 1) - scale, grid.data_0[i](0, 2) - scale);
    node.boxMax = float3(grid.data_0[i](0, 0) + scale, grid.data_0[i](0, 1) + scale, grid.data_0[i](0, 2) + scale);

    nodes.push_back(node);
  }

  return nodes;
}

std::vector<BVHNode> BVHRT::GetBoxes_SdfOctree(SdfOctreeView octree)
{
  std::vector<BVHNode> nodes;
  nodes.resize(2);
  nodes[0].boxMin = float3(-1,-1,-1);
  nodes[0].boxMax = float3(1,1,0);
  nodes[1].boxMin = float3(-1,-1,0);
  nodes[1].boxMax = float3(1,1,1);
  return nodes;
}

void add_border_nodes_rec(const SdfFrameOctreeView &octree, std::vector<BVHNode> &nodes,
                          unsigned idx, float3 p, float d)
{
  unsigned ofs = octree.nodes[idx].offset;
  if (ofs == 0) 
  {
    bool less = false;
    bool more = false;
    for (int i=0;i<8;i++)
    {
      if (octree.nodes[idx].values[i] <= 0)
        less = true;
      else if (octree.nodes[idx].values[i] >= -sqrt(3)*d)
        more = true;
    }

    if (true)
    {
      float3 min_pos = 2.0f*(d*p) - 1.0f;
      float3 max_pos = min_pos + 2.0f*d*float3(1,1,1);
      nodes.emplace_back();
      nodes.back().boxMax = max_pos;
      nodes.back().boxMin = min_pos;
      nodes.back().leftOffset = idx; //just store idx here, it will be later replaced by real offset in BVHBuilder anyway
    }
  }
  else
  {
    for (int i = 0; i < 8; i++)
    {
      float ch_d = d / 2;
      float3 ch_p = 2 * p + float3((i & 4) >> 2, (i & 2) >> 1, i & 1);
      add_border_nodes_rec(octree, nodes, ofs + i, ch_p, ch_d);
    }
  }
}

std::vector<BVHNode> BVHRT::GetBoxes_SdfFrameOctree(SdfFrameOctreeView octree)
{
  std::vector<BVHNode> nodes;
  add_border_nodes_rec(octree, nodes, 0, float3(0,0,0), 1);
  return nodes;
}

void add_border_nodes_rec(const SdfFrameOctreeTexView &octree, std::vector<BVHNode> &nodes,
                          unsigned idx, float3 p, float d)
{
  unsigned ofs = octree.nodes[idx].offset;
  if (ofs == 0) 
  {
    bool less = false;
    bool more = false;
    for (int i=0;i<8;i++)
    {
      if (octree.nodes[idx].values[i] <= 0)
        less = true;
      else if (octree.nodes[idx].values[i] >= -sqrt(3)*d)
        more = true;
    }

    if (true)
    {
      float3 min_pos = 2.0f*(d*p) - 1.0f;
      float3 max_pos = min_pos + 2.0f*d*float3(1,1,1);
      nodes.emplace_back();
      nodes.back().boxMax = max_pos;
      nodes.back().boxMin = min_pos;
      nodes.back().leftOffset = idx; //just store idx here, it will be later replaced by real offset in BVHBuilder anyway
    }
  }
  else
  {
    for (int i = 0; i < 8; i++)
    {
      float ch_d = d / 2;
      float3 ch_p = 2 * p + float3((i & 4) >> 2, (i & 2) >> 1, i & 1);
      add_border_nodes_rec(octree, nodes, ofs + i, ch_p, ch_d);
    }
  }
}

std::vector<BVHNode> BVHRT::GetBoxes_SdfFrameOctreeTex(SdfFrameOctreeTexView octree)
{
  std::vector<BVHNode> nodes;
  add_border_nodes_rec(octree, nodes, 0, float3(0,0,0), 1);

  return nodes;
}

//SdfSceneFunction interface implementation
void BVHRT::init(SdfSceneView scene)
{
  m_SdfParameters.insert(m_SdfParameters.end(), scene.parameters, scene.parameters + scene.parameters_count);
  m_SdfObjects.insert(m_SdfObjects.end(), scene.objects, scene.objects + scene.objects_count);
  m_SdfConjunctions.insert(m_SdfConjunctions.end(), scene.conjunctions, scene.conjunctions + scene.conjunctions_count);
  m_SdfNeuralProperties.insert(m_SdfNeuralProperties.end(), scene.neural_properties, scene.neural_properties + scene.neural_properties_count);
} 
  
float BVHRT::eval_distance(float3 pos)
{
  if (!m_SdfConjunctions.empty())
  {
    float dist = 1e6;
    for (int i=0; i<m_SdfConjunctions.size(); i++)
      dist = std::min(dist, eval_dist_sdf_conjunction(i, pos));
    return dist;
  }
  else if (!m_SdfOctreeNodes.empty())
    return eval_distance_sdf_octree(0, pos, 1000);
  else if (!m_SdfGridData.empty())
    return eval_distance_sdf_grid(0, pos);

  return 1e6; 
}

//SdfOctreeFunction interface implementation
void BVHRT::init(SdfOctreeView octree)
{
  m_SdfOctreeRoots.push_back(m_SdfOctreeNodes.size());
  m_SdfOctreeNodes.insert(m_SdfOctreeNodes.end(), octree.nodes, octree.nodes + octree.size);  
}

float BVHRT::eval_distance_level(float3 pos, unsigned max_level)
{
  return eval_distance_sdf_octree(0, pos, max_level);
}

std::vector<SdfOctreeNode> &BVHRT::get_nodes()
{
  return m_SdfOctreeNodes;
}

const std::vector<SdfOctreeNode> &BVHRT::get_nodes() const
{
  return m_SdfOctreeNodes;
}

//SdfGridFunction interface implementation
void BVHRT::init(SdfGridView grid)
{
  m_SdfGridOffsets.push_back(m_SdfGridData.size());
  m_SdfGridSizes.push_back(grid.size);
  m_SdfGridData.insert(m_SdfGridData.end(), grid.data, grid.data + grid.size.x*grid.size.y*grid.size.z);
} 

//implementation of different constructor-like functions

std::shared_ptr<ISdfSceneFunction> get_SdfSceneFunction(SdfSceneView scene)
{
  std::shared_ptr<ISdfSceneFunction> rt(new BVHRT("", "")); 
  rt->init(scene);
  return rt;
}

std::shared_ptr<ISdfOctreeFunction> get_SdfOctreeFunction(SdfOctreeView scene)
{
  std::shared_ptr<ISdfOctreeFunction> rt(new BVHRT("", "")); 
  rt->init(scene);
  return rt;  
}

std::shared_ptr<ISdfGridFunction> get_SdfGridFunction(SdfGridView scene)
{
  std::shared_ptr<ISdfGridFunction> rt(new BVHRT("", "")); 
  rt->init(scene);
  return rt;    
}

ISceneObject* MakeBruteForceRT(const char* a_implName);
ISceneObject* MakeBVH2CommonRT(const char* a_implName, const char* a_buildName, const char* a_layoutName) 
{
  return new BVHRT(a_buildName, a_layoutName); 
}
std::shared_ptr<ISceneObject> CreateSceneRT(const char* a_implName, const char* a_buildName, const char* a_layoutName)
{
  const std::string className(a_implName); //
  if (className.find("BruteForce") != std::string::npos)
    return std::shared_ptr<ISceneObject>(MakeBruteForceRT(a_implName));
  else if (className.find("BVH2Common") != std::string::npos || className.find("BVH2") != std::string::npos)
    return std::shared_ptr<ISceneObject>(MakeBVH2CommonRT(a_implName, a_buildName, a_layoutName));
  return nullptr;
}

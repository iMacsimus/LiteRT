#pragma once

#include <cstdint>
#include <memory>
#include <array>
#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>

#include "LiteMath.h"
#include "../ISceneObject.h"
#include "../IRenderer.h"
#include "../BVH/BVH2Common.h"
#include "LiteScene/cmesh4.h"
#include "Image2d.h"

using LiteMath::uint;
using LiteImage::Image2D;
using LiteImage::Sampler;
using LiteImage::ICombinedImageSampler;

struct SparseOctreeSettings;

//enum MultiRendererMaterialType
static constexpr unsigned MULTI_RENDER_MATERIAL_TYPE_COLORED  = 0;
static constexpr unsigned MULTI_RENDER_MATERIAL_TYPE_TEXTURED = 1;

struct MultiRendererMaterial
{
  unsigned type;
  unsigned texId; // valid if type == MULTI_RENDER_MATERIAL_TYPE_TEXTURED
  unsigned _pad[2];
  float4 base_color; // valid if type == MULTI_RENDER_MATERIAL_TYPE_COLORED
};

class MultiRenderer : public IRenderer
{
public:

  //a bunch of functions extending IRenderer to make working with MultiRenderer easier
#ifndef KERNEL_SLICER 
  void SetScene(const cmesh4::SimpleMesh &scene);
  void SetScene(SdfSceneView scene);
  void SetScene(SdfGridView scene);
  void SetScene(SdfOctreeView scene);
  void SetScene(SdfFrameOctreeView scene);
  void SetScene(SdfSVSView scene);
  void SetScene(SdfSBSView scene);
  void SetScene(SdfHPOctreeView scene);

  void SetScene(SdfFrameOctreeTexView scene);
#endif
  void Render(uint32_t* imageData, uint32_t a_width, uint32_t a_height, 
              const LiteMath::float4x4& a_worldView, const LiteMath::float4x4& a_proj,
              MultiRenderPreset preset = getDefaultPreset(), int a_passNum = 1);

  void SetPreset(const MultiRenderPreset& a_preset);

  //functions implementing IRenderer interface
  MultiRenderer(); 
  const char* Name() const override;
  
  //required by slicer!
  virtual void SceneRestrictions(uint32_t a_restrictions[4]) const
  {
    uint32_t maxMeshes            = 1024;
    uint32_t maxTotalVertices     = 8'000'000;
    uint32_t maxTotalPrimitives   = 8'000'000;
    uint32_t maxPrimitivesPerMesh = 4'000'000;

    a_restrictions[0] = maxMeshes;
    a_restrictions[1] = maxTotalVertices;
    a_restrictions[2] = maxTotalPrimitives;
    a_restrictions[3] = maxPrimitivesPerMesh;
  }

  bool LoadScene(const char* a_scenePath) override;

  void Clear (uint32_t a_width, uint32_t a_height, const char* a_what) override;
  void Render(uint32_t* imageData, uint32_t a_width, uint32_t a_height, const char* a_what, int a_passNum = 1) override;
  void SetViewport(int a_xStart, int a_yStart, int a_width, int a_height) override;

  void SetAccelStruct(std::shared_ptr<ISceneObject> a_customAccelStruct) override { m_pAccelStruct = a_customAccelStruct;}
  std::shared_ptr<ISceneObject> GetAccelStruct() override { return m_pAccelStruct; }

  void GetExecutionTime(const char* a_funcName, float a_out[4]) override;

  void CommitDeviceData() override {}

  void UpdateCamera(const LiteMath::float4x4& a_worldView, const LiteMath::float4x4& a_proj) override;
  
  //ddefault one, loading meshes
  bool LoadSceneHydra(const std::string& a_path);

  //so_settings is used only when type is soem kind of SDF octree
  bool LoadSceneHydra(const std::string& a_path, unsigned type, SparseOctreeSettings so_settings);  

  uint32_t AddTexture(const Image2D<float4> &image);
  uint32_t AddMaterial(const MultiRendererMaterial &material);
  void     SetMaterial(uint32_t matId, uint32_t geomId);

  LiteMath::float4x4 getProj() { return m_proj; }
  LiteMath::float4x4 getWorldView() { return m_worldView; }

protected:

  virtual void PackXYBlock(uint tidX, uint tidY, uint a_passNum);
  virtual void PackXY(uint tidX, uint tidY);
  virtual void kernel_PackXY(uint tidX, uint tidY, uint* out_pakedXY);
  
  virtual void CastRaySingle(uint32_t tidX, uint32_t* out_color __attribute__((size("tidX"))));

  virtual void CastRaySingleBlock(uint32_t tidX, uint32_t* out_color, uint32_t a_numPasses = 1);

  void kernel_InitEyeRay(uint32_t tidX, LiteMath::float4* rayPosAndNear, LiteMath::float4* rayDirAndFar);
  void kernel_RayTrace(uint32_t tidX, const LiteMath::float4* rayPosAndNear, const LiteMath::float4* rayDirAndFar, uint32_t* out_color);

#ifndef KERNEL_SLICER 
  void add_mesh_internal(const cmesh4::SimpleMesh& mesh, unsigned geomId);
#endif  

  uint32_t m_width;
  uint32_t m_height;
  MultiRenderPreset m_preset;

  LiteMath::float4x4 m_proj;
  LiteMath::float4x4 m_worldView;
  LiteMath::float4x4 m_projInv;
  LiteMath::float4x4 m_worldViewInv;

  std::shared_ptr<ISceneObject> m_pAccelStruct;
  std::vector<uint32_t>         m_packedXY;

  float4 m_mainLightDir; //direction to main light, normalized
  float4 m_mainLightColor; //color of main light, also intensity

  //duplicating data for meshes if we want to visualize them with textures
#ifndef DISABLE_MESH_TEX
  std::vector<float4> m_vertices; //.w is tc.x
  std::vector<float4> m_normals;  //.w is tc.y
  std::vector<uint32_t> m_indices;
  std::vector<uint2> m_geomOffsets;
#endif

  //materials and textures if at least one textured type is enabled
//#if !defined(DISABLE_MESH_TEX) || !defined(DISABLE_SDF_TEX)
  std::vector<MultiRendererMaterial> m_materials;
  std::vector< std::shared_ptr<ICombinedImageSampler> > m_textures;
  std::vector<uint32_t> m_matIdbyPrimId;
  std::vector<uint2> m_matIdOffsets; //for every geometry, start and size of it's part of m_matIdbyPrimId
  unsigned active_textures_count = 0;
//#endif

  // color palette to select color for objects based on mesh/instance id
  static constexpr uint32_t palette_size = 20;
  static constexpr uint32_t m_palette[palette_size] = {
    0xffe6194b, 0xff3cb44b, 0xffffe119, 0xff0082c8,
    0xfff58231, 0xff911eb4, 0xff46f0f0, 0xfff032e6,
    0xffd2f53c, 0xfffabebe, 0xff008080, 0xffe6beff,
    0xffaa6e28, 0xfffffac8, 0xff800000, 0xffaaffc3,
    0xff808000, 0xffffd8b1, 0xff000080, 0xff808080
  };

  //statistics
  std::unordered_map<std::string, float> timeDataByName;
  uint64_t m_totalTris         = 0;
  uint64_t m_totalTrisVisiable = 0;

  uint32_t GetGeomNum() const override { return m_pAccelStruct->GetGeomNum(); };
  uint32_t GetInstNum() const override { return m_pAccelStruct->GetInstNum(); };
  const LiteMath::float4* GetGeomBoxes() const  override { return m_pAccelStruct->GetGeomBoxes(); };
};

std::shared_ptr<MultiRenderer> CreateMultiRenderer(const char* a_name);

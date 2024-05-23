#include <cfloat>
#include <cstring>
#include <sstream>
#include <fstream>

#include "eye_ray.h"
#include "LiteScene/hydraxml.h"
#include "LiteScene/cmesh4.h"
#include "../Timer.h"
#include "../utils/mesh.h"
#include "../utils/mesh_bvh.h"
#include "../utils/sparse_octree.h"

using LiteMath::float2;
using LiteMath::float3;
using LiteMath::float4;

using LiteMath::perspectiveMatrix;
using LiteMath::lookAt;
using LiteMath::inverse4x4;

MultiRenderer::MultiRenderer() 
{ 
  m_pAccelStruct = CreateSceneRT("BVH2Common", "cbvh_embree2", "SuperTreeletAlignedMerged4"); //default
  m_preset = getDefaultPreset();
  m_mainLightDir = normalize3(float4(1,0.5,0.5,1));
  m_mainLightColor = 1.0f*normalize3(float4(1,1,0.98,1));
}

void MultiRenderer::SetViewport(int a_xStart, int a_yStart, int a_width, int a_height)
{
  m_width  = a_width;
  m_height = a_height;
  m_packedXY.resize(m_width*m_height);
}

bool MultiRenderer::LoadScene(const char* a_scenePath)
{
  m_pAccelStruct->ClearGeom();
  return LoadSceneHydra(std::string(a_scenePath));
}

void MultiRenderer::UpdateCamera(const LiteMath::float4x4& worldView, const LiteMath::float4x4& proj)
{
  m_proj = proj;
  m_worldView = worldView;
  m_projInv      = inverse4x4(proj);
  m_worldViewInv = inverse4x4(worldView);
}

bool MultiRenderer::LoadSceneHydra(const std::string& a_path)
{
  return LoadSceneHydra(a_path, TYPE_MESH_TRIANGLE, SparseOctreeSettings());
}

bool MultiRenderer::LoadSceneHydra(const std::string& a_path, unsigned type, SparseOctreeSettings so_settings)
{
  hydra_xml::HydraScene scene;
  if(scene.LoadState(a_path) < 0)
    return false;

  for(auto cam : scene.Cameras())
  {
    float aspect   = float(m_width) / float(m_height);
    auto proj      = perspectiveMatrix(cam.fov, aspect, cam.nearPlane, cam.farPlane);
    auto worldView = lookAt(float3(cam.pos), float3(cam.lookAt), float3(cam.up));
    UpdateCamera(worldView, proj);
    break; // take first cam
  }

  m_pAccelStruct->ClearGeom();
  auto mIter = scene.GeomNodes().begin();
  size_t pos = a_path.find_last_of('/');
  std::string root_dir = a_path.substr(0, pos);

  std::vector<LiteMath::float4x4> addGeomTransform;

  while (mIter != scene.GeomNodes().end())
  {
    std::string dir = root_dir + "/" + hydra_xml::ws2s(std::wstring(mIter->attribute(L"loc").as_string()));
    std::string name = hydra_xml::ws2s(std::wstring(mIter->name()));
    addGeomTransform.push_back(float4x4());
    if (name == "mesh")
    {
      std::cout << "[LoadScene]: mesh = " << dir.c_str() << std::endl;
      auto currMesh = cmesh4::LoadMeshFromVSGF(dir.c_str());
      float4x4 trans = cmesh4::normalize_mesh(currMesh, true);
      addGeomTransform.back() = inverse4x4(trans);

      switch (type)
      {
      case TYPE_MESH_TRIANGLE:
      {
        m_pAccelStruct->AddGeom_Triangles3f((const float *)currMesh.vPos4f.data(), currMesh.vPos4f.size(),
                                            currMesh.indices.data(), currMesh.indices.size(), BUILD_HIGH, sizeof(float) * 4);
      }
      break;
      case TYPE_SDF_SVS:
      {
        std::vector<SdfSVSNode> svs_nodes;
        if (so_settings.build_type == SparseOctreeBuildType::DEFAULT)
        {
          MeshBVH mesh_bvh;
          mesh_bvh.init(currMesh);
          SparseOctreeBuilder builder;
          builder.construct([&mesh_bvh](const float3 &p)
                            { return mesh_bvh.get_signed_distance(p); },
                            so_settings);
          builder.convert_to_sparse_voxel_set(svs_nodes);
        }
        else if (so_settings.build_type == SparseOctreeBuildType::MESH_TLO)
        {
          // search_range_mult is selected by experiments, more is better, but slower.
          // 2.0 is probably the right value in theory.
          constexpr float search_range_mult = 2.0f;
          auto oct = cmesh4::create_triangle_list_octree(currMesh, so_settings.depth, 1, search_range_mult);
          SparseOctreeBuilder::mesh_octree_to_SVS(currMesh, oct, svs_nodes);
        }
        if (false) //use to check quality
        {
          MeshBVH mesh_bvh;
          mesh_bvh.init(currMesh);
          SparseOctreeBuilder builder;
          builder.check_quality([&mesh_bvh](const float3 &p)
                            { return mesh_bvh.get_signed_distance(p); }, svs_nodes);
        }
        m_pAccelStruct->AddGeom_SdfSVS({(unsigned)svs_nodes.size(), svs_nodes.data()});
      }
      break;
      case TYPE_SDF_GRID:
      {
        MeshBVH mesh_bvh;
        mesh_bvh.init(currMesh);
        unsigned sz = pow(2, so_settings.depth);
        std::vector<float> data(sz * sz * sz, 0);

        for (int i = 0; i < sz; i++)
        {
          for (int j = 0; j < sz; j++)
          {
            for (int k = 0; k < sz; k++)
            {
              data[i * sz * sz + j * sz + k] = mesh_bvh.get_signed_distance(2.0f * (float3(k + 0.5, j + 0.5, i + 0.5) / float(sz)) - 1.0f);
            }
          }
        }

        m_pAccelStruct->AddGeom_SdfGrid(SdfGridView(uint3(sz, sz, sz), data));
      }
      break;
      default:
        printf("cannot transform meshes from Hydra scene to type %u\n", type);
        break;
      }
    }
    else if (name == "sdf")
    {
      std::cout << "[LoadScene]: sdf = " << dir.c_str() << std::endl;
      SdfScene scene;
      load_sdf_scene(scene, dir);
      m_pAccelStruct->AddGeom_SdfScene(scene);
    }
    else if (name == "sdf_grid")
    {
      std::cout << "[LoadScene]: sdf grid = " << dir.c_str() << std::endl;
      SdfGrid scene;
      load_sdf_grid(scene, dir);
      m_pAccelStruct->AddGeom_SdfGrid(scene);
    }
    else if (name == "sdf_octree")
    {
      std::cout << "[LoadScene]: sdf octree = " << dir.c_str() << std::endl;
      std::vector<SdfOctreeNode> scene;
      load_sdf_octree(scene, dir);
      m_pAccelStruct->AddGeom_SdfOctree({(unsigned)scene.size(), scene.data()});
    }
    else if (name == "sdf_frame_octree")
    {
      std::cout << "[LoadScene]: sdf frame octree = " << dir.c_str() << std::endl;
      std::vector<SdfFrameOctreeNode> scene;
      load_sdf_frame_octree(scene, dir);
      m_pAccelStruct->AddGeom_SdfFrameOctree({(unsigned)scene.size(), scene.data()});
    }
    else if (name == "sdf_svs")
    {
      std::cout << "[LoadScene]: sdf svs = " << dir.c_str() << std::endl;
      std::vector<SdfSVSNode> scene;
      load_sdf_SVS(scene, dir);
      m_pAccelStruct->AddGeom_SdfSVS({(unsigned)scene.size(), scene.data()});
    }
    else if (name == "sdf_sbs")
    {
      std::cout << "[LoadScene]: sdf sbs = " << dir.c_str() << std::endl;
      SdfSBS scene;
      load_sdf_SBS(scene, dir);
      m_pAccelStruct->AddGeom_SdfSBS(scene);
    }
    else if (name == "sdf_hp")
    {
      std::cout << "[LoadScene]: sdf hp = " << dir.c_str() << std::endl;
      SdfHPOctree scene;
      load_sdf_hp_octree(scene, dir);
      m_pAccelStruct->AddGeom_SdfHpOctree(scene);
    }
    else if (name == "nsdf")
    {
      std::cout << "[LoadScene]: neural sdf = " << dir.c_str() << std::endl;
      SdfScene scene;
      load_neural_sdf_scene_SIREN(scene, dir);
      m_pAccelStruct->AddGeom_SdfScene(scene);
    }
    else if (name == "rf")
    {
      std::cout << "[LoadScene]: radiance fields = " << dir.c_str() << std::endl;
      RFScene scene;
      load_rf_scene(scene, dir);
      m_pAccelStruct->AddGeom_RFScene(scene);
    }
    else if (name == "gs")
    {
      std::cout << "[LoadScene]: gaussian splatting = " << dir.c_str() << std::endl;
      GSScene scene;
      load_gs_scene(scene, dir);
      m_pAccelStruct->AddGeom_GSScene(scene);
    }
    else
    {
      std::cout << "[LoadScene]: unknown geometry node type: " << name.c_str() << std::endl;
    }
    mIter++;
  }
  
  m_pAccelStruct->ClearScene();
  for(auto inst : scene.InstancesGeom())
    m_pAccelStruct->AddInstance(inst.geomId, inst.matrix*addGeomTransform[inst.geomId]);
  m_pAccelStruct->CommitScene();

  return true;
}

////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////

void MultiRenderer::Render(uint32_t* a_outColor, uint32_t a_width, uint32_t a_height, const char* a_what, int a_passNum)
{
  profiling::Timer timer;
  for (int i=0;i<a_passNum;i++)
    CastRaySingleBlock(a_width*a_height, a_outColor, a_passNum);
  timeDataByName["CastRaySingleBlock"] = timer.getElapsedTime().asMilliseconds();
}

void MultiRenderer::CastRaySingleBlock(uint32_t tidX, uint32_t * out_color, uint32_t a_numPasses)
{
  //CPU version is mostly used by debug, so better make it single-threaded
  //also per-pixel debug does not work with multithreading
  //#ifndef _DEBUG
  //#pragma omp parallel for default(shared)
  //#endif
  for(int i=0;i<tidX;i++)
    CastRaySingle(i, out_color);
}

const char* MultiRenderer::Name() const
{
  return m_pAccelStruct->Name();
}

void MultiRenderer::GetExecutionTime(const char* a_funcName, float a_out[4])
{
  auto p = timeDataByName.find(a_funcName);
  if(p == timeDataByName.end())
    return;
  a_out[0] = p->second;
}

void MultiRenderer::SetScene(const cmesh4::SimpleMesh &scene)
{
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_Triangles3f((const float*)scene.vPos4f.data(), scene.vPos4f.size(),
                                        scene.indices.data(), scene.indices.size(), BUILD_HIGH, sizeof(float)*4);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfSceneView scene)
{
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfScene(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfGridView scene)
{
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfGrid(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfOctreeView scene)
{
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfOctree(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfFrameOctreeView scene)
{
  SetPreset(m_preset);
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfFrameOctree(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfSVSView scene)
{
  SetPreset(m_preset);
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfSVS(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}


void MultiRenderer::SetScene(SdfSBSView scene)
{
  SetPreset(m_preset);
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfSBS(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetScene(SdfHPOctreeView scene)
{
  SetPreset(m_preset);
  GetAccelStruct()->ClearGeom();
  GetAccelStruct()->AddGeom_SdfHpOctree(scene);
  GetAccelStruct()->ClearScene();
  GetAccelStruct()->AddInstance(0, LiteMath::float4x4());
  GetAccelStruct()->CommitScene();
}

void MultiRenderer::SetPreset(const MultiRenderPreset& a_preset)
{
  m_preset = a_preset;

  if (m_pAccelStruct)
    m_pAccelStruct->SetPreset(a_preset);
}

void MultiRenderer::Render(uint32_t* imageData, uint32_t a_width, uint32_t a_height, 
                           const LiteMath::float4x4& a_worldView, const LiteMath::float4x4& a_proj,
                           MultiRenderPreset preset, int a_passNum)
{
  SetViewport(0,0, a_width, a_height);
  UpdateCamera(a_worldView, a_proj);
  SetPreset(preset);
  CommitDeviceData();
  Clear(a_width, a_height, "color");
  Render(imageData, a_width, a_height, "color", a_passNum); 
}

#if defined(USE_GPU)
#include "eye_ray_gpu.h"
std::shared_ptr<MultiRenderer> CreateMultiRenderer_GPU(vk_utils::VulkanContext a_ctx, size_t a_maxThreadsGenerated);
std::shared_ptr<MultiRenderer> CreateMultiRenderer(const char* a_name) 
{ 
  if (std::string(a_name) == "GPU")
    return CreateMultiRenderer_GPU(vk_utils::globalContextGet(true, 0u), 256); 
  else
    return std::shared_ptr<MultiRenderer>(new MultiRenderer());
}
#else
std::shared_ptr<MultiRenderer> CreateMultiRenderer(const char* a_name) 
{ 
  return std::shared_ptr<MultiRenderer>(new MultiRenderer()); 
}
#endif

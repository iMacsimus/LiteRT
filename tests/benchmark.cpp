#include "tests.h"
#include "../IRenderer.h"
#include "../Renderer/eye_ray.h"
#include "../utils/mesh_bvh.h"
#include "../utils/mesh.h"
#include "../utils/sparse_octree.h"
#include "LiteScene/hydraxml.h"
#include "LiteMath/Image2d.h"

#include <functional>
#include <cassert>
#include <chrono>
#include <map>

void render(LiteImage::Image2D<uint32_t> &image, std::shared_ptr<MultiRenderer> pRender, 
            float3 pos, float3 target, float3 up, 
            MultiRenderPreset preset, int a_passNum);

struct BenchmarkResult
{
  unsigned iters;
  float4 render_average_time_ms;
  float4 render_min_time_ms;
  std::string scene_name;
  std::string render_name;
  std::string as_name;
  std::string preset_name;
};

void benchmark_framed_octree_intersection()
{
  constexpr unsigned iters = 3;
  constexpr unsigned pass_size = 50;
  SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
  SdfSBSHeader header;
  header.brick_size = 1;
  header.brick_pad = 0;
  header.bytes_per_value = 1;
  unsigned W = 1024, H = 1024;
  bool save_images = true;

  std::vector<BenchmarkResult> results;

  std::vector<std::string> scene_paths = {"scenes/01_simple_scenes/data/teapot.vsgf"}; 
  std::vector<std::string> scene_names = {"Teapot", "Bunny"};

  std::vector<unsigned> render_modes = {MULTI_RENDER_MODE_LAMBERT, MULTI_RENDER_MODE_LINEAR_DEPTH};
  std::vector<std::string> render_names = {"lambert", "depth"};

  std::vector<unsigned> AS_types = {TYPE_SDF_FRAME_OCTREE, TYPE_SDF_SVS, TYPE_SDF_SBS, TYPE_MESH_TRIANGLE};
  std::vector<std::string> AS_names = {"framed_octree", "sparse_voxel_set", "sparse_brick_set", "mesh"};

  std::vector<std::vector<unsigned>> presets_ob(4);
  std::vector<std::vector<unsigned>> presets_oi(4);
  std::vector<std::vector<std::string>> preset_names(4);

  presets_ob[0] = {SDF_OCTREE_BLAS_NO, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT};

  presets_oi[0] = {SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                   SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                   SDF_OCTREE_NODE_INTERSECT_ST, 
                   SDF_OCTREE_NODE_INTERSECT_ANALYTIC, 
                   SDF_OCTREE_NODE_INTERSECT_NEWTON,
                   SDF_OCTREE_NODE_INTERSECT_IT,
                   SDF_OCTREE_NODE_INTERSECT_BBOX};

  preset_names[0] = {"no_bvh_traversal",
                     "bvh_traversal",
                     "bvh_sphere_tracing",
                     "bvh_analytic",
                     "bvh_newton",
                     "bvh_interval_tracing",
                     "bvh_nodes"};

  presets_ob[1] = {SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT};

  presets_oi[1] = {SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                   SDF_OCTREE_NODE_INTERSECT_ST, 
                   SDF_OCTREE_NODE_INTERSECT_ANALYTIC, 
                   SDF_OCTREE_NODE_INTERSECT_NEWTON,
                   SDF_OCTREE_NODE_INTERSECT_IT,
                   SDF_OCTREE_NODE_INTERSECT_BBOX};

  preset_names[1] = {"bvh_traversal",
                     "bvh_sphere_tracing",
                     "bvh_analytic",
                     "bvh_newton",
                     "bvh_interval_tracing",
                     "bvh_nodes"};

  presets_ob[2] = {SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT, 
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT,
                   SDF_OCTREE_BLAS_DEFAULT};

  presets_oi[2] = {SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                   SDF_OCTREE_NODE_INTERSECT_ST, 
                   SDF_OCTREE_NODE_INTERSECT_ANALYTIC, 
                   SDF_OCTREE_NODE_INTERSECT_NEWTON,
                   SDF_OCTREE_NODE_INTERSECT_IT,
                   SDF_OCTREE_NODE_INTERSECT_BBOX};

  preset_names[2] = {"bvh_traversal",
                     "bvh_sphere_tracing",
                     "bvh_analytic",
                     "bvh_newton",
                     "bvh_interval_tracing",
                     "bvh_nodes"};

  presets_ob[3] = {SDF_OCTREE_BLAS_DEFAULT};
  presets_oi[3] = {SDF_OCTREE_NODE_INTERSECT_DEFAULT};
  preset_names[3] = {"default"};

  assert(scene_names.size() >= scene_paths.size());
  assert(render_modes.size() >= render_names.size());

  assert(AS_types.size() >= AS_names.size());
  assert(AS_types.size() >= presets_ob.size());
  assert(AS_types.size() >= presets_oi.size());
  assert(AS_types.size() >= preset_names.size());

  for (int scene_n = 0; scene_n < scene_paths.size(); scene_n++)
  {
    auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path+scene_paths[scene_n]).c_str());

    float3 mb1,mb2, ma1,ma2;
    cmesh4::get_bbox(mesh, &mb1, &mb2);
    cmesh4::rescale_mesh(mesh, float3(-0.9,-0.9,-0.9), float3(0.9,0.9,0.9));
    cmesh4::get_bbox(mesh, &ma1, &ma2);
    MeshBVH mesh_bvh;
    mesh_bvh.init(mesh);

    SparseOctreeBuilder builder;

    std::vector<SdfFrameOctreeNode> frame_nodes;
    std::vector<SdfSVSNode> svs_nodes;
    std::vector<SdfSBSNode> sbs_nodes;
    std::vector<uint32_t>   sbs_data;

    builder.construct([&mesh_bvh](const float3 &p) { return mesh_bvh.get_signed_distance(p); }, settings);
    builder.convert_to_frame_octree(frame_nodes);
    builder.convert_to_sparse_voxel_set(svs_nodes);
    builder.convert_to_sparse_brick_set(header, sbs_nodes, sbs_data);

    LiteImage::Image2D<uint32_t> image(W, H);

    for (int rm=0; rm<render_names.size(); rm++)
    {
      for (int as_n=0; as_n<AS_types.size(); as_n++)
      {
        for (int i=0; i<presets_ob[as_n].size(); i++)
        {
          MultiRenderPreset preset = getDefaultPreset();
          preset.mode = render_modes[rm];
          preset.sdf_frame_octree_blas = presets_ob[as_n][i];
          preset.sdf_frame_octree_intersect = presets_oi[as_n][i];

          auto pRender = CreateMultiRenderer("GPU");
          pRender->SetPreset(preset);
          if (AS_types[as_n] == TYPE_SDF_FRAME_OCTREE)
            pRender->SetScene({(unsigned)frame_nodes.size(), frame_nodes.data()});
          else if (AS_types[as_n] == TYPE_SDF_SVS) 
            pRender->SetScene({(unsigned)svs_nodes.size(), svs_nodes.data()});
          else if (AS_types[as_n] == TYPE_MESH_TRIANGLE) 
            pRender->SetScene(mesh);
          else if (AS_types[as_n] == TYPE_SDF_SBS)
            pRender->SetScene({header, (unsigned)sbs_nodes.size(), sbs_nodes.data(), (unsigned)sbs_data.size(), sbs_data.data()});

          double sum_ms[4] = {0,0,0,0};
          double min_ms[4] = {1e6,1e6,1e6,1e6};
          double max_ms[4] = {0,0,0,0};
          render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset, pass_size);
          for (int iter = 0; iter<iters; iter++)
          {
            float timings[4] = {0,0,0,0};
            
            auto t1 = std::chrono::steady_clock::now();
            pRender->Render(image.data(), image.width(), image.height(), "color", pass_size); 
            pRender->GetExecutionTime("CastRaySingleBlock", timings);
            auto t2 = std::chrono::steady_clock::now();
            
            float time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
            //printf("%s rendered in %.1f ms. %d kRays/s\n", "SDF Framed Octree", time_ms, (int)((W * H) / time_ms));
            //printf("CastRaySingleBlock took %.1f ms\n", timings[0]);

            if (iter == 0 && save_images)
              LiteImage::SaveImage<uint32_t>(("saves/benchmark_"+scene_names[scene_n]+"_"+render_names[rm]+"_"+AS_names[as_n]+"_"+preset_names[as_n][i]+".bmp").c_str(), image); 
            for (int i=0;i<4;i++)
            {
              min_ms[i] = std::min(min_ms[i], (double)timings[i]);
              max_ms[i] = std::max(max_ms[i], (double)timings[i]);
              sum_ms[i] += timings[i];
            }
          }

          results.emplace_back();
          results.back().scene_name = scene_names[scene_n];
          results.back().render_name = render_names[rm];
          results.back().as_name = AS_names[as_n];
          results.back().preset_name = preset_names[as_n][i];
          results.back().iters = iters;
          results.back().render_average_time_ms = float4(sum_ms[0], sum_ms[1], sum_ms[2], sum_ms[3])/(iters*pass_size);
          results.back().render_min_time_ms = float4(min_ms[0], min_ms[1], min_ms[2], min_ms[3])/pass_size;
        }
      }
    }
  }

  for (auto &res : results)
  {
    printf("[%10s + %10s + %20s + %20s] min:%6.2f + %5.2f, av:%6.2f + %5.2f ms/frame \n", 
           res.scene_name.c_str(), res.render_name.c_str(), res.as_name.c_str(), res.preset_name.c_str(), 
           res.render_min_time_ms.x,
           res.render_min_time_ms.y + res.render_min_time_ms.z + res.render_min_time_ms.w,
           res.render_average_time_ms.x,
           res.render_average_time_ms.y + res.render_average_time_ms.z + res.render_average_time_ms.w);
  }
}
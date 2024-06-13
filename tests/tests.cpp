#include "tests.h"
#include "../IRenderer.h"
#include "../Renderer/eye_ray.h"
#include "../utils/mesh_bvh.h"
#include "../utils/mesh.h"
#include "LiteScene/hydraxml.h"
#include "LiteMath/Image2d.h"
#include "../NeuralRT/NeuralRT.h"
#include "../utils/hp_octree.h"
#include "../utils/sdf_converter.h"
#include "../utils/sparse_octree_2.h"
#include "../utils/marching_cubes.h"
#include "../utils/sdf_smoother.h"

#include <functional>
#include <cassert>
#include <chrono>

std::string scenes_folder_path = "./";

void render(LiteImage::Image2D<uint32_t> &image, std::shared_ptr<MultiRenderer> pRender, 
            float3 pos, float3 target, float3 up, 
            MultiRenderPreset preset, int a_passNum = 1)
{
  float fov_degrees = 60;
  float z_near = 0.1f;
  float z_far = 100.0f;
  float aspect   = 1.0f;
  auto proj      = LiteMath::perspectiveMatrix(fov_degrees, aspect, z_near, z_far);
  auto worldView = LiteMath::lookAt(pos, target, up);

  pRender->Render(image.data(), image.width(), image.height(), worldView, proj, preset, a_passNum);
}

float PSNR(const LiteImage::Image2D<uint32_t> &image_1, const LiteImage::Image2D<uint32_t> &image_2)
{
  assert(image_1.vector().size() == image_2.vector().size());
  unsigned sz = image_1.vector().size();
  double sum = 0.0;
  for (int i=0;i<sz;i++)
  {
    unsigned r1 = (image_1.vector()[i] & 0x000000FF);
    unsigned g1 = (image_1.vector()[i] & 0x0000FF00) >> 8;
    unsigned b1 = (image_1.vector()[i] & 0x00FF0000) >> 16;
    unsigned r2 = (image_2.vector()[i] & 0x000000FF);
    unsigned g2 = (image_2.vector()[i] & 0x0000FF00) >> 8;
    unsigned b2 = (image_2.vector()[i] & 0x00FF0000) >> 16;
    sum += ((r1-r2)*(r1-r2)+(g1-g2)*(g1-g2)+(b1-b2)*(b1-b2)) / (3.0f*255.0f*255.0f);
  }
  float mse = sum / sz;

  return -10*log10(std::max<double>(1e-10, mse));
}

void litert_test_1_framed_octree()
{
    auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path+"scenes/01_simple_scenes/data/teapot.vsgf").c_str());

    float3 mb1,mb2, ma1,ma2;
    cmesh4::get_bbox(mesh, &mb1, &mb2);
    cmesh4::rescale_mesh(mesh, float3(-0.9,-0.9,-0.9), float3(0.9,0.9,0.9));
    cmesh4::get_bbox(mesh, &ma1, &ma2);

    printf("total triangles %d\n", (int)mesh.TrianglesNum());
    printf("bbox [(%f %f %f)-(%f %f %f)] to [(%f %f %f)-(%f %f %f)]\n",
           mb1.x, mb1.y, mb1.z, mb2.x, mb2.y, mb2.z, ma1.x, ma1.y, ma1.z, ma2.x, ma2.y, ma2.z);

    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 9);
    std::vector<SdfFrameOctreeNode> frame_nodes = sdf_converter::create_sdf_frame_octree(settings, mesh);

    unsigned W = 2048, H = 2048;
    LiteImage::Image2D<uint32_t> image(W, H);
    float timings[4] = {0,0,0,0};

    std::vector<unsigned> presets_ob = {SDF_OCTREE_BLAS_NO, SDF_OCTREE_BLAS_DEFAULT,
                                        SDF_OCTREE_BLAS_DEFAULT, SDF_OCTREE_BLAS_DEFAULT,
                                        SDF_OCTREE_BLAS_DEFAULT, SDF_OCTREE_BLAS_DEFAULT};

    std::vector<unsigned> presets_oi = {SDF_OCTREE_NODE_INTERSECT_DEFAULT, SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                                        SDF_OCTREE_NODE_INTERSECT_ST, SDF_OCTREE_NODE_INTERSECT_ANALYTIC, 
                                        SDF_OCTREE_NODE_INTERSECT_NEWTON, SDF_OCTREE_NODE_INTERSECT_BBOX};

    std::vector<std::string> names = {"no_bvh_traversal", "bvh_traversal", "bvh_sphere_tracing", "bvh_analytic", "bvh_newton", "bvh_bboxes"};

    for (int i=0; i<presets_ob.size(); i++)
    {
      MultiRenderPreset preset = getDefaultPreset();
      preset.mode = MULTI_RENDER_MODE_PHONG;
      preset.sdf_frame_octree_blas = presets_ob[i];
      preset.sdf_frame_octree_intersect = presets_oi[i];

      auto pRender = CreateMultiRenderer("GPU");
      pRender->SetPreset(preset);
      pRender->SetScene(frame_nodes);

  auto t1 = std::chrono::steady_clock::now();
      render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
      pRender->GetExecutionTime("CastRaySingleBlock", timings);
  auto t2 = std::chrono::steady_clock::now();

      float time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
      printf("%s rendered in %.1f ms. %d kRays/s\n", "SDF Framed Octree", time_ms, (int)((W * H) / time_ms));
      printf("CastRaySingleBlock took %.1f ms\n", timings[0]);

      LiteImage::SaveImage<uint32_t>(("saves/test_1_"+names[i]+".bmp").c_str(), image); 
    }
}

void litert_test_2_SVS()
{
    auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path+"scenes/01_simple_scenes/data/teapot.vsgf").c_str());

    float3 mb1,mb2, ma1,ma2;
    cmesh4::get_bbox(mesh, &mb1, &mb2);
    cmesh4::rescale_mesh(mesh, float3(-0.9,-0.9,-0.9), float3(0.9,0.9,0.9));
    cmesh4::get_bbox(mesh, &ma1, &ma2);

    printf("total triangles %d\n", (int)mesh.TrianglesNum());
    printf("bbox [(%f %f %f)-(%f %f %f)] to [(%f %f %f)-(%f %f %f)]\n",
           mb1.x, mb1.y, mb1.z, mb2.x, mb2.y, mb2.z, ma1.x, ma1.y, ma1.z, ma2.x, ma2.y, ma2.z);

    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 9);
    std::vector<SdfSVSNode> frame_nodes = sdf_converter::create_sdf_SVS(settings, mesh);

    unsigned W = 2048, H = 2048;
    LiteImage::Image2D<uint32_t> image(W, H);
    float timings[4] = {0,0,0,0};

    std::vector<unsigned> presets_ob = {SDF_OCTREE_BLAS_NO, SDF_OCTREE_BLAS_DEFAULT,
                                        SDF_OCTREE_BLAS_DEFAULT, SDF_OCTREE_BLAS_DEFAULT,
                                        SDF_OCTREE_BLAS_DEFAULT, SDF_OCTREE_BLAS_DEFAULT};

    std::vector<unsigned> presets_oi = {SDF_OCTREE_NODE_INTERSECT_DEFAULT, SDF_OCTREE_NODE_INTERSECT_DEFAULT, 
                                        SDF_OCTREE_NODE_INTERSECT_ST, SDF_OCTREE_NODE_INTERSECT_ANALYTIC, 
                                        SDF_OCTREE_NODE_INTERSECT_NEWTON, SDF_OCTREE_NODE_INTERSECT_BBOX};

    std::vector<std::string> names = {"no_bvh_traversal", "bvh_traversal", "bvh_sphere_tracing", "bvh_analytic", "bvh_newton", "bvh_bboxes"};

    for (int i=0; i<presets_ob.size(); i++)
    {
      MultiRenderPreset preset = getDefaultPreset();
      preset.mode = MULTI_RENDER_MODE_PHONG;
      preset.sdf_frame_octree_blas = presets_ob[i];
      preset.sdf_frame_octree_intersect = presets_oi[i];

      auto pRender = CreateMultiRenderer("GPU");
      pRender->SetPreset(preset);
      pRender->SetScene(frame_nodes);

  auto t1 = std::chrono::steady_clock::now();
      render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
      pRender->GetExecutionTime("CastRaySingleBlock", timings);
  auto t2 = std::chrono::steady_clock::now();

      float time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
      printf("%s rendered in %.1f ms. %d kRays/s\n", "SDF Framed Octree", time_ms, (int)((W * H) / time_ms));
      printf("CastRaySingleBlock took %.1f ms\n", timings[0]);

      LiteImage::SaveImage<uint32_t>(("saves/test_2_"+names[i]+".bmp").c_str(), image); 
    }
}

void litert_test_3_SBS_verify()
{
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LINEAR_DEPTH;
  preset.sdf_frame_octree_blas = SDF_OCTREE_BLAS_DEFAULT;
  preset.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_ST;

  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path+"scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::rescale_mesh(mesh, float3(-0.9, -0.9, -0.9), float3(0.9, 0.9, 0.9));

  SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
  SdfSBSHeader header_1_1{1,0,1,2};
  SdfSBSHeader header_1_2{1,0,2,2};
  SdfSBSHeader header_2_1{2,0,1,3};
  SdfSBSHeader header_2_2{2,0,2,3};

  SdfSBS sbs_1_1 = sdf_converter::create_sdf_SBS(settings, header_1_1, mesh);
  SdfSBS sbs_1_2 = sdf_converter::create_sdf_SBS(settings, header_1_2, mesh);
  SdfSBS sbs_2_1 = sdf_converter::create_sdf_SBS(settings, header_2_1, mesh);
  SdfSBS sbs_2_2 = sdf_converter::create_sdf_SBS(settings, header_2_2, mesh);

  std::vector<SdfSVSNode> svs_nodes = sdf_converter::create_sdf_SVS(settings, mesh);

  unsigned W = 1024, H = 1024;
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);
  LiteImage::Image2D<uint32_t> svs_image(W, H);

  printf("TEST 3. SVS and SBS correctness\n");
  {
    auto pRender = CreateMultiRenderer("CPU");
    pRender->SetPreset(preset);
    pRender->SetScene(mesh);
    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_reference.bmp", image); 
    ref_image = image;
  }
  {
    auto pRender = CreateMultiRenderer("CPU");
    pRender->SetPreset(preset);
    pRender->SetScene(svs_nodes);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SVS.bmp", image); 
    svs_image = image;

    float psnr = PSNR(ref_image, image);

    printf("  3.1. %-64s", "[CPU] SVS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);
  }
  {
    auto pRender = CreateMultiRenderer("CPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs_1_1);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SBS_1_1.bmp", image); 

    float psnr = PSNR(ref_image, image);
    printf("  3.2. %-64s", "[CPU] 1-voxel,1-byte SBS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);

    float svs_psnr = PSNR(svs_image, image);
    printf("  3.3. %-64s", "[CPU] 1-voxel,1-byte SBS matches SVS");
    if (svs_psnr >= 40)
      printf("passed\n");
    else
      printf("FAILED, psnr = %f\n", svs_psnr);
  }
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs_1_1);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SBS_1_1.bmp", image); 

    float psnr = PSNR(ref_image, image);
    printf("  3.4. %-64s", "1-voxel,1-byte SBS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);

    float svs_psnr = PSNR(svs_image, image);
    printf("  3.5. %-64s", "1-voxel,1-byte SBS matches SVS");
    if (svs_psnr >= 40)
      printf("passed\n");
    else
      printf("FAILED, psnr = %f\n", svs_psnr);
  }
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs_1_2);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SBS_1_2.bmp", image); 

    float psnr = PSNR(ref_image, image);
    printf("  3.6. %-64s", "1-voxel,2-byte SBS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);
  }
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs_2_1);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SBS_2_1.bmp", image); 

    float psnr = PSNR(ref_image, image);
    printf("  3.7. %-64s", "8-voxel,1-byte SBS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);
  }
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs_2_2);

    render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_3_SBS_2_2.bmp", image); 

    float psnr = PSNR(ref_image, image);
    printf("  3.8. %-64s", "8-voxel,2-byte SBS and mesh PSNR > 40 ");
    if (psnr >= 40)
      printf("passed    (%.2f)\n", psnr);
    else
      printf("FAILED, psnr = %f\n", psnr);
  }
}

void litert_test_4_hydra_scene()
{
  //create renderers for SDF scene and mesh scene
  const char *scene_name = "scenes/01_simple_scenes/instanced_objects.xml";
  //const char *scene_name = "large_scenes/02_casual_effects/dragon/change_00000.xml";
  //const char *scene_name = "scenes/01_simple_scenes/bunny_cornell.xml";
  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_frame_octree_blas = SDF_OCTREE_BLAS_DEFAULT;
  preset.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_ANALYTIC;
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("GPU");
  pRenderRef->SetPreset(preset);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->LoadSceneHydra((scenes_folder_path+scene_name).c_str(), TYPE_SDF_SVS, 
                          SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9));

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  //render(image, pRender, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
  //render(ref_image, pRenderRef, float3(0,0,3), float3(0,0,0), float3(0,1,0), preset);
  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  pRenderRef->Render(ref_image.data(), ref_image.width(), ref_image.height(), m1, m2, preset);

  LiteImage::SaveImage<uint32_t>("saves/test_4_res.bmp", image); 
  LiteImage::SaveImage<uint32_t>("saves/test_4_ref.bmp", ref_image);

  float psnr = PSNR(ref_image, image);
  printf("TEST 4. Rendering Hydra scene\n");
  printf("  4.1. %-64s", "mesh and SDF PSNR > 30 ");
  if (psnr >= 30)
    printf("passed    (%.2f)\n", psnr);
  else
    printf("FAILED, psnr = %f\n", psnr);
}

void litert_test_5_interval_tracing()
{
  //create renderers for SDF scene and mesh scene
  //const char *scene_name = "large_scenes/02_casual_effects/dragon/change_00000.xml";
  const char *scene_name = "scenes/01_simple_scenes/instanced_objects.xml";
  //const char *scene_name = "large_scenes/02_casual_effects/dragon/change_00000.xml";
  //const char *scene_name = "scenes/01_simple_scenes/bunny_cornell.xml";
  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset_ref = getDefaultPreset();
  MultiRenderPreset preset_1 = getDefaultPreset();
  preset_1.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_ST;
  MultiRenderPreset preset_2 = getDefaultPreset();
  preset_2.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_IT;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("GPU");
  pRenderRef->SetPreset(preset_ref);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset_1);
  pRender->SetViewport(0,0,W,H);
  pRender->LoadSceneHydra((scenes_folder_path+scene_name).c_str(), TYPE_SDF_SVS,
                          SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9));

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  pRender->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset_1);
  pRender->Render(image_2.data(), image_2.width(), image_2.height(), m1, m2, preset_2);
  pRenderRef->Render(ref_image.data(), ref_image.width(), ref_image.height(), m1, m2, preset_ref);

  LiteImage::SaveImage<uint32_t>("saves/test_5_ST.bmp", image_1); 
  LiteImage::SaveImage<uint32_t>("saves/test_5_IT.bmp", image_2); 
  LiteImage::SaveImage<uint32_t>("saves/test_5_ref.bmp", ref_image);

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(ref_image, image_2);
  printf("TEST 5. Interval tracing\n");
  printf("  5.1. %-64s", "mesh and SDF PSNR > 30 ");
  if (psnr_2 >= 30)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
  printf("  5.2. %-64s", "Interval and Sphere tracing PSNR > 45 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);
}

void litert_test_6_faster_bvh_build()
{
  //create renderers for SDF scene and mesh scene
  //const char *scene_name = "large_scenes/02_casual_effects/dragon/change_00000.xml";
  const char *scene_name = "scenes/01_simple_scenes/bunny.xml";
  //const char *scene_name = "large_scenes/02_casual_effects/dragon/change_00000.xml";
  //const char *scene_name = "scenes/01_simple_scenes/bunny_cornell.xml";
  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset_ref = getDefaultPreset();
  MultiRenderPreset preset_1 = getDefaultPreset();
  preset_1.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_NEWTON;
  MultiRenderPreset preset_2 = getDefaultPreset();
  preset_2.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_NEWTON;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("GPU");
  pRenderRef->SetPreset(preset_ref);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto pRender_1 = CreateMultiRenderer("GPU");
  pRender_1->SetPreset(preset_1);
  pRender_1->SetViewport(0,0,W,H);
auto t1 = std::chrono::steady_clock::now();
  pRender_1->LoadSceneHydra((scenes_folder_path+scene_name).c_str(), TYPE_SDF_SVS,
                            SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9));
auto t2 = std::chrono::steady_clock::now();
  float time_1 = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

  auto pRender_2 = CreateMultiRenderer("GPU");
  pRender_2->SetPreset(preset_2);
  pRender_2->SetViewport(0,0,W,H);
auto t3 = std::chrono::steady_clock::now();
  pRender_2->LoadSceneHydra((scenes_folder_path+scene_name).c_str(), TYPE_SDF_SVS,
                            SparseOctreeSettings(SparseOctreeBuildType::MESH_TLO, 9));
auto t4 = std::chrono::steady_clock::now();
  float time_2 = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count();

  auto m1 = pRender_1->getWorldView();
  auto m2 = pRender_1->getProj();

  pRender_1->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset_1);
  pRender_2->Render(image_2.data(), image_2.width(), image_2.height(), m1, m2, preset_2);
  pRenderRef->Render(ref_image.data(), ref_image.width(), ref_image.height(), m1, m2, preset_ref);

  LiteImage::SaveImage<uint32_t>("saves/test_6_default.bmp", image_1); 
  LiteImage::SaveImage<uint32_t>("saves/test_6_mesh_tlo.bmp", image_2); 
  LiteImage::SaveImage<uint32_t>("saves/test_6_ref.bmp", ref_image);

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(ref_image, image_2);
  printf("TEST 6. MESH_TLO SDF BVH build. default %.1f s, mesh TLO %.1f s\n", time_1/1000.0f, time_2/1000.0f);
  printf("  6.1. %-64s", "mesh and SDF PSNR > 30 ");
  if (psnr_2 >= 30)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
  printf("  6.2. %-64s", "DEFAULT and MESH_TLO PSNR > 45 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);
}

void test_7_neural_SDF()
{
  const char *scene_name = "scenes/02_sdf_scenes/sdf_neural.xml"; 
  unsigned W = 1024, H = 1024;

  MultiRenderPreset preset_1 = getDefaultPreset();
  preset_1.mode = MULTI_RENDER_MODE_LINEAR_DEPTH;

  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);
  LiteImage::Image2D<uint32_t> image_4(W, H);
  LiteImage::Image2D<uint32_t> image_5(W, H);
  LiteImage::Image2D<uint32_t> image_6(W, H);

  auto pRender_1 = CreateMultiRenderer("GPU");
  pRender_1->SetPreset(preset_1);
  pRender_1->SetViewport(0,0,W,H);
  pRender_1->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  ISdfSceneFunction *sdf_func = dynamic_cast<ISdfSceneFunction*>(pRender_1->GetAccelStruct().get());
  SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
  std::vector<SdfSVSNode> frame_nodes = sdf_converter::create_sdf_SVS(settings, 
                                          [&sdf_func](const float3 &p) -> float { return sdf_func->eval_distance(p); });

  auto pRender_2 = CreateMultiRenderer("GPU");
  pRender_2->SetPreset(preset_1);
  pRender_2->SetScene(frame_nodes);

  std::shared_ptr<NeuralRT> neuralRT1 = CreateNeuralRT("CPU");
  std::shared_ptr<NeuralRT> neuralRT2 = CreateNeuralRT("GPU");
  BVHRT *rt = static_cast<BVHRT*>(pRender_1->GetAccelStruct().get());
  neuralRT1->AddGeom_NeuralSdf(rt->m_SdfNeuralProperties[0], rt->m_SdfParameters.data());
  neuralRT2->AddGeom_NeuralSdf(rt->m_SdfNeuralProperties[0], rt->m_SdfParameters.data());

  auto m1 = pRender_1->getWorldView();
  auto m2 = pRender_1->getProj();

  float timings[5][4];

  pRender_1->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset_1);
  pRender_1->GetExecutionTime("CastRaySingleBlock", timings[0]);

  neuralRT1->Render(image_2.data(), image_2.width(), image_2.height(), m1, m2);

  neuralRT2->Render(image_3.data(), image_3.width(), image_3.height(), m1, m2, NEURALRT_RENDER_SIMPLE);
  neuralRT2->GetExecutionTime("Render_internal", timings[1]);
  neuralRT2->Render(image_4.data(), image_4.width(), image_4.height(), m1, m2, NEURALRT_RENDER_BLOCKED);
  neuralRT2->GetExecutionTime("Render_internal", timings[2]);

  neuralRT2->Render(image_5.data(), image_5.width(), image_5.height(), m1, m2, NEURALRT_RENDER_COOP_MATRICES);
  neuralRT2->GetExecutionTime("Render_internal", timings[3]);

  pRender_2->Render(image_6.data(), image_6.width(), image_6.height(), m1, m2, preset_1);
  pRender_2->GetExecutionTime("CastRaySingleBlock", timings[4]);

  LiteImage::SaveImage<uint32_t>("saves/test_7_default.bmp", image_1); 
  LiteImage::SaveImage<uint32_t>("saves/test_7_NeuralRT_CPU.bmp", image_2); 
  LiteImage::SaveImage<uint32_t>("saves/test_7_NeuralRT_GPU_default.bmp", image_3); 
  LiteImage::SaveImage<uint32_t>("saves/test_7_NeuralRT_GPU_blocked.bmp", image_4); 
  LiteImage::SaveImage<uint32_t>("saves/test_7_NeuralRT_GPU_coop_matrices.bmp", image_5); 
  LiteImage::SaveImage<uint32_t>("saves/test_7_octree.bmp", image_6); 

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(image_1, image_3);
  float psnr_3 = PSNR(image_1, image_4);
  float psnr_4 = PSNR(image_1, image_5);
  float psnr_5 = PSNR(image_1, image_6);
  printf("TEST 7. NEURAL SDF rendering\n");
  printf("  7.1. %-64s", "default and NeuralRT CPU PSNR > 45 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);

  printf("  7.2. %-64s", "default and NeuralRT GPU default PSNR > 45 ");
  if (psnr_2 >= 45)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);

  printf("  7.3. %-64s", "default and NeuralRT GPU blocked PSNR > 45 ");
  if (psnr_3 >= 45)
    printf("passed    (%.2f)\n", psnr_3);
  else
    printf("FAILED, psnr = %f\n", psnr_3);

  printf("  7.4. %-64s", "default and NeuralRT GPU coop matrices PSNR > 45 ");
  if (psnr_4 >= 45)
    printf("passed    (%.2f)\n", psnr_4);
  else
    printf("FAILED, psnr = %f\n", psnr_4);

  printf("  7.5. %-64s", "default and SDF octree PSNR > 25 ");
  if (psnr_5 >= 25)
    printf("passed    (%.2f)\n", psnr_5);
  else
    printf("FAILED, psnr = %f\n", psnr_5);

  printf("timings: reference = %f; default = %f; blocked = %f; coop matrices = %f; octree = %f\n", 
         timings[0][0], timings[1][0], timings[2][0], timings[3][0], timings[4][0]);
}

void litert_test_8_SDF_grid()
{
  //create renderers for SDF scene and mesh scene
  const char *scene_name = "scenes/01_simple_scenes/teapot.xml";
  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_frame_octree_blas = SDF_OCTREE_BLAS_DEFAULT;
  preset.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_ANALYTIC;
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("GPU");
  pRenderRef->SetPreset(preset);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->LoadSceneHydra((scenes_folder_path+scene_name).c_str(), TYPE_SDF_GRID, 
                          SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 7));

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  pRenderRef->Render(ref_image.data(), ref_image.width(), ref_image.height(), m1, m2, preset);

  LiteImage::SaveImage<uint32_t>("saves/test_8_res.bmp", image); 
  LiteImage::SaveImage<uint32_t>("saves/test_8_ref.bmp", ref_image);

  float psnr = PSNR(ref_image, image);
  printf("TEST 8. Rendering Hydra scene\n");
  printf("  8.1. %-64s", "mesh and SDF grid PSNR > 30 ");
  if (psnr >= 30)
    printf("passed    (%.2f)\n", psnr);
  else
    printf("FAILED, psnr = %f\n", psnr);
}

void litert_test_9_mesh()
{
  //create renderers for SDF scene and mesh scene
  const char *scene_name = "scenes/01_simple_scenes/teapot.xml";
  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset = getDefaultPreset();
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("CPU");
  pRenderRef->SetPreset(preset);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->LoadSceneHydra((scenes_folder_path+scene_name).c_str());

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  pRenderRef->Render(ref_image.data(), ref_image.width(), ref_image.height(), m1, m2, preset);

  LiteImage::SaveImage<uint32_t>("saves/test_9_res.bmp", image); 
  LiteImage::SaveImage<uint32_t>("saves/test_9_ref.bmp", ref_image);

  float psnr = PSNR(ref_image, image);
  printf("TEST 9. Rendering simple mesh\n");
  printf("  9.1. %-64s", "CPU and GPU render PSNR > 45 ");
  if (psnr >= 45)
    printf("passed    (%.2f)\n", psnr);
  else
    printf("FAILED, psnr = %f\n", psnr);
}

// save and load octrees of all types
void litert_test_10_save_load()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);
  MeshBVH mesh_bvh;
  mesh_bvh.init(mesh);

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 7);

    std::vector<SdfOctreeNode> octree_nodes = sdf_converter::create_sdf_octree(settings, mesh);
    std::vector<SdfFrameOctreeNode> frame_nodes = sdf_converter::create_sdf_frame_octree(settings, mesh);
    std::vector<SdfSVSNode> svs_nodes = sdf_converter::create_sdf_SVS(settings, mesh);
    SdfSBS sbs = sdf_converter::create_sdf_SBS(settings, SdfSBSHeader{1, 0, 1, 2}, mesh);

    save_sdf_octree(octree_nodes, "saves/test_10_octree.bin");
    save_sdf_frame_octree(frame_nodes, "saves/test_10_frame_octree.bin");
    save_sdf_SVS(svs_nodes, "saves/test_10_svs.bin");
    save_sdf_SBS(sbs, "saves/test_10_sbs.bin");
  }

  std::vector<SdfOctreeNode> octree_nodes;
  std::vector<SdfFrameOctreeNode> frame_nodes;
  std::vector<SdfSVSNode> svs_nodes;
  SdfSBS sbs;

  load_sdf_octree(octree_nodes, "saves/test_10_octree.bin");
  load_sdf_frame_octree(frame_nodes, "saves/test_10_frame_octree.bin");
  load_sdf_SVS(svs_nodes, "saves/test_10_svs.bin");
  load_sdf_SBS(sbs, "saves/test_10_sbs.bin");

  unsigned W = 1024, H = 1024;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_octree_sampler = SDF_OCTREE_SAMPLER_MIPSKIP_3X3;

  LiteImage::Image2D<uint32_t> image_ref(W, H);
  auto pRender_ref = CreateMultiRenderer("GPU");
  pRender_ref->SetPreset(preset);
  pRender_ref->SetScene(mesh);
  render(image_ref, pRender_ref, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_ref.bmp", image_ref);

  LiteImage::Image2D<uint32_t> image_1(W, H);
  auto pRender_1 = CreateMultiRenderer("GPU");
  pRender_1->SetPreset(preset);
  pRender_1->SetScene(octree_nodes);
  render(image_1, pRender_1, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_octree.bmp", image_1);

  LiteImage::Image2D<uint32_t> image_2(W, H);
  auto pRender_2 = CreateMultiRenderer("GPU");
  pRender_2->SetPreset(preset);
  pRender_2->SetScene(frame_nodes);
  render(image_2, pRender_2, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_frame_octree.bmp", image_2);

  LiteImage::Image2D<uint32_t> image_3(W, H);
  auto pRender_3 = CreateMultiRenderer("GPU");
  pRender_3->SetPreset(preset);
  pRender_3->SetScene(svs_nodes);
  render(image_3, pRender_3, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_svs.bmp", image_3);

  LiteImage::Image2D<uint32_t> image_4(W, H);
  auto pRender_4 = CreateMultiRenderer("GPU");
  pRender_4->SetPreset(preset);
  pRender_4->SetScene(sbs);
  render(image_4, pRender_4, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_sbs.bmp", image_4);

  LiteImage::Image2D<uint32_t> image_5(W, H);
  auto pRender_5 = CreateMultiRenderer("GPU");
  pRender_5->SetPreset(preset);
  pRender_5->LoadSceneHydra("scenes/02_sdf_scenes/test_10.xml");
  render(image_5, pRender_5, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  LiteImage::SaveImage<uint32_t>("saves/test_10_hydra_scene.bmp", image_5);

  float psnr_1 = PSNR(image_ref, image_1);
  float psnr_2 = PSNR(image_ref, image_2);
  float psnr_3 = PSNR(image_ref, image_3);
  float psnr_4 = PSNR(image_ref, image_4);
  float psnr_5 = PSNR(image_ref, image_5);

  printf(" 10.1. %-64s", "SDF Octree ");
  if (psnr_1 >= 25)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);

  printf(" 10.2. %-64s", "SDF Framed Octree ");
  if (psnr_2 >= 25)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);

  printf(" 10.3. %-64s", "SDF Sparse Voxel Set ");
  if (psnr_3 >= 25)
    printf("passed    (%.2f)\n", psnr_3);
  else
    printf("FAILED, psnr = %f\n", psnr_3);

  printf(" 10.4. %-64s", "SDF Sparse Brick Set ");
  if (psnr_4 >= 25)
    printf("passed    (%.2f)\n", psnr_4);
  else
    printf("FAILED, psnr = %f\n", psnr_4);
  
  printf(" 10.5. %-64s", "SDF Scene loaded from Hydra scene");
  if (psnr_5 >= 25)
    printf("passed    (%.2f)\n", psnr_5);
  else
    printf("FAILED, psnr = %f\n", psnr_5);
}

static double urand(double from=0, double to=1)
{
  return ((double)rand() / RAND_MAX) * (to - from) + from;
}
void litert_test_11_hp_octree_legacy()
{
  HPOctreeBuilder builder;
  builder.readLegacy(scenes_folder_path+"scenes/02_sdf_scenes/sphere_hp.bin");

  double diff = 0.0;
  int cnt = 10000;
  for (int i = 0; i < cnt; i++)
  {
    float3 rnd_pos = float3(urand(-0.5f, 0.5f), urand(-0.5f, 0.5f), urand(-0.5f, 0.5f));
    float dist_real = length(rnd_pos) - 0.5f;
    float dist = builder.QueryLegacy(rnd_pos);
    diff += abs(dist_real - dist);
    //printf("%.4f - %.4f = %.4f\n", dist_real, dist, dist_real - dist);
  }
  diff /= cnt;
  printf("TEST 11. hp-Octree legacy\n");
  printf(" 11.1. %-64s", "reading from Legacy format ");
  if (diff < 1e-4f)
    printf("passed    (%f)\n", diff);
  else
    printf("FAILED, diff = %f\n", diff);
}

void litert_test_12_hp_octree_render()
{
  HPOctreeBuilder builder;
  builder.readLegacy(scenes_folder_path+"scenes/02_sdf_scenes/sphere_hp.bin");

  unsigned W = 1024, H = 1024;

  MultiRenderPreset preset = getDefaultPreset();
  //preset.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_BBOX;
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("CPU");
  pRenderRef->SetPreset(preset);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->SetScene(SdfHPOctreeView(builder.octree.nodes, builder.octree.data));
  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->SetScene(SdfHPOctreeView(builder.octree.nodes, builder.octree.data));

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  render(ref_image, pRenderRef, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  render(image, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);

  LiteImage::SaveImage<uint32_t>("saves/test_12_res.bmp", image); 
  LiteImage::SaveImage<uint32_t>("saves/test_12_ref.bmp", ref_image);

  float psnr = PSNR(ref_image, image);
  printf("TEST 12. Rendering hp-adaptive SDF octree\n");
  printf("  12.1. %-64s", "CPU and GPU render PSNR > 45 ");
  if (psnr >= 45)
    printf("passed    (%.2f)\n", psnr);
  else
    printf("FAILED, psnr = %f\n", psnr);
}

void litert_test_13_hp_octree_build()
{
#ifdef HP_OCTREE_BUILDER

  HPOctreeBuilder builder;
  HPOctreeBuilder::BuildSettings settings;
  settings.threads = 15;
  settings.target_error = 1e-5f;

  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);
  builder.construct(mesh, settings);

  save_sdf_hp_octree(builder.octree, "saves/test_13_hp_octree.bin");
  SdfHPOctree loaded_octree;
  load_sdf_hp_octree(loaded_octree, "saves/test_13_hp_octree.bin");

  HPOctreeBuilder::BuildSettings settings2;
  settings2.threads = 15;
  settings2.target_error = 1e-5f;
  settings2.nodesLimit = 15000;
  HPOctreeBuilder builder2;
  builder2.construct(mesh, settings2);

  unsigned W = 1024, H = 1024;

  MultiRenderPreset preset = getDefaultPreset();
  //preset.sdf_frame_octree_intersect = SDF_OCTREE_NODE_INTERSECT_BBOX;
  LiteImage::Image2D<uint32_t> image(W, H);
  LiteImage::Image2D<uint32_t> image_l(W, H);
  LiteImage::Image2D<uint32_t> image_limited(W, H);
  LiteImage::Image2D<uint32_t> ref_image(W, H);

  auto pRenderRef = CreateMultiRenderer("CPU");
  pRenderRef->SetPreset(preset);
  pRenderRef->SetViewport(0,0,W,H);
  pRenderRef->SetScene(SdfHPOctreeView(builder.octree.nodes, builder.octree.data));

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->SetScene(SdfHPOctreeView(builder.octree.nodes, builder.octree.data));

  auto pRender_l = CreateMultiRenderer("GPU");
  pRender_l->SetPreset(preset);
  pRender_l->SetViewport(0,0,W,H);
  pRender_l->SetScene(SdfHPOctreeView(loaded_octree.nodes, loaded_octree.data));

  auto pRender_limited = CreateMultiRenderer("GPU");
  pRender_limited->SetPreset(preset);
  pRender_limited->SetViewport(0,0,W,H);
  pRender_limited->SetScene(SdfHPOctreeView(builder2.octree.nodes, builder2.octree.data));

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  render(ref_image, pRenderRef, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  render(image, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  render(image_l, pRender_l, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  render(image_limited, pRender_limited, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);

  LiteImage::SaveImage<uint32_t>("saves/test_13_res.bmp", image); 
  LiteImage::SaveImage<uint32_t>("saves/test_13_ref.bmp", ref_image);
  LiteImage::SaveImage<uint32_t>("saves/test_13_res_l.bmp", image_l);
  LiteImage::SaveImage<uint32_t>("saves/test_13_res_limited.bmp", image_limited);

  float psnr = PSNR(ref_image, image);
  float psnr_l = PSNR(ref_image, image_l);
  float psnr_limited = PSNR(ref_image, image_limited);

  printf("TEST 13. Rendering hp-adaptive SDF octree\n");
  printf("  13.1. %-64s", "CPU and GPU render PSNR > 45 ");
  if (psnr >= 45)
    printf("passed    (%.2f)\n", psnr);
  else
    printf("FAILED, psnr = %f\n", psnr);
  
  printf("  13.2. %-64s", "original and loaded render PSNR > 45 ");
  if (psnr_l >= 45)
    printf("passed    (%.2f)\n", psnr_l);
  else
    printf("FAILED, psnr = %f\n", psnr_l);
  
  printf("  13.3. %-64s", "limited and loaded render PSNR > 30 ");
  if (psnr_limited >= 30)
    printf("passed    (%.2f)\n", psnr_limited);
  else
    printf("FAILED, psnr = %f\n", psnr_limited);
#else
  printf("TEST 13. Skipping hp-adaptive SDF octree test: HP_OCTREE_BUILDER is disabled\n");
#endif
}

void litert_test_14_octree_nodes_removal()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);

  std::vector<SdfOctreeNode> octree_nodes_ref;
  std::vector<SdfOctreeNode> octree_nodes_7;
  std::vector<SdfOctreeNode> octree_nodes_8;
  const unsigned level_6_nodes = 21603;

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 6);
    octree_nodes_ref = sdf_converter::create_sdf_octree(settings, mesh);
    sdf_converter::octree_limit_nodes(octree_nodes_ref, level_6_nodes);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 7);
    octree_nodes_7 = sdf_converter::create_sdf_octree(settings, mesh);
    sdf_converter::octree_limit_nodes(octree_nodes_7, level_6_nodes);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
    octree_nodes_8 = sdf_converter::create_sdf_octree(settings, mesh);
    sdf_converter::octree_limit_nodes(octree_nodes_8, level_6_nodes);
  }

  unsigned W = 1024, H = 1024;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_octree_sampler = SDF_OCTREE_SAMPLER_MIPSKIP_3X3;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);

  {
    auto pRender_1 = CreateMultiRenderer("GPU");
    pRender_1->SetPreset(preset);
    pRender_1->SetScene(octree_nodes_ref);
    render(image_1, pRender_1, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_14_ref.bmp", image_1);
  }

  {
    auto pRender_2 = CreateMultiRenderer("GPU");
    pRender_2->SetPreset(preset);
    pRender_2->SetScene(octree_nodes_7);
    render(image_2, pRender_2, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_14_trimmed_7.bmp", image_2);
  }

  {
    auto pRender_3 = CreateMultiRenderer("GPU");
    pRender_3->SetPreset(preset);
    pRender_3->SetScene(octree_nodes_8);
    render(image_3, pRender_3, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_14_trimmed_8.bmp", image_3);
  }

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(image_1, image_3);

  printf("TEST 14. Octree nodes removal\n");
  printf("  14.1. %-64s", "octrees have the same node count ");
  if (octree_nodes_ref.size() == octree_nodes_7.size() && octree_nodes_ref.size() == octree_nodes_8.size())
    printf("passed\n");
  else
    printf("FAILED, %d, %d, %d\n", (int)octree_nodes_ref.size(), (int)octree_nodes_7.size(), (int)octree_nodes_8.size());
  printf("  14.2. %-64s", "clear level 7 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);

  printf("  14.3. %-64s", "clear levels 7 and 8 ");
  if (psnr_2 >= 45)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
}

void litert_test_15_frame_octree_nodes_removal()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);
  MeshBVH mesh_bvh;
  mesh_bvh.init(mesh);

  std::vector<SdfFrameOctreeNode> octree_nodes_ref;
  std::vector<SdfFrameOctreeNode> octree_nodes_7;
  std::vector<SdfFrameOctreeNode> octree_nodes_8;
  const unsigned level_6_nodes = 21603;

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 6);
    octree_nodes_ref = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(octree_nodes_ref, level_6_nodes, false);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 7);
    octree_nodes_7 = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(octree_nodes_7, level_6_nodes, false);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
    octree_nodes_8 = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(octree_nodes_8, level_6_nodes, false);
  }

  unsigned W = 1024, H = 1024;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_octree_sampler = SDF_OCTREE_SAMPLER_MIPSKIP_3X3;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);

  {
    auto pRender_1 = CreateMultiRenderer("GPU");
    pRender_1->SetPreset(preset);
    pRender_1->SetScene(octree_nodes_ref);
    render(image_1, pRender_1, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_15_ref.bmp", image_1);
  }

  {
    auto pRender_2 = CreateMultiRenderer("GPU");
    pRender_2->SetPreset(preset);
    pRender_2->SetScene(octree_nodes_7);
    render(image_2, pRender_2, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_15_trimmed_7.bmp", image_2);
  }

  {
    auto pRender_3 = CreateMultiRenderer("GPU");
    pRender_3->SetPreset(preset);
    pRender_3->SetScene(octree_nodes_8);
    render(image_3, pRender_3, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_15_trimmed_8.bmp", image_3);
  }

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(image_1, image_3);

  printf("TEST 15. Frame octree nodes removal\n");
  printf("  15.1. %-64s", "octrees have the same node count ");
  if (octree_nodes_ref.size() == octree_nodes_7.size() && octree_nodes_ref.size() == octree_nodes_8.size())
    printf("passed\n");
  else
    printf("FAILED, %d, %d, %d\n", (int)octree_nodes_ref.size(), (int)octree_nodes_7.size(), (int)octree_nodes_8.size());
  printf("  15.2. %-64s", "clear level 7 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);

  printf("  15.3. %-64s", "clear levels 7 and 8 ");
  if (psnr_2 >= 45)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
}

void litert_test_16_SVS_nodes_removal()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);
  MeshBVH mesh_bvh;
  mesh_bvh.init(mesh);

  std::vector<SdfSVSNode> octree_nodes_ref;
  std::vector<SdfSVSNode> octree_nodes_7;
  std::vector<SdfSVSNode> octree_nodes_8;
  const unsigned level_6_nodes = 11215;

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 6);
    std::vector<SdfFrameOctreeNode> nodes = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(nodes, level_6_nodes, true);
    sdf_converter::frame_octree_to_SVS_rec(nodes, octree_nodes_ref, 0, uint3(0,0,0), 1);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 7);
    std::vector<SdfFrameOctreeNode> nodes = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(nodes, level_6_nodes, true);
    sdf_converter::frame_octree_to_SVS_rec(nodes, octree_nodes_7, 0, uint3(0,0,0), 1);
  }

  {
    SparseOctreeSettings settings(SparseOctreeBuildType::DEFAULT, 8);
    std::vector<SdfFrameOctreeNode> nodes = sdf_converter::create_sdf_frame_octree(settings, mesh);
    sdf_converter::frame_octree_limit_nodes(nodes, level_6_nodes, true);
    sdf_converter::frame_octree_to_SVS_rec(nodes, octree_nodes_8, 0, uint3(0,0,0), 1);
  }

  unsigned W = 1024, H = 1024;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;
  preset.sdf_octree_sampler = SDF_OCTREE_SAMPLER_MIPSKIP_3X3;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);

  {
    auto pRender_1 = CreateMultiRenderer("GPU");
    pRender_1->SetPreset(preset);
    pRender_1->SetScene(octree_nodes_ref);
    render(image_1, pRender_1, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_16_ref.bmp", image_1);
  }

  {
    auto pRender_2 = CreateMultiRenderer("GPU");
    pRender_2->SetPreset(preset);
    pRender_2->SetScene(octree_nodes_7);
    render(image_2, pRender_2, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_16_trimmed_7.bmp", image_2);
  }

  {
    auto pRender_3 = CreateMultiRenderer("GPU");
    pRender_3->SetPreset(preset);
    pRender_3->SetScene(octree_nodes_8);
    render(image_3, pRender_3, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_16_trimmed_8.bmp", image_3);
  }

  float psnr_1 = PSNR(image_1, image_2);
  float psnr_2 = PSNR(image_1, image_3);

  printf("TEST 16. SVS nodes removal\n");
  printf("  16.1. %-64s", "octrees have correct node count ");
  if (octree_nodes_ref.size() >= octree_nodes_7.size() && octree_nodes_ref.size() >= octree_nodes_8.size())
    printf("passed\n");
  else
    printf("FAILED, %d, %d, %d\n", (int)octree_nodes_ref.size(), (int)octree_nodes_7.size(), (int)octree_nodes_8.size());
  printf("  16.2. %-64s", "clear level 7 ");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);

  printf("  16.3. %-64s", "clear levels 7 and 8 ");
  if (psnr_2 >= 45)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
}

void litert_test_17_all_types_sanity_check()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/teapot.vsgf").c_str());
  cmesh4::normalize_mesh(mesh);

  unsigned W = 512, H = 512;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_LAMBERT;

  LiteImage::Image2D<uint32_t> image_ref(W, H);
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);
  LiteImage::Image2D<uint32_t> image_4(W, H);
  LiteImage::Image2D<uint32_t> image_5(W, H);
  LiteImage::Image2D<uint32_t> image_6(W, H);
  
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(mesh);
    render(image_ref, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_ref.bmp", image_ref);
  }

  {
    auto grid = sdf_converter::create_sdf_grid(GridSettings(64), mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(grid);
    render(image_1, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_grid.bmp", image_1);    
  }

  {
    auto octree = sdf_converter::create_sdf_octree(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 8, 64*64*64), mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(octree);
    render(image_2, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_octree.bmp", image_2);
  }

  {
    auto octree = sdf_converter::create_sdf_frame_octree(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 8, 64*64*64), mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(octree);
    render(image_3, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_frame_octree.bmp", image_3);
  }

  {
    auto octree = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 8, 64*64*64), mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(octree);
    render(image_4, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_SVS.bmp", image_4);
  }

  {
    SdfSBSHeader header;
    header.brick_size = 2;
    header.brick_pad = 0;
    header.bytes_per_value = 1;

    auto sbs = sdf_converter::create_sdf_SBS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 8, 64*64*64), header, mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(sbs);
    render(image_5, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_SBS.bmp", image_5);
  }

  {
    HPOctreeBuilder::BuildSettings settings;
    settings.target_error = 1e-7f;
    settings.nodesLimit = 7500;
    settings.threads = 15;

    auto hp_octree = sdf_converter::create_sdf_hp_octree(settings, mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetScene(hp_octree);
    render(image_6, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_17_hp_octree.bmp", image_6);
  }

  float psnr_1 = PSNR(image_1, image_ref);
  float psnr_2 = PSNR(image_2, image_ref);
  float psnr_3 = PSNR(image_3, image_ref);
  float psnr_4 = PSNR(image_4, image_ref);
  float psnr_5 = PSNR(image_5, image_ref);
  float psnr_6 = PSNR(image_6, image_ref);

  printf("TEST 17. all types sanity check\n");
  printf("  17.1. %-64s", "SDF grid");
  if (psnr_1 >= 30)
    printf("passed %f\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);
  
  printf("  17.2. %-64s", "SDF octree");
  if (psnr_2 >= 30)
    printf("passed %f\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
  
  printf("  17.3. %-64s", "SDF frame octree");
  if (psnr_3 >= 30)
    printf("passed %f\n", psnr_3);
  else
    printf("FAILED, psnr = %f\n", psnr_3);
  
  printf("  17.4. %-64s", "SDF SVS");
  if (psnr_4 >= 30)
    printf("passed %f\n", psnr_4);
  else
    printf("FAILED, psnr = %f\n", psnr_4);
  
  printf("  17.5. %-64s", "SDF SBS");
  if (psnr_5 >= 30)
    printf("passed %f\n", psnr_5);
  else
    printf("FAILED, psnr = %f\n", psnr_5);
  
  printf("  17.6. %-64s", "SDF hp octree");
  if (psnr_6 >= 30)
    printf("passed %f\n", psnr_6);
  else
    printf("FAILED, psnr = %f\n", psnr_6);
}

void litert_test_18_mesh_normalization()
{
  //create renderers for SDF scene and mesh scene
  cmesh4::SimpleMesh mesh, mesh_filled, mesh_compressed, mesh_n_fixed, mesh_normalized;
  mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "saves/dragon/mesh.vsgf").c_str());
  cmesh4::rescale_mesh(mesh, 0.999f*float3(-1, -1, -1), 0.999f*float3(1, 1, 1));

  printf("mesh size = %d\n", (int)mesh.TrianglesNum());

  unsigned W = 2048, H = 2048;

  MultiRenderPreset preset = getDefaultPreset();

  LiteImage::Image2D<uint32_t> ref_image(W, H);
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);
  LiteImage::Image2D<uint32_t> image_4(W, H);

  LiteImage::Image2D<uint32_t> ref_sdf(W, H);
  LiteImage::Image2D<uint32_t> sdf_1(W, H);
  LiteImage::Image2D<uint32_t> sdf_2(W, H);
  LiteImage::Image2D<uint32_t> sdf_3(W, H);
  LiteImage::Image2D<uint32_t> sdf_4(W, H);

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh);
    render(ref_image, pRender, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_1ref.bmp", ref_image);

    auto sdf = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9), mesh);
    auto pRenderSdf = CreateMultiRenderer("GPU");
    pRenderSdf->SetPreset(preset);
    pRenderSdf->SetViewport(0,0,W,H);
    pRenderSdf->SetScene(sdf);
    render(ref_sdf, pRenderSdf, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_sdf_1ref.bmp", ref_sdf);
  }

  {
    int ind = -1;
    bool fl = false;
    mesh_filled = cmesh4::check_watertight_mesh(mesh, true) ? mesh : cmesh4::removing_holes(mesh, ind, fl);
    mesh_filled = mesh;
    printf("mesh_filled size = %d\n", (int)mesh_filled.TrianglesNum());

    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh_filled);
    render(image_1, pRender, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_2removed_holes.bmp", image_1);


    auto sdf = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9), mesh_filled);
    auto pRenderSdf = CreateMultiRenderer("GPU");
    pRenderSdf->SetPreset(preset);
    pRenderSdf->SetViewport(0,0,W,H);
    pRenderSdf->SetScene(sdf);
    render(sdf_1, pRenderSdf, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_sdf_2removed_holes.bmp", sdf_1);
  }

  {
    mesh_compressed = mesh_filled;
    cmesh4::compress_close_vertices(mesh_compressed, 1e-9f, true);
    printf("mesh_compressed size = %d\n", (int)mesh_compressed.TrianglesNum());

    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh_compressed);
    render(image_2, pRender, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_3compressed.bmp", image_2);


    auto sdf = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9), mesh_compressed);
    auto pRenderSdf = CreateMultiRenderer("GPU");
    pRenderSdf->SetPreset(preset);
    pRenderSdf->SetViewport(0,0,W,H);
    pRenderSdf->SetScene(sdf);
    render(sdf_2, pRenderSdf, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_sdf_3compressed.bmp", sdf_2);
  }

  {
    mesh_n_fixed = mesh_compressed;
    cmesh4::fix_normals(mesh_n_fixed, true);
    printf("mesh_compressed size = %d\n", (int)mesh_n_fixed.TrianglesNum());

    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh_n_fixed);
    render(image_3, pRender, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_4n_fixed.bmp", image_3);


    auto sdf = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 9), mesh_n_fixed);
    auto pRenderSdf = CreateMultiRenderer("GPU");
    pRenderSdf->SetPreset(preset);
    pRenderSdf->SetViewport(0,0,W,H);
    pRenderSdf->SetScene(sdf);
    render(sdf_3, pRenderSdf, float3(2, 0, 2), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_18_sdf_4n_fixed.bmp", sdf_3);
  }

  float psnr_1 = PSNR(ref_image, image_1);
  float psnr_2 = PSNR(ref_image, image_2);
  float psnr_3 = PSNR(ref_image, image_3);

  float psnr_sdf_1 = PSNR(ref_sdf, sdf_1);
  float psnr_sdf_2 = PSNR(ref_sdf, sdf_2);
  float psnr_sdf_3 = PSNR(ref_sdf, sdf_3);

  printf("TEST 18. Mesh normalization\n");

  printf(" 18.1. %-64s", "Removing holes left mesh intact");
  if (psnr_1 >= 45)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1); 

  printf(" 18.2. %-64s", "Removing holes left SDF intact");
  if (psnr_sdf_1 >= 45)
    printf("passed    (%.2f)\n", psnr_sdf_1);
  else
    printf("FAILED, psnr = %f\n", psnr_sdf_1);   

  printf(" 18.1. %-64s", "Removing holes left mesh intact");
  if (psnr_2 >= 45)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2); 

  printf(" 18.2. %-64s", "Removing holes left SDF intact");
  if (psnr_sdf_2 >= 45)
    printf("passed    (%.2f)\n", psnr_sdf_2);
  else
    printf("FAILED, psnr = %f\n", psnr_sdf_2);   

  printf(" 18.1. %-64s", "Removing holes left mesh intact");
  if (psnr_3 >= 45)
    printf("passed    (%.2f)\n", psnr_3);
  else
    printf("FAILED, psnr = %f\n", psnr_3); 

  printf(" 18.2. %-64s", "Removing holes left SDF intact");
  if (psnr_sdf_3 >= 45)
    printf("passed    (%.2f)\n", psnr_sdf_3);
  else
    printf("FAILED, psnr = %f\n", psnr_sdf_3);
}

void litert_test_19_marching_cubes()
{
  printf("TEST 19. Marching cubes\n");

  cmesh4::MultithreadedDensityFunction sdf = [](const float3 &pos, unsigned idx) -> float
  {
    float3 rp = float3(pos.x, pos.y, pos.z);
    const float radius = 0.95f;
    const float max_A = 0.125f;
    float l = sqrt(rp.x * rp.x + rp.z * rp.z);
    float A = max_A*(1.0f - l / radius);
    float c = A*(cos(10*M_PI*l) + 1.1f) - std::abs(rp.y) - 1e-6f;
    return c;
  };
  cmesh4::MarchingCubesSettings settings;
  settings.size = LiteMath::uint3(256, 256, 256);
  settings.min_pos = float3(-1, -1, -1);
  settings.max_pos = float3(1, 1, 1);
  settings.iso_level = 0.0f;

  cmesh4::SimpleMesh mesh = cmesh4::create_mesh_marching_cubes(settings, sdf, 15);

  unsigned W = 4096, H = 4096;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mesh_normal_mode = MESH_NORMAL_MODE_VERTEX;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh);
    render(image_1, pRender, float3(3, 1, 0), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_19_1.bmp", image_1);
  }
}

void litert_test_20_radiance_fields()
{
  printf("TEST 20. Radiance fields\n");

  const char *scene_name = "scenes/02_sdf_scenes/relu_fields.xml";
  unsigned W = 1024, H = 1024;

  MultiRenderPreset preset = getDefaultPreset();
  preset.mode = MULTI_RENDER_MODE_RF;
  LiteImage::Image2D<uint32_t> image(W, H);

  auto pRender = CreateMultiRenderer("GPU");
  pRender->SetPreset(preset);
  pRender->SetViewport(0,0,W,H);
  pRender->LoadScene((scenes_folder_path+scene_name).c_str());

  auto m1 = pRender->getWorldView();
  auto m2 = pRender->getProj();

  preset.mode = MULTI_RENDER_MODE_RF;
  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  LiteImage::SaveImage<uint32_t>("saves/test_20_rf.bmp", image); 

  preset.mode = MULTI_RENDER_MODE_RF_DENSITY;
  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  LiteImage::SaveImage<uint32_t>("saves/test_20_rf_density.bmp", image); 

  preset.mode = MULTI_RENDER_MODE_LINEAR_DEPTH;
  pRender->Render(image.data(), image.width(), image.height(), m1, m2, preset);
  LiteImage::SaveImage<uint32_t>("saves/test_20_depth.bmp", image); 
}

void litert_test_21_rf_to_mesh()
{
  printf("TEST 21. Radiance field to mesh\n");

  const char *scene_name = "scenes/02_sdf_scenes/relu_fields.xml";
  RFScene scene;
  load_rf_scene(scene, "scenes/02_sdf_scenes/model.dat");

  cmesh4::MultithreadedDensityFunction sdf = [&scene](const float3 &pos, unsigned idx) -> float
  {
    if (pos.x < 0.01f || pos.x > 0.99f || pos.y < 0.01f || pos.y > 0.99f || pos.z < 0.01f || pos.z > 0.99f)
      return -1.0f;
    float3 f_idx = scene.size*pos;
    uint3 idx0 = uint3(f_idx);
    float3 dp = f_idx - float3(idx0);
    float values[8];
    for (int i = 0; i < 8; i++)
    {
      uint3 idx = clamp(idx0 + uint3((i & 4) >> 2, (i & 2) >> 1, i & 1), uint3(0u), uint3((unsigned)(scene.size - 1)));
      values[i] = scene.data[CellSize*(idx.z*scene.size*scene.size + idx.y*scene.size + idx.x)];
    }

    //bilinear sampling
    return (1-dp.x)*(1-dp.y)*(1-dp.z)*values[0] + 
           (1-dp.x)*(1-dp.y)*(  dp.z)*values[1] + 
           (1-dp.x)*(  dp.y)*(1-dp.z)*values[2] + 
           (1-dp.x)*(  dp.y)*(  dp.z)*values[3] + 
           (  dp.x)*(1-dp.y)*(1-dp.z)*values[4] + 
           (  dp.x)*(1-dp.y)*(  dp.z)*values[5] + 
           (  dp.x)*(  dp.y)*(1-dp.z)*values[6] + 
           (  dp.x)*(  dp.y)*(  dp.z)*values[7];
  };
  cmesh4::MarchingCubesSettings settings;
  settings.size = LiteMath::uint3(1024, 1024, 1024);
  settings.min_pos = float3(-1, -1, -1);
  settings.max_pos = float3(1, 1, 1);
  settings.iso_level = 0.5f;

  cmesh4::SimpleMesh mesh = cmesh4::create_mesh_marching_cubes(settings, sdf, 15);

  unsigned W = 2048, H = 2048;
  MultiRenderPreset preset = getDefaultPreset();
  preset.mesh_normal_mode = MESH_NORMAL_MODE_VERTEX;
  float4x4 m1, m2;
  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);

  {
    preset = getDefaultPreset();
    preset.mode = MULTI_RENDER_MODE_RF;

    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->LoadScene((scenes_folder_path+scene_name).c_str());
    m1 = pRender->getWorldView();
    m2 = pRender->getProj();

    pRender->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset);
    LiteImage::SaveImage<uint32_t>("saves/test_21_rf.bmp", image_1); 
  }

  {
    preset = getDefaultPreset();
    preset.mesh_normal_mode = MESH_NORMAL_MODE_VERTEX;
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh);
    pRender->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset);
    LiteImage::SaveImage<uint32_t>("saves/test_21_mesh.bmp", image_1);
  }

  {
    auto octree = sdf_converter::create_sdf_SVS(SparseOctreeSettings(SparseOctreeBuildType::DEFAULT, 10), mesh);
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(octree);
    pRender->Render(image_1.data(), image_1.width(), image_1.height(), m1, m2, preset);
    LiteImage::SaveImage<uint32_t>("saves/test_21_SVS.bmp", image_1);
  }
}

float2 get_quality(sdf_converter::MultithreadedDistanceFunction sdf, SdfGridView grid, unsigned points = 100000)
{
  long double sum = 0;
  long double sum_abs = 0;

  auto grid_sdf = get_SdfGridFunction(grid);

  for (unsigned i = 0; i < points; i++)
  {
    float3 p = float3(urand(), urand(), urand())*2.0f - 1.0f;
    float d1 = sdf(p, 0);
    float d2 = grid_sdf->eval_distance(p);

    sum += d1-d2;
    sum_abs += std::abs(d1-d2);
  }

  return float2(sum/points, sum_abs/points);
}

void litert_test_22_sdf_grid_smoothing()
{
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/bunny.vsgf").c_str());
  cmesh4::rescale_mesh(mesh, float3(-0.95, -0.95, -0.95), float3(0.95, 0.95, 0.95));
  unsigned W = 2048, H = 2048;
  MultiRenderPreset preset = getDefaultPreset();

    unsigned max_threads = 15;
    float noise = 0.05f;

    std::vector<MeshBVH> bvh(max_threads);
    for (unsigned i = 0; i < max_threads; i++)
      bvh[i].init(mesh);
    auto noisy_sdf = [&](const float3 &p, unsigned idx) -> float { return bvh[idx].get_signed_distance(p) + urand()*noise; };
    auto real_sdf = [&](const float3 &p, unsigned idx) -> float { return bvh[idx].get_signed_distance(p); };


  LiteImage::Image2D<uint32_t> image_1(W, H);
  LiteImage::Image2D<uint32_t> image_2(W, H);
  LiteImage::Image2D<uint32_t> image_3(W, H);

  {
    preset = getDefaultPreset();
    preset.mesh_normal_mode = MESH_NORMAL_MODE_VERTEX;
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(mesh);
    render(image_1, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_22_mesh.bmp", image_1);
  } 

  auto grid = sdf_converter::create_sdf_grid(GridSettings(100), real_sdf, max_threads);
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(grid);
    render(image_1, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_22_grid.bmp", image_1);
  }
  float2 q_best = get_quality(real_sdf, grid);
  printf("q_best = %f, %f\n", q_best.x, q_best.y);

  auto noisy_grid = sdf_converter::create_sdf_grid(GridSettings(100), noisy_sdf, max_threads);
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(noisy_grid);
    render(image_1, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_22_grid_noisy.bmp", image_1);
  }
  float2 q_noisy = get_quality(real_sdf, grid);
  printf("q_noisy = %f, %f\n", q_noisy.x, q_noisy.y);

  auto smoothed_grid = sdf_converter::sdf_grid_smoother(grid, 1.1, 0.025, 0.02, 100);
  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);
    pRender->SetScene(smoothed_grid);
    render(image_1, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    LiteImage::SaveImage<uint32_t>("saves/test_22_grid_smoothed.bmp", image_1);    
  }
  float2 q_smoothed = get_quality(real_sdf, grid);
  printf("q_smoothed = %f, %f\n", q_smoothed.x, q_smoothed.y);
}

void perform_tests_litert(const std::vector<int> &test_ids)
{
  std::vector<int> tests = test_ids;

  std::vector<std::function<void(void)>> test_functions = {
      litert_test_1_framed_octree, litert_test_2_SVS, litert_test_3_SBS_verify,
      litert_test_4_hydra_scene, litert_test_5_interval_tracing, litert_test_6_faster_bvh_build,
      test_7_neural_SDF, litert_test_8_SDF_grid, litert_test_9_mesh, 
      litert_test_10_save_load, litert_test_11_hp_octree_legacy, litert_test_12_hp_octree_render,
      litert_test_13_hp_octree_build, litert_test_14_octree_nodes_removal, 
      litert_test_15_frame_octree_nodes_removal, litert_test_16_SVS_nodes_removal,
      litert_test_17_all_types_sanity_check, litert_test_18_mesh_normalization,
      litert_test_19_marching_cubes, litert_test_20_radiance_fields, litert_test_21_rf_to_mesh,
      litert_test_22_sdf_grid_smoothing};

  if (tests.empty())
  {
    tests.resize(test_functions.size());
    for (int i = 0; i < test_functions.size(); i++)
      tests[i] = i + 1;
  }

  for (int i = 0; i < 80; i++)
    printf("#");
  printf("\nSDF SCENE TESTS\n");
  for (int i = 0; i < 80; i++)
    printf("#");
  printf("\n");

  for (int i : tests)
  {
    assert(i > 0 && i <= test_functions.size());
    test_functions[i - 1]();
  }
}
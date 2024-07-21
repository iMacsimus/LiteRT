#include "tests.h"
#include "../IRenderer.h"
#include "../Renderer/eye_ray.h"
#include "../utils/mesh_bvh.h"
#include "../utils/mesh.h"
#include "LiteScene/hydraxml.h"
#include "LiteMath/Image2d.h"
#include "../utils/sdf_converter.h"
#include "../utils/sparse_octree_2.h"
#include "../utils/marching_cubes.h"
#include "../utils/sdf_smoother.h"
#include "../utils/demo_meshes.h"
#include "../utils/image_metrics.h"
#include "../diff_render/MultiRendererDR.h"

#include <functional>
#include <cassert>
#include <chrono>

static double urand(double from=0, double to=1)
{
  return ((double)rand() / RAND_MAX) * (to - from) + from;
}

float circle_sdf(float3 center, float radius, float3 p)
{
  return length(p - center) - radius;
}
float3 gradient_color(float3 p)
{
  return  (1-p.x)*(1-p.y)*(1-p.z)*float3(0,0,0) + 
          (1-p.x)*(1-p.y)*(  p.z)*float3(0,0,1) + 
          (1-p.x)*(  p.y)*(1-p.z)*float3(0,1,0) + 
          (1-p.x)*(  p.y)*(  p.z)*float3(0,1,1) + 
          (  p.x)*(1-p.y)*(1-p.z)*float3(1,0,0) + 
          (  p.x)*(1-p.y)*(  p.z)*float3(1,0,1) + 
          (  p.x)*(  p.y)*(1-p.z)*float3(1,1,0) + 
          (  p.x)*(  p.y)*(  p.z)*float3(1,1,1);
}

//creates SBS where all nodes are present, i.e.
//it is really a regular grid, but with more indexes
//distance and color fields must be given
//it is for test purposes only
//SBS is created in [-1,1]^3 cube, as usual
SdfSBS create_grid_sbs(unsigned brick_count, unsigned brick_size, 
                       std::function<float(float3)>  sdf_func,
                       std::function<float3(float3)> color_func)
{
  unsigned v_size = brick_size+1;
  unsigned dist_per_node = v_size*v_size*v_size;
  unsigned colors_per_node = 8;
  unsigned p_count = brick_count*brick_size + 1u;
  unsigned c_count = brick_count + 1u;
  unsigned c_offset = p_count*p_count*p_count;

  SdfSBS scene;
  scene.header.brick_size = brick_size;
  scene.header.brick_pad  = 0;
  scene.header.bytes_per_value = 4;
  scene.header.aux_data = SDF_SBS_NODE_LAYOUT_ID32F_IRGB32F;

  scene.values_f.resize(p_count*p_count*p_count + 3*c_count*c_count*c_count);
  scene.values.resize(brick_count*brick_count*brick_count*(dist_per_node+colors_per_node));
  scene.nodes.resize(brick_count*brick_count*brick_count);

  //fill the distances
  for (unsigned x = 0; x < p_count; x++)
  {
    for (unsigned y = 0; y < p_count; y++)
    {
      for (unsigned z = 0; z < p_count; z++)
      {
        unsigned idx = x*p_count*p_count + y*p_count + z;
        float3 p = 2.0f*(float3(x, y, z) / float3(brick_count*brick_size)) - 1.0f;
        scene.values_f[idx] = sdf_func(p);
      }
    }
  }

  //fill the colors
  for (unsigned x = 0; x < c_count; x++)
  {
    for (unsigned y = 0; y < c_count; y++)
    {
      for (unsigned z = 0; z < c_count; z++)
      {
        unsigned idx = x*c_count*c_count + y*c_count + z;
        float3 p = 2.0f*(float3(x, y, z) / float3(brick_count)) - 1.0f;
        float3 color = color_func(p);
        scene.values_f[c_offset + 3*idx + 0] = color.x;
        scene.values_f[c_offset + 3*idx + 1] = color.y;
        scene.values_f[c_offset + 3*idx + 2] = color.z;
      }
    }
  }

  //fill the nodes and indices
  for (unsigned bx = 0; bx < brick_count; bx++)
  {
    for (unsigned by = 0; by < brick_count; by++)
    {
      for (unsigned bz = 0; bz < brick_count; bz++)
      {

        //nodes
        unsigned n_idx = bx*brick_count*brick_count + by*brick_count + bz;
        unsigned offset = n_idx*(dist_per_node+colors_per_node);
        scene.nodes[n_idx].pos_xy = (bx << 16) | by;
        scene.nodes[n_idx].pos_z_lod_size = (bz << 16) | brick_count;
        scene.nodes[n_idx].data_offset = offset;

        //indices for distances
        for (unsigned x = 0; x < v_size; x++)
        {
          for (unsigned y = 0; y < v_size; y++)
          {
            for (unsigned z = 0; z < v_size; z++)
            {
              unsigned idx = x*v_size*v_size + y*v_size + z;
              unsigned val_idx = (bx*brick_size + x)*p_count*p_count + (by*brick_size + y)*p_count + (bz*brick_size + z);
              scene.values[offset + idx] = val_idx;
            }
          }
        }

        //indices for colors
        for (unsigned x = 0; x < 2; x++)
        {
          for (unsigned y = 0; y < 2; y++)
          {
            for (unsigned z = 0; z < 2; z++)
            {
              unsigned idx = x*2*2 + y*2 + z;
              unsigned val_idx = c_offset + 3*((bx + x)*c_count*c_count + (by + y)*c_count + (bz + z));
              scene.values[offset + dist_per_node + idx] = val_idx;
            }
          }
        }
      }
    }
  }

  return scene;  
}

SdfSBS circle_one_brick_scene()
{
  return create_grid_sbs(1, 16, 
                         [&](float3 p){return circle_sdf(float3(0,0,0), 0.8f, p);}, 
                         gradient_color);
}

SdfSBS circle_small_scene()
{
  return create_grid_sbs(4, 4, 
                         [&](float3 p){return circle_sdf(float3(0,0,0), 0.8f, p);}, 
                         gradient_color);
}

SdfSBS circle_medium_scene()
{
  return create_grid_sbs(16, 4, 
                         [&](float3 p){return circle_sdf(float3(0,0,0), 0.8f, p);}, 
                         gradient_color);
}

std::vector<float4x4> get_cameras_uniform_sphere(int count, float3 center, float radius)
{
  std::vector<float4x4> cameras;
  for (int i = 0; i < count; i++)
  {
    float phi = 2 * M_PI * urand();
    float psi = (M_PI / 2) * (1 - sqrtf(urand()));
    if (urand() > 0.5)
      psi = -psi;

    float3 view_dir = -float3(cos(psi) * sin(phi), sin(psi), cos(psi) * cos(phi));
    float3 tangent = normalize(cross(view_dir, float3(0, 1, 0)));
    float3 new_up = normalize(cross(view_dir, tangent));
    cameras.push_back(LiteMath::lookAt(center - radius * view_dir, center, new_up));
  }

  return cameras;
}

std::vector<float4x4> get_cameras_turntable(int count, float3 center, float radius, float height)
{
  std::vector<float4x4> cameras;
  for (int i = 0; i < count; i++)
  {
    float phi = 2 * M_PI * urand();

    float3 view_dir = -float3(sin(phi), height/radius, cos(phi));
    float3 tangent = normalize(cross(view_dir, float3(0, 1, 0)));
    float3 new_up = normalize(cross(view_dir, tangent));
    cameras.push_back(LiteMath::lookAt(center - radius * view_dir, center, new_up));
  }

  return cameras;
}

void randomize_color(SdfSBS &sbs)
{
  int v_size = sbs.header.brick_size + 2 * sbs.header.brick_pad + 1;
  int dist_per_node = v_size * v_size * v_size;
  for (auto &n : sbs.nodes)
  {
    for (int i = 0; i < 8; i++)
    {
      unsigned off = sbs.values[n.data_offset + dist_per_node + i];
      for (int j = 0; j < 3; j++)
        sbs.values_f[off + j] = urand();
    }
  }
}

void render(LiteImage::Image2D<float4> &image, std::shared_ptr<MultiRenderer> pRender, 
            float3 pos, float3 target, float3 up, 
            MultiRenderPreset preset, int a_passNum = 1)
{
  float fov_degrees = 60;
  float z_near = 0.1f;
  float z_far = 100.0f;
  float aspect   = 1.0f;
  auto proj      = LiteMath::perspectiveMatrix(fov_degrees, aspect, z_near, z_far);
  auto worldView = LiteMath::lookAt(pos, target, up);

  pRender->RenderFloat(image.data(), image.width(), image.height(), worldView, proj, preset, a_passNum);
}

#ifdef USE_ENZYME
int enzyme_const, enzyme_dup, enzyme_out; // must be global
double __enzyme_autodiff(void*, ...);

double litert_test_28_enzyme_ad_sqr_1(double  x) { return  x *  x; }
double litert_test_28_enzyme_ad_sqr_2(double* x) { return *x * *x; }
double litert_test_28_enzyme_ad_mul(double k, double x) { return k * x; }
#endif

void diff_render_test_1_enzyme_ad()
{
  printf("TEST 1. Enzyme AD\n");
#ifdef USE_ENZYME
  double x = 5.;
  double df_dx_res = 2*x;

  // Output argument
  printf(" 1.1. %-64s", "Basic square derivative");
  double df_dx_out = __enzyme_autodiff((void*)litert_test_28_enzyme_ad_sqr_1, x);
  if ((df_dx_out - df_dx_res) < 1e-7 && (df_dx_out - df_dx_res) > -1e-7) // actually even (df_dx_out == df_dx_res) works
    printf("passed    (d(x^2) = %.1f, x = %.1f)\n", df_dx_out, x);
  else
    printf("FAILED,   (df_dx_out = %f, must be %.1f)\n", df_dx_out, df_dx_res);

  // Duplicated argument
  printf(" 1.2. %-64s", "Square derivative, dx is stored in an argument");
  double df_dx_arg = 0.;
  __enzyme_autodiff((void*)litert_test_28_enzyme_ad_sqr_2, &x, &df_dx_arg);
  if ((df_dx_arg - df_dx_res) < 1e-7 && (df_dx_arg - df_dx_res) > -1e-7) // actually even (df_dx_arg == df_dx_res) works
    printf("passed    (d(x^2) = %.1f, x = %.1f)\n", df_dx_arg, x);
  else
    printf("FAILED,   (df_dx_arg = %f, must be %.1f)\n", df_dx_arg, df_dx_res);

  // Inactive (const) argument (explicit)
  printf(" 1.3. %-64s", "Derivative d(k*x)/dx, k - parameter");
  double k = 7;
  df_dx_out = 0.; // only to verify the result
  df_dx_out = __enzyme_autodiff((void*)litert_test_28_enzyme_ad_mul, enzyme_const, k, x); // enzyme_const not needed if k is int
  if ((df_dx_out - k) < 1e-7 && (df_dx_out - k) > -1e-7) // actually even (df_dx_out == k) works
    printf("passed    (d(k*x)/dx = %.1f, k = %.1f)\n", df_dx_out, k);
  else
    printf("FAILED,   (df_dx_out = %f, must be %.1f)\n", df_dx_out, k);

#else
  printf("  Enzyme AD is not used.\n");

#endif
}

void diff_render_test_2_forward_pass()
{
  //create renderers for SDF scene and mesh scene
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/bunny.vsgf").c_str());
  cmesh4::rescale_mesh(mesh, float3(-0.95, -0.95, -0.95), float3(0.95, 0.95, 0.95));

  unsigned W = 1024, H = 1024;

  MultiRenderPreset preset = getDefaultPreset();
  preset.render_mode = MULTI_RENDER_MODE_DIFFUSE;
  preset.ray_gen_mode = RAY_GEN_MODE_RANDOM;
  preset.spp = 16;
  
  SparseOctreeSettings settings(SparseOctreeBuildType::MESH_TLO, 7);

  SdfSBSHeader header;
  header.brick_size = 2;
  header.brick_pad = 0;
  header.bytes_per_value = 1;
  SdfSBS indexed_SBS;

  LiteImage::Image2D<float4> texture = LiteImage::LoadImage<float4>("scenes/porcelain.png");

  float4x4 view, proj;
  LiteImage::Image2D<float4> image_mesh(W, H);
  LiteImage::Image2D<float4> image_SBS(W, H);
  LiteImage::Image2D<float4> image_SBS_dr(W, H);

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    uint32_t texId = pRender->AddTexture(texture);
    MultiRendererMaterial mat;
    mat.type = MULTI_RENDER_MATERIAL_TYPE_TEXTURED;
    mat.texId = texId;
    uint32_t matId = pRender->AddMaterial(mat);
    pRender->SetMaterial(matId, 0);

    pRender->SetScene(mesh);
    render(image_mesh, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    view = pRender->getWorldView();
    proj = pRender->getProj();
  }

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    uint32_t texId = pRender->AddTexture(texture);
    MultiRendererMaterial mat;
    mat.type = MULTI_RENDER_MATERIAL_TYPE_TEXTURED;
    mat.texId = texId;
    uint32_t matId = pRender->AddMaterial(mat);
    pRender->SetMaterial(matId, 0);  

    indexed_SBS = sdf_converter::create_sdf_SBS_indexed(settings, header, mesh, matId, pRender->getMaterials(), pRender->getTextures());
    pRender->SetScene(indexed_SBS);
    pRender->RenderFloat(image_SBS.data(), image_SBS.width(), image_SBS.height(), view, proj, preset);   
  }

  {
    dr::MultiRendererDR dr_render;
    dr::MultiRendererDRPreset dr_preset = dr::getDefaultPresetDR();

    dr_preset.opt_iterations = 1;
    dr_preset.opt_lr = 0;
    dr_preset.spp = 16;

    dr_render.SetReference({image_mesh}, {view}, {proj});
    dr_render.OptimizeColor(dr_preset, indexed_SBS);
    
    image_SBS_dr = dr_render.getLastImage(0);
  }

  LiteImage::SaveImage<float4>("saves/test_dr_2_mesh.bmp", image_mesh); 
  LiteImage::SaveImage<float4>("saves/test_dr_2_sbs.bmp", image_SBS);
  LiteImage::SaveImage<float4>("saves/test_dr_2_sbs_dr.bmp", image_SBS_dr);

  //float psnr_1 = image_metrics::PSNR(image_mesh, image_SBS);
  float psnr_2 = image_metrics::PSNR(image_SBS, image_SBS_dr);

  printf("TEST 2. Differentiable render forward pass\n");

  printf(" 2.1. %-64s", "Diff render of SBS match regular render");
  if (psnr_2 >= 40)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
}

void diff_render_test_3_optimize_color()
{
  //create renderers for SDF scene and mesh scene
  auto mesh = cmesh4::LoadMeshFromVSGF((scenes_folder_path + "scenes/01_simple_scenes/data/bunny.vsgf").c_str());
  cmesh4::rescale_mesh(mesh, float3(-0.95, -0.95, -0.95), float3(0.95, 0.95, 0.95));

  unsigned W = 512, H = 512;

  MultiRenderPreset preset = getDefaultPreset();
  preset.render_mode = MULTI_RENDER_MODE_DIFFUSE;
  preset.ray_gen_mode = RAY_GEN_MODE_RANDOM;
  preset.spp = 16;
  
  SparseOctreeSettings settings(SparseOctreeBuildType::MESH_TLO, 7);

  SdfSBSHeader header;
  header.brick_size = 2;
  header.brick_pad = 0;
  header.bytes_per_value = 1;
  SdfSBS indexed_SBS;

  LiteImage::Image2D<float4> texture = LiteImage::LoadImage<float4>("scenes/porcelain.png");

  float4x4 view, proj;
  LiteImage::Image2D<float4> image_mesh(W, H);
  LiteImage::Image2D<float4> image_SBS(W, H);
  LiteImage::Image2D<float4> image_SBS_dr(W, H);

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    uint32_t texId = pRender->AddTexture(texture);
    MultiRendererMaterial mat;
    mat.type = MULTI_RENDER_MATERIAL_TYPE_TEXTURED;
    mat.texId = texId;
    uint32_t matId = pRender->AddMaterial(mat);
    pRender->SetMaterial(matId, 0);

    pRender->SetScene(mesh);
    render(image_mesh, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
    view = pRender->getWorldView();
    proj = pRender->getProj();
  }

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    uint32_t texId = pRender->AddTexture(texture);
    MultiRendererMaterial mat;
    mat.type = MULTI_RENDER_MATERIAL_TYPE_TEXTURED;
    mat.texId = texId;
    uint32_t matId = pRender->AddMaterial(mat);
    pRender->SetMaterial(matId, 0);  

    indexed_SBS = sdf_converter::create_sdf_SBS_indexed(settings, header, mesh, matId, pRender->getMaterials(), pRender->getTextures());
    pRender->SetScene(indexed_SBS);
    pRender->RenderFloat(image_SBS.data(), image_SBS.width(), image_SBS.height(), view, proj, preset);   
  }

  {
    //put random colors to SBS
    randomize_color(indexed_SBS);

    dr::MultiRendererDR dr_render;
    dr::MultiRendererDRPreset dr_preset = dr::getDefaultPresetDR();

    dr_preset.opt_iterations = 200;
    dr_preset.opt_lr = 0.25;
    dr_preset.spp = 1;

    dr_render.SetReference({image_mesh}, {view}, {proj});
    dr_render.OptimizeColor(dr_preset, indexed_SBS);
    
    image_SBS_dr = dr_render.getLastImage(0);
  }

  LiteImage::SaveImage<float4>("saves/test_dr_3_mesh.bmp", image_mesh); 
  LiteImage::SaveImage<float4>("saves/test_dr_3_sbs.bmp", image_SBS);
  LiteImage::SaveImage<float4>("saves/test_dr_3_sbs_dr.bmp", image_SBS_dr);

  //float psnr_1 = image_metrics::PSNR(image_mesh, image_SBS);
  float psnr_2 = image_metrics::PSNR(image_mesh, image_SBS_dr);

  printf("TEST 3. Differentiable render optimize color\n");

  printf(" 3.1. %-64s", "Diff render for color reconstruction");
  if (psnr_2 >= 30)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
}

void diff_render_test_4_render_simple_scenes()
{
  unsigned W = 1024, H = 1024;
  MultiRenderPreset preset = getDefaultPreset();
  preset.render_mode = MULTI_RENDER_MODE_DIFFUSE;

  LiteImage::Image2D<float4> image_med(W, H);
  LiteImage::Image2D<float4> image_small(W, H);
  LiteImage::Image2D<float4> image_one_brick(W, H);

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    auto scene = circle_medium_scene();
    pRender->SetScene(scene);
    render(image_med, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  }

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    auto scene = circle_small_scene();
    pRender->SetScene(scene);
    render(image_small, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  }

  {
    auto pRender = CreateMultiRenderer("GPU");
    pRender->SetPreset(preset);
    pRender->SetViewport(0,0,W,H);

    auto scene = circle_one_brick_scene();
    pRender->SetScene(scene);
    render(image_one_brick, pRender, float3(0, 0, 3), float3(0, 0, 0), float3(0, 1, 0), preset);
  }

  LiteImage::SaveImage<float4>("saves/test_dr_4_medium.bmp", image_med); 
  LiteImage::SaveImage<float4>("saves/test_dr_4_small.bmp", image_small);
  LiteImage::SaveImage<float4>("saves/test_dr_4_one_brick.bmp", image_one_brick);

  float psnr_1 = image_metrics::PSNR(image_med, image_small);
  float psnr_2 = image_metrics::PSNR(image_med, image_one_brick);
  float psnr_3 = image_metrics::PSNR(image_small, image_one_brick);

  printf("TEST 4. Render simple scenes\n");

  printf(" 4.1. %-64s", "Small scene is ok");
  if (psnr_1 >= 30)
    printf("passed    (%.2f)\n", psnr_1);
  else
    printf("FAILED, psnr = %f\n", psnr_1);
  
  printf(" 4.2. %-64s", "One brick scene is ok");
  if (psnr_2 >= 30)
    printf("passed    (%.2f)\n", psnr_2);
  else
    printf("FAILED, psnr = %f\n", psnr_2);
  
  printf(" 4.3. %-64s", "Small and one brick scene are equal");
  if (psnr_3 >= 40)
    printf("passed    (%.2f)\n", psnr_3);
  else
    printf("FAILED, psnr = %f\n", psnr_3);
}

void perform_tests_diff_render(const std::vector<int> &test_ids)
{
  std::vector<int> tests = test_ids;

  std::vector<std::function<void(void)>> test_functions = {
      diff_render_test_1_enzyme_ad, diff_render_test_2_forward_pass, diff_render_test_3_optimize_color,
      diff_render_test_4_render_simple_scenes};

  if (tests.empty())
  {
    tests.resize(test_functions.size());
    for (int i = 0; i < test_functions.size(); i++)
      tests[i] = i + 1;
  }

  for (int i = 0; i < 80; i++)
    printf("#");
  printf("\nDIFF RENDER TESTS\n");
  for (int i = 0; i < 80; i++)
    printf("#");
  printf("\n");

  for (int i : tests)
  {
    assert(i > 0 && i <= test_functions.size());
    test_functions[i - 1]();
  }
}
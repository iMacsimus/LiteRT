#include "MultiRendererDR.h"
#include "BVH2DR.h"
#include <omp.h>

using LiteMath::float2;
using LiteMath::float3;
using LiteMath::float4;

using LiteMath::inverse4x4;
using LiteMath::lookAt;
using LiteMath::perspectiveMatrix;

//extern float __delta;

namespace dr
{
  static constexpr float z_near = 0.1;
  static constexpr float z_far = 10;

  static float linear_to_absolute_depth(float lz)
  {
    return lz * (z_far - z_near) + z_near;
  }

  static float absolute_to_linear_depth(float z)
  {
    return (z - z_near) / (z_far - z_near);
  }

  static float Loss(unsigned loss_function, float3 color, float3 ref_color)
  {
    switch (loss_function)
    {
    case DR_LOSS_FUNCTION_MSE:
    {
      float3 diff = color - ref_color;
      return dot(diff, diff);
    }
    break;
    case DR_LOSS_FUNCTION_MAE:
    {
      float3 diff = abs(color - ref_color);
      return diff.x + diff.y + diff.z;
    }
    break;    
    default:
      break;
    }
    return 0;
  }

  static float3 LossGrad(unsigned loss_function, float3 color, float3 ref_color)
  {
    switch (loss_function)
    {
    case DR_LOSS_FUNCTION_MSE:
    {
      float3 diff = color - ref_color;
      return 2.0f * diff;
    }
    break;
    case DR_LOSS_FUNCTION_MAE:
    {
      float3 diff = color - ref_color;
      return sign(diff);
    }
    break;
    default:
      break;
    }
    return float3(0,0,0);
  }

  static uint32_t diff_render_mode_to_multi_render_mode(uint32_t diff_render_mode)
  {
    switch (diff_render_mode)
    {
    case DR_RENDER_MODE_DIFFUSE:
    case DR_DEBUG_RENDER_MODE_BORDER_DETECTION:
    case DR_DEBUG_RENDER_MODE_BORDER_INTEGRAL:
      return MULTI_RENDER_MODE_DIFFUSE;
    case DR_RENDER_MODE_LAMBERT:
      return MULTI_RENDER_MODE_LAMBERT;    
    case DR_RENDER_MODE_MASK:
      return MULTI_RENDER_MODE_MASK;
    case DR_DEBUG_RENDER_MODE_PRIMITIVE:
      return MULTI_RENDER_MODE_PRIMITIVE;
    case DR_RENDER_MODE_LINEAR_DEPTH:
      return MULTI_RENDER_MODE_LINEAR_DEPTH;
    default:
      printf("Unknown diff_render_mode: %u\n", diff_render_mode);
      return MULTI_RENDER_MODE_DIFFUSE;
    }
  }

  MultiRendererDR::MultiRendererDR()
  {
    m_pAccelStruct = std::shared_ptr<ISceneObject>(new BVHDR());
    m_preset = getDefaultPreset();
    m_preset_dr = getDefaultPresetDR();
    m_mainLightDir = normalize3(float4(1, 0.5, 0.5, 1));
    m_mainLightColor = 1.0f * normalize3(float4(1, 1, 0.98, 1));
    m_seed = rand();
  }

  void MultiRendererDR::SetReference(const std::vector<LiteImage::Image2D<float4>> &images,
                                     const std::vector<LiteMath::float4x4> &worldView,
                                     const std::vector<LiteMath::float4x4> &proj)
  {
    m_imagesRef = images;
    m_worldViewRef = worldView;
    m_projRef = proj;

  }

  void MultiRendererDR::OptimizeColor(MultiRendererDRPreset preset, SdfSBS &sbs, bool verbose)
  {
    assert(sbs.nodes.size() > 0);
    assert(sbs.values.size() > 0);
    assert(sbs.values_f.size() > 0);
    assert(sbs.header.aux_data == SDF_SBS_NODE_LAYOUT_ID32F_IRGB32F);
    assert(preset.opt_iterations > 0);
    assert(preset.spp > 0);
    assert(m_imagesRef.size() > 0);
    assert(m_worldViewRef.size() == m_imagesRef.size());
    assert(m_projRef.size() == m_imagesRef.size());
    for (int i = 1; i < m_imagesRef.size(); i++)
    {
      assert(m_imagesRef[i].width() == m_imagesRef[0].width());
      assert(m_imagesRef[i].height() == m_imagesRef[0].height());
    }

    if (preset.dr_input_type == DR_INPUT_TYPE_LINEAR_DEPTH)
    {
      assert(preset.dr_render_mode == DR_RENDER_MODE_LINEAR_DEPTH);
      assert((preset.dr_reconstruction_flags & DR_RECONSTRUCTION_FLAG_COLOR) == 0);
    }

    unsigned max_threads = omp_get_max_threads();
    unsigned params_count = sbs.values_f.size();

    m_dLoss_dS_tmp = std::vector<float>(params_count*max_threads, 0);
    m_Opt_tmp = std::vector<float>(2*params_count, 0);

    m_preset_dr = preset;
    m_preset.spp = preset.spp;
    m_preset.sdf_node_intersect = SDF_OCTREE_NODE_INTERSECT_NEWTON; //we need newton to minimize calculations for border integral
    m_preset.ray_gen_mode = RAY_GEN_MODE_RANDOM;
    m_preset.render_mode = diff_render_mode_to_multi_render_mode(preset.dr_render_mode);

    m_width = m_imagesRef[0].width();
    m_height = m_imagesRef[0].height();
    m_images.resize(m_imagesRef.size(), LiteImage::Image2D<float4>(m_width, m_height, float4(0, 0, 0, 1)));

    SetScene(sbs, true);
    float *params = ((BVHDR*)m_pAccelStruct.get())->m_SdfSBSDataF.data();
    unsigned images_count = m_imagesRef.size();

    if (debug_pd_images)
      m_imagesDebug.resize(params_count, LiteImage::Image2D<float4>(m_width, m_height, float4(0, 0, 0, 1)));

    for (int iter = 0; iter < preset.opt_iterations; iter++)
    {
      float loss_sum = 0;
      float loss_max = -1e6;
      float loss_min = 1e6;

      //render (with multithreading)
      for (int image_iter = 0; image_iter < preset.image_batch_size; image_iter++)
      {
        unsigned image_id = rand() % images_count;
        SetViewport(0,0, m_width, m_height);
        UpdateCamera(m_worldViewRef[image_id], m_projRef[image_id]);
        Clear(m_width, m_height, "color");
        std::fill(m_dLoss_dS_tmp.begin(), m_dLoss_dS_tmp.end(), 0.0f);

        float loss = 1e6f;
        if (preset.dr_diff_mode == DR_DIFF_MODE_DEFAULT)
        {
          loss = RenderDR(m_imagesRef[image_id].data(), m_images[image_id].data(), m_dLoss_dS_tmp.data(), params_count);
        }
        else if (preset.dr_diff_mode == DR_DIFF_MODE_FINITE_DIFF)
        {
          bool is_geometry = (preset.dr_reconstruction_flags & DR_RECONSTRUCTION_FLAG_GEOMETRY);
          unsigned color_params = 3*8*sbs.nodes.size();
          unsigned active_params_start = is_geometry ? 0 : params_count - color_params;
          unsigned active_params_end   = is_geometry ? params_count - color_params : params_count;

          loss = RenderDRFiniteDiff(m_imagesRef[image_id].data(), m_images[image_id].data(), m_dLoss_dS_tmp.data(), params_count,
                                    active_params_start, active_params_end, 0.005f);
        }

        loss_sum += loss;
        loss_max = std::max(loss_max, loss);
        loss_min = std::min(loss_min, loss);

        //if (verbose && iter % 10 == 0)
        //  printf("%f\n", loss);
        if (verbose && image_id == 0)
        {
          printf("Iter:%4d, loss: %f (%f-%f)\n", iter, loss_sum/preset.image_batch_size, loss_min, loss_max);
          LiteImage::SaveImage<float4>(("saves/iter_"+std::to_string(iter)+"_"+std::to_string(image_id)+".bmp").c_str(), m_images[image_id]);
        }
      }

      //accumulate
      for (int i=1; i< max_threads; i++)
        for (int j = 0; j < params_count; j++)
          m_dLoss_dS_tmp[j] += m_dLoss_dS_tmp[j + params_count * i];

      for (int j = 0; j < params_count; j++)
        m_dLoss_dS_tmp[j] /= preset.image_batch_size;

      OptimizeStepAdam(iter, m_dLoss_dS_tmp.data(), params, m_Opt_tmp.data(), params_count, preset);
    } 

    if  (debug_pd_images && preset.dr_diff_mode == DR_DIFF_MODE_DEFAULT)
    {
      for (int i = 0; i < params_count; i++)
      {
        for (int j = 0; j < m_width * m_height; j++)
        {
          float l = m_imagesDebug[i].data()[j].x;
          m_imagesDebug[i].data()[j] = float4(std::max(0.0f, l), std::max(0.0f, -l),0,1);
        }
        LiteImage::SaveImage<float4>(("saves/PD_"+std::to_string(i)+"a.bmp").c_str(), m_imagesDebug[i]);
      }
    }
  }

  float MultiRendererDR::RenderDR(const float4 *image_ref, LiteMath::float4 *out_image,
                                  float *out_dLoss_dS, unsigned params_count)
  {
    unsigned max_threads = omp_get_max_threads();
    unsigned steps = (m_width * m_height + max_threads - 1)/max_threads;
    std::vector<double> loss_v(max_threads, 0.0f);

    //I - render image, calculate internal derivatives
    #pragma omp parallel for
    for (int thread_id = 0; thread_id < max_threads; thread_id++)
    {
      unsigned start = thread_id * steps;
      unsigned end = std::min((thread_id + 1) * steps, m_width * m_height);
      for (int i = start; i < end; i++)
        loss_v[thread_id] += CastRayWithGrad(i, image_ref, out_image, out_dLoss_dS + (params_count * thread_id));
    }

    //II - find border pixels
    m_borderPixels.resize(0);
    const int search_radius = 1;
    const float border_threshold = 1.0f; // ok for masks
    for (int i = 0; i < m_width * m_height; i++)
    {
      if (i >= m_packedXY.size())
        continue;

      const uint XY = m_packedXY[i];
      const uint x  = (XY & 0x0000FFFF);
      const uint y  = (XY & 0xFFFF0000) >> 16;

      if (x < search_radius || x >= m_width - search_radius || y <= search_radius || y >= m_height - search_radius)
        continue;
      
      float min_depth = 1e10f;
      float max_depth = -1e10f;

      for (int dx = -search_radius; dx <= search_radius; dx++)
      {
        for (int dy = -search_radius; dy <= search_radius; dy++)
        {
          float d = out_image[(y + dy) * m_width + x + dx].w;
          min_depth = std::min(min_depth, d);
          max_depth = std::max(max_depth, d);
        }
      }

      //printf("min_depth = %f, max_depth = %f\n", min_depth, max_depth);

      if (max_depth - min_depth  > border_threshold)
        m_borderPixels.push_back(i);
    }

    //III - calculate border intergral on border pixels
    if (m_borderPixels.size() > 0 && m_preset_dr.dr_diff_mode == DR_DIFF_MODE_DEFAULT &&
        (m_preset_dr.dr_reconstruction_flags & DR_RECONSTRUCTION_FLAG_GEOMETRY))
    {
      unsigned border_steps = (m_borderPixels.size() + max_threads - 1)/max_threads;
      #pragma omp parallel for
      for (int thread_id = 0; thread_id < max_threads; thread_id++)
      {
        unsigned start = thread_id * border_steps;
        unsigned end = std::min((thread_id + 1) * border_steps, (unsigned)m_borderPixels.size());
        for (int i = start; i < end; i++)
          CastBorderRay(m_borderPixels[i], image_ref, out_image, out_dLoss_dS + (params_count * thread_id));
      }
    }

    //IV - calculate loss
    double loss = 0.0f;
    for (auto &l : loss_v)
      loss += l;
    
    return loss/(m_width * m_height);
  }

  float MultiRendererDR::RenderDRFiniteDiff(const float4 *image_ref, LiteMath::float4* out_image, float *out_dLoss_dS, unsigned params_count,
                                            unsigned start_index, unsigned end_index, float delta)
  {
    assert(end_index > start_index);
    assert(end_index <= params_count);
    float *params = ((BVHDR*)m_pAccelStruct.get())->m_SdfSBSDataF.data();

    std::vector<float> m_dLoss_dS_tmp_2(m_dLoss_dS_tmp.size(), 0.0f);

    LiteImage::Image2D<float4> image_1(m_width, m_height, float4(0,0,0,1)), image_2(m_width, m_height, float4(0,0,0,1));

    for (unsigned i = start_index; i < end_index; i++)
    {
      float p0 = params[i];
      double loss_plus = 0.0f;
      double loss_minus = 0.0f;

      params[i] = p0 + delta;
      //RenderFloat(out_image, m_width, m_height, "color");
      RenderDR(image_ref, out_image, m_dLoss_dS_tmp_2.data(), params_count);
      for (int j = 0; j < m_width * m_height; j++)
      {
        float l = Loss(m_preset_dr.dr_loss_function, to_float3(out_image[j]), to_float3(image_ref[j]));
        image_1.data()[j] = out_image[j];//float4(l,l,0,1);
        loss_plus += l;
      }
    
      params[i] = p0 - delta;
      //RenderFloat(out_image, m_width, m_height, "color");
      RenderDR(image_ref, out_image, m_dLoss_dS_tmp_2.data(), params_count);
      for (int j = 0; j < m_width * m_height; j++)
      {
        float l = Loss(m_preset_dr.dr_loss_function, to_float3(out_image[j]), to_float3(image_ref[j]));
        image_2.data()[j] = out_image[j];//float4(l,l,0,1);
        loss_minus += l;
      }
      
      params[i] = p0;

      //it is the same way as loss is accumulated in RenderDR
      //loss_plus /= m_width * m_height;
      //loss_minus /= m_width * m_height;
      //printf("loss_plus = %f, loss_minus = %f\n", loss_plus, loss_minus);

      if (debug_pd_images)
      {
        LiteImage::SaveImage<float4>(("saves/PD_"+std::to_string(i)+"_1.bmp").c_str(), image_1);
        LiteImage::SaveImage<float4>(("saves/PD_"+std::to_string(i)+"_2.bmp").c_str(), image_2);

        for (int j = 0; j < m_width * m_height; j++)
        {
          float l1 = Loss(m_preset_dr.dr_loss_function, to_float3(image_1.data()[j]), to_float3(image_ref[j]));
          float l2 = Loss(m_preset_dr.dr_loss_function, to_float3(image_2.data()[j]), to_float3(image_ref[j]));
          float l = (l1 - l2) / (2 * delta);
          image_1.data()[j] = float4(std::max(0.0f, l), std::max(0.0f, -l),0,1);
        }

        LiteImage::SaveImage<float4>(("saves/PD_"+std::to_string(i)+"b.bmp").c_str(), image_1);
      }

      out_dLoss_dS[i] = (loss_plus - loss_minus) / (2 * delta);
    }

    double loss = 0.0f;
    //RenderFloat(out_image, m_width, m_height, "color");
    for (int j = 0; j < m_width * m_height; j++)
      loss += Loss(m_preset_dr.dr_loss_function, to_float3(out_image[j]), to_float3(image_ref[j]));
    
    return loss / (m_width * m_height);
  }

  void MultiRendererDR::OptimizeStepAdam(unsigned iter, const float* dX, float *X, float *tmp, unsigned size, MultiRendererDRPreset preset)
  {
    float lr = preset.opt_lr;
    float beta_1 = preset.opt_beta_1;
    float beta_2 = preset.opt_beta_2;
    float eps = preset.opt_eps;

    float *V = tmp;
    float *S = tmp + size;

    std::vector<uint2> borders{uint2(0, size)};
    std::vector<float2> limits{float2(-1000.0f, 1000.0f)};

    for (int border_id = 0; border_id < borders.size(); border_id++)
    {
      for (int i = borders[border_id].x; i < borders[border_id].y; i++)
      {
        float g = dX[i];
        V[i] = beta_1 * V[i] + (1 - beta_1) * g;
        float Vh = V[i] / (1 - pow(beta_1, iter + 1));
        S[i] = beta_2 * S[i] + (1 - beta_2) * g * g;
        float Sh = S[i] / (1 - pow(beta_2, iter + 1));
        X[i] = LiteMath::clamp(X[i] - lr * Vh / (sqrt(Sh) + eps), limits[border_id].x, limits[border_id].y);
      }
    }
  }

  float3 MultiRendererDR::CalculateColorWithGrad(const CRT_HitDR &hit,
                                                 LiteMath::float3x3 &dColor_dDiffuse,
                                                 LiteMath::float3x3 &dColor_dNorm)
  {
    float3 color = float3(1, 0, 1);

    switch (m_preset_dr.dr_render_mode)
    {
    case DR_RENDER_MODE_DIFFUSE:
    case DR_DEBUG_RENDER_MODE_BORDER_INTEGRAL:
    case DR_DEBUG_RENDER_MODE_BORDER_DETECTION:
      color = hit.color;

      dColor_dDiffuse = LiteMath::make_float3x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1));
      dColor_dNorm = LiteMath::make_float3x3(float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0));
      break;
    case DR_RENDER_MODE_LAMBERT:
    {
      float3 light_dir = normalize(float3(1, 1, 1));
      float3 norm = hit.normal;
      float q0 = dot(norm, light_dir); // = norm.x*light_dir.x + norm.y*light_dir.y + norm.z*light_dir.z
      float q = max(0.1f, q0);
      color = q * hit.color;

      dColor_dDiffuse = LiteMath::make_float3x3(float3(q, 0, 0), float3(0, q, 0), float3(0, 0, q));
      if (q0 > 0.1f)
      {
        float3 dq_dnorm = light_dir;
        dColor_dNorm = LiteMath::make_float3x3(hit.color.x * light_dir,
                                               hit.color.y * light_dir,
                                               hit.color.z * light_dir);
      }
      else
        dColor_dNorm = LiteMath::make_float3x3(float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0));
    }
    break;
    case DR_RENDER_MODE_MASK:
      color = float3(1, 1, 1);

      dColor_dDiffuse = LiteMath::make_float3x3(float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0));
      dColor_dNorm = LiteMath::make_float3x3(float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0));
      break;
    case DR_DEBUG_RENDER_MODE_PRIMITIVE:
      color = to_float3(decode_RGBA8(m_palette[(hit.primId) % palette_size]));
      break;
    case DR_RENDER_MODE_LINEAR_DEPTH:
    {
      float d = absolute_to_linear_depth(hit.t);
      color = float3(d, d, d);
    }
    break;
    default:
      break;
    }

    return color;
  }

  float MultiRendererDR::CastRayWithGrad(uint32_t tidX, const float4 *image_ref, LiteMath::float4 *out_image, float *out_dLoss_dS)
  {
    if (tidX >= m_packedXY.size())
      return 0.0;

    const uint XY = m_packedXY[tidX];
    const uint x  = (XY & 0x0000FFFF);
    const uint y  = (XY & 0xFFFF0000) >> 16;
    
    float3 res_color = float3(0,0,0);
    float tMin = 1e10f;
    float res_loss = 0.0f;
    uint32_t spp_sqrt = uint32_t(sqrt(m_preset.spp));
    float i_spp_sqrt = 1.0f/spp_sqrt;

    const float3 background_color = float3(0.0f, 0.0f, 0.0f);

    uint32_t ray_flags;

    if (m_preset_dr.dr_diff_mode == DR_DIFF_MODE_FINITE_DIFF)
    {
      ray_flags = DR_RAY_FLAG_NO_DIFF;
    }
    else if (m_preset_dr.dr_input_type == DR_INPUT_TYPE_LINEAR_DEPTH)
    {
      ray_flags = DR_RAY_FLAG_DDIST_DPOS;
    }
    else
    {
      ray_flags = DR_RAY_FLAG_NO_DIFF;

      if (m_preset_dr.dr_reconstruction_flags & DR_RECONSTRUCTION_FLAG_COLOR)
      {
        ray_flags |= DR_RAY_FLAG_DDIFFUSE_DCOLOR;
      }
      else if (m_preset_dr.dr_reconstruction_flags & DR_RECONSTRUCTION_FLAG_GEOMETRY)
      {
        if (m_preset_dr.dr_render_mode == DR_RENDER_MODE_MASK)
          ray_flags |= DR_RAY_FLAG_NO_DIFF;
        else if (m_preset_dr.dr_render_mode == DR_RENDER_MODE_DIFFUSE)
          ray_flags |= DR_RAY_FLAG_DDIFFUSE_DPOS;
        else if (m_preset_dr.dr_render_mode == DR_RENDER_MODE_LAMBERT)
          ray_flags |= DR_RAY_FLAG_DDIFFUSE_DPOS | DR_RAY_FLAG_DNORM_DPOS;
      }
    }

    for (uint32_t i = 0; i < m_preset.spp; i++)
    {
      float2 d = m_preset.ray_gen_mode == RAY_GEN_MODE_RANDOM ? rand2(x, y, i) : i_spp_sqrt*float2(i/spp_sqrt+0.5, i%spp_sqrt+0.5);
      float4 rayPosAndNear, rayDirAndFar;
      kernel_InitEyeRay(tidX, d, &rayPosAndNear, &rayDirAndFar);
      
      float3 color = float3(0,0,0);
      float3 dLoss_dColor = float3(0,0,0);
      LiteMath::float3x3 dColor_dDiffuse = LiteMath::make_float3x3(float3(1,0,0), float3(0,1,0), float3(0,0,1));
      LiteMath::float3x3 dColor_dNorm    = LiteMath::make_float3x3(float3(0,0,0), float3(0,0,0), float3(0,0,0));
      
      CRT_HitDR hit = ((BVHDR*)m_pAccelStruct.get())->RayQuery_NearestHitWithGrad(ray_flags, rayPosAndNear, rayDirAndFar, nullptr);
      
      if (hit.primId == 0xFFFFFFFF) //no hit
      {
        color = background_color;
        res_color += color / m_preset.spp;
      }
      else
      {
        color = CalculateColorWithGrad(hit, dColor_dDiffuse, dColor_dNorm);
        dLoss_dColor = LossGrad(m_preset_dr.dr_loss_function, color, to_float3(image_ref[y * m_width + x]));

        if (ray_flags & DR_RAY_FLAG_DDIFFUSE_DCOLOR)
        {
          for (PDColor &pd : hit.dDiffuse_dSc)
          {
            float3 diff = dLoss_dColor * (dColor_dDiffuse * float3(pd.value));
            out_dLoss_dS[pd.index + 0] += diff.x / m_preset.spp;
            out_dLoss_dS[pd.index + 1] += diff.y / m_preset.spp;
            out_dLoss_dS[pd.index + 2] += diff.z / m_preset.spp;

            if (debug_pd_images)
            {
              m_imagesDebug[pd.index + 0].data()[y * m_width + x] += (diff.x / m_preset.spp) * float4(1,1,1,0);
              m_imagesDebug[pd.index + 1].data()[y * m_width + x] += (diff.y / m_preset.spp) * float4(1,1,1,0);
              m_imagesDebug[pd.index + 2].data()[y * m_width + x] += (diff.z / m_preset.spp) * float4(1,1,1,0);
            }
          }
        }
        
        if (ray_flags & (DR_RAY_FLAG_DDIFFUSE_DPOS | DR_RAY_FLAG_DNORM_DPOS))
        {
          for (PDDist &pd : hit.dDiffuseNormal_dSd)
          {
            float diff = dot(dLoss_dColor, dColor_dDiffuse * pd.dDiffuse + dColor_dNorm * pd.dNorm);
            out_dLoss_dS[pd.index] += diff / m_preset.spp;

            if (debug_pd_images)
              m_imagesDebug[pd.index].data()[y * m_width + x] += (diff / m_preset.spp) * float4(1,1,1,0);
          }
        }

        if (ray_flags & DR_RAY_FLAG_DDIST_DPOS)
        {
          for (PDDist &pd : hit.dDiffuseNormal_dSd)
          {
            float diff = dot(dLoss_dColor, float3(pd.dDist)) / (z_far - z_near);
            out_dLoss_dS[pd.index] += diff / m_preset.spp;
            
            if (debug_pd_images)
              m_imagesDebug[pd.index].data()[y * m_width + x] += (diff / m_preset.spp) * float4(1,1,1,0);
          }
        }
      }

      res_color += color / m_preset.spp;
      tMin = min(tMin, hit.t);

      float loss          = Loss(m_preset_dr.dr_loss_function, color, to_float3(image_ref[y * m_width + x]));
      res_loss += loss / m_preset.spp;
    }
    out_image[y * m_width + x] = float4(res_color.x, res_color.y, res_color.z, tMin);

    return res_loss;
  }

  void MultiRendererDR::CastBorderRay(uint32_t tidX, const float4 *image_ref, LiteMath::float4* out_image, float* out_dLoss_dS)
  {
    if (tidX >= m_packedXY.size())
      return;

    const uint XY = m_packedXY[tidX];
    const uint x  = (XY & 0x0000FFFF);
    const uint y  = (XY & 0xFFFF0000) >> 16;

    const unsigned border_ray_flags = DR_RAY_FLAG_BORDER;
    const float relax_eps = 3e-4f;
    const float3 background_color = float3(0.0f, 0.0f, 0.0f);

    const unsigned border_spp = m_preset_dr.border_spp;
    const uint32_t spp_sqrt = uint32_t(sqrt(border_spp));
    const float i_spp_sqrt = 1.0f / spp_sqrt;
    const float3 dLoss_dColor = LossGrad(m_preset_dr.dr_loss_function, to_float3(out_image[y * m_width + x]), to_float3(image_ref[y * m_width + x]));
    
    float total_diff = 0.0f;

    for (int sample_id = 0; sample_id < border_spp; sample_id++)
    {
      PDShape relax_pt{
          .f_in = background_color,
          .t = 0.f,
          .f_out = background_color,
          .sdf = relax_eps, // only choose points with SDF() less than this, no need to pass relax_eps
          .indices = {0, 0, 0, 0, 0, 0, 0, 0},
          .dSDF_dtheta = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f}};
      float2 d = m_preset.ray_gen_mode == RAY_GEN_MODE_RANDOM ? rand2(x, y, sample_id) : i_spp_sqrt * float2(sample_id / spp_sqrt + 0.5, sample_id % spp_sqrt + 0.5);
      float4 rayPosAndNear, rayDirAndFar;
      kernel_InitEyeRay(tidX, d, &rayPosAndNear, &rayDirAndFar);
      CRT_HitDR hit = ((BVHDR *)m_pAccelStruct.get())->RayQuery_NearestHitWithGrad(border_ray_flags, rayPosAndNear, rayDirAndFar, &relax_pt);

      if (relax_pt.sdf < relax_eps)
      {
        float3 color_delta = float3(0,0,0);
        if (m_preset_dr.dr_input_type == DR_INPUT_TYPE_COLOR)
        {
          color_delta = relax_pt.f_in - relax_pt.f_out;
        }
        else if (m_preset_dr.dr_input_type == DR_INPUT_TYPE_LINEAR_DEPTH)
        {
          float l_in = linear_to_absolute_depth(relax_pt.t);
          float l_out = linear_to_absolute_depth(hit.t);
          color_delta = float3(l_in - l_out);
        }

        for (int j = 0; j < 8; j++)
        {
          float diff = dot(dLoss_dColor, (1.0f / relax_eps) * relax_pt.dSDF_dtheta[j] * (relax_pt.f_in - relax_pt.f_out));
          out_dLoss_dS[relax_pt.indices[j]] += diff / border_spp;

          total_diff += abs(diff) / border_spp;
          if (debug_pd_images)
            m_imagesDebug[relax_pt.indices[j]].data()[y * m_width + x] += (diff / border_spp) * float4(1, 1, 1, 0);
        }
      }
    }

    if (m_preset_dr.dr_render_mode == DR_DEBUG_RENDER_MODE_BORDER_DETECTION)
    {
      //printf("%u %u total_diff %f\n", x, y, total_diff);
      out_image[y * m_width + x] = float4(1, 1, 1, 1);
    }
    else if (m_preset_dr.dr_render_mode == DR_DEBUG_RENDER_MODE_BORDER_INTEGRAL)
      out_image[y * m_width + x] = float4(0.001f*total_diff, 0.01f*total_diff, 0.1f*total_diff, 1.0f);

  }

  
  float MultiRendererDR::SolveEikonal(float3 axes_mins, float grid_spacing)
  {
    float3 m = axes_mins;
    float  h = grid_spacing;

    // Sort according to m
    if (m[0] > m[1])
      std::swap(m[0], m[1]);
    if (m[0] > m[2])
      std::swap(m[0], m[2]);
    if (m[1] > m[2])
      std::swap(m[1], m[2]);


    // f == 1

    float dist_new = m[0] + h;
    if (dist_new > m[1])
    {
      float h_2 = h * h;
      float m_sum = m[0] + m[1], c_2 = (m[0] - m[1]) * (m[0] - m[1]);

      float x1 = (m_sum - std::sqrt(2 * h_2 - c_2)) * 0.5f,
            x2 = (m_sum + std::sqrt(2 * h_2 - c_2)) * 0.5f;

      bool x1_good = x1 > m[1];
      bool x2_good = x2 > m[1];

      if (x1_good == x2_good)
      {
        printf("Error: both roots are equally good/bad: %f < [%f, %f] <= %f\n", m[1], x1, x2, m[2]);
        // throw "Error";
      }
      dist_new = x2_good ? x2 : x1;

      if (dist_new > m[2])
      {
        const float one_third = 1.f / 3;
        m_sum += m[2];
        c_2 = 3.f * (m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - h_2);

        x1 = (m_sum - std::sqrt(m_sum * m_sum - c_2)) * one_third;
        x2 = (m_sum + std::sqrt(m_sum * m_sum - c_2)) * one_third;
        x1_good = x1 > m[2];
        x2_good = x2 > m[2];


        if (x1_good == x2_good)
        {
          printf("Error: both roots are equally good/bad: %f < [%f, %f]\n", m[2], x1, x2);
          throw "Error";
        }
        dist_new = x2_good ? x2 : x1;
      }
    }
    return dist_new;
  }


  void MultiRendererDR::Redistance(float *dist_in, uint3 size_in, float grid_spacing, uint32_t num_iters)
  {
    const uint32_t N_in = size_in.x * size_in.y * size_in.z;
    const int3 axes_offsets{(int) (size_in.y * size_in.z), (int) size_in.z, 1};

    float *dist_bord[2] = { new float[N_in], new float[N_in] };
    bool *frozen = new bool[N_in];


    // Init

    const float big_active_const = 99999.f;
    const float Infin = 1e38f;
    for (int i = 0; i < size_in.x; ++i)
      for (int j = 0; j < size_in.y; ++j)
        for (int k = 0; k < size_in.z; ++k)
        {
          int3 ind_in = { i, j, k };
          uint32_t ind_in_lin = ind_in.x * axes_offsets.x + ind_in.y * axes_offsets.y + ind_in.z;

          frozen[ind_in_lin] = false;

          // Initialize with a big value
          dist_bord[0][ind_in_lin] = big_active_const;
          dist_bord[1][ind_in_lin] = big_active_const;

          float dist_val = dist_in[ind_in_lin];

          // Surface itself is not moved
          if (dist_val == 0.f)
          {
            frozen[ind_in_lin] = true;
            dist_bord[0][ind_in_lin] = 0.f;
            dist_bord[1][ind_in_lin] = 0.f;
          }

          // Also freeze if a surface is between this node and any of its neighbours
          if (!frozen[ind_in_lin])
          {
            float3 ldistance{Infin,Infin,Infin};
            bool near_surface = false;

            for (int dim = 0; dim < 3; ++dim)
            {
              for (int offset_sign = -1; offset_sign < 2; offset_sign += 2)
              {
                int3 neighbour = ind_in;
                neighbour[dim] += offset_sign;

                if (neighbour.x >= 0 && neighbour.x < size_in.x &&
                    neighbour.y >= 0 && neighbour.y < size_in.y &&
                    neighbour.z >= 0 && neighbour.z < size_in.z)
                {
                  float n_val = dist_in[neighbour.x * axes_offsets.x +
                                        neighbour.y * axes_offsets.y +
                                        neighbour.z];
                  if (n_val * dist_val < 0)
                  {
                    near_surface = true;

                    float d = grid_spacing * dist_val / (dist_val - n_val);
                    if (ldistance[dim] > d)
                      ldistance[dim] = d;
                  }
                }
              }
            }

            if (near_surface)
            {
              float dsum = 0.f;

              for (int dim = 0; dim < 3; ++dim)
                if (ldistance[dim] > 0.f && ldistance[dim] < Infin)
                  dsum += 1.f / (ldistance[dim] * ldistance[dim]);

              dsum = std::sqrt(1.f / dsum);
              if (dist_val < 0.f)
                dsum *= -1.f;

              dist_bord[0][ind_in_lin] = dsum;
              dist_bord[1][ind_in_lin] = dsum;

              frozen[ind_in_lin] = true;
            }
          }
        }

    int curr_arr_ind = 0;
    const int max_level = size_in.x + size_in.y + size_in.z - 3;

    for (uint32_t iter = 0u; iter < num_iters; ++iter)
    {
      for (int ordering = 0; ordering < 8; ++ordering)
      {
        int start, end, step;
        if (ordering == 1 || ordering == 4 || ordering == 6 || ordering == 7)
        {
          start = max_level;
          end   = -1;
          step  = -1;
        }
        else
        {
          start = 0;
          end   = max_level + 1;
          step  = 1;
        }


        for (int level = start; level != end; level += step)
        {
          int xs = std::max(0, level - int(size_in.y + size_in.z - 2));
          int ys = std::max(0, level - int(size_in.x + size_in.z - 2));
          int xe = std::min((int) size_in.x - 1, level);
          int ye = std::min((int) size_in.y - 1, level);
          int xr = xe - xs + 1;
          int yr = ye - ys + 1;

          // pragma parallel for
          for (int i = xs; i < xs + xr; ++i)
            for (int j = ys; j < ys + yr; ++j)
            {
              int k = level - i - j;
              if (k < 0 || k >= size_in.z) continue; // only k check is needed

              int3 ind_in{ i, j, k };
              uint32_t ind_in_lin = ind_in.x * axes_offsets.x + ind_in.y * axes_offsets.y + ind_in.z;

              dist_bord[curr_arr_ind][ind_in_lin] = dist_bord[1 - curr_arr_ind][ind_in_lin];
              if (frozen[ind_in_lin] || dist_bord[curr_arr_ind][ind_in_lin] != big_active_const) continue;

              float3 axes_mins{big_active_const, big_active_const, big_active_const};

              for (int dim = 0; dim < 3; ++dim)
              {
                if (ind_in[dim] > 0 && ind_in[dim] < size_in[dim] - 1)
                {
                  axes_mins[dim] = dist_bord[1 - curr_arr_ind][ind_in_lin - axes_offsets[dim]];
                  float dist_tmp = dist_bord[1 - curr_arr_ind][ind_in_lin + axes_offsets[dim]];
                  if (std::abs(dist_tmp) < std::abs(axes_mins[dim]))
                    axes_mins[dim] = dist_tmp;
                }
                else if (ind_in[dim] >  0 && ind_in[dim] == size_in[dim] - 1)
                  axes_mins[dim] = dist_bord[1 - curr_arr_ind][ind_in_lin - axes_offsets[dim]];
                else if (ind_in[dim] == 0 && ind_in[dim] <  size_in[dim] - 1)
                  axes_mins[dim] = dist_bord[1 - curr_arr_ind][ind_in_lin + axes_offsets[dim]];
              }

              if (axes_mins.x < big_active_const || axes_mins.y < big_active_const || axes_mins.z < big_active_const)
              {
                bool pos_part = axes_mins.x >= 0.f && axes_mins.y >= 0.f && axes_mins.z >= 0.f;

                if (pos_part)
                {
                  dist_bord[curr_arr_ind][ind_in_lin] = std::min( SolveEikonal(axes_mins, grid_spacing),
                                                                  dist_bord[1 - curr_arr_ind][ind_in_lin]);
                }
                else
                {
                  if (axes_mins.x < 0.f) axes_mins.x = -axes_mins.x;
                  if (axes_mins.y < 0.f) axes_mins.y = -axes_mins.y;
                  if (axes_mins.z < 0.f) axes_mins.z = -axes_mins.z;
                  dist_bord[curr_arr_ind][ind_in_lin] = std::max(-SolveEikonal(axes_mins, grid_spacing),
                                                                 -dist_bord[1 - curr_arr_ind][ind_in_lin]);
                }
              }
            }
        }
      }
      curr_arr_ind = 1 - curr_arr_ind; // swap
    }

    // Copy from the result array (memcpy?)
    // pragma parallel for
    for (int i = 0; i < size_in.x; ++i)
      for (int j = 0; j < size_in.y; ++j)
        for (int k = 0; k < size_in.z; ++k)
        {
          uint32_t ind_in_lin = i * axes_offsets.x + j * axes_offsets.y + k;
          dist_in[ind_in_lin] = dist_bord[1 - curr_arr_ind][ind_in_lin];
        }

    delete[] frozen;
    delete[] dist_bord[1];
    delete[] dist_bord[0];
  }
}
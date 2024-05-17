#include "mesh.h"
#include "LiteMath/LiteMath.h"
#include <math.h> 
#include <stack>

namespace cmesh4
{
  using namespace LiteMath;

  void get_bbox(const cmesh4::SimpleMesh &mesh, float3 *min_pos, float3 *max_pos)
  {
    *min_pos = float3(1e9,1e9,1e9);
    *max_pos = float3(-1e9,-1e9,-1e9);

    for (const float4 &p : mesh.vPos4f)
    {
      *min_pos = min(*min_pos, to_float3(p));
      *max_pos = max(*max_pos, to_float3(p));
    }
  }

  float4x4 rescale_mesh(cmesh4::SimpleMesh &mesh, float3 min_pos, float3 max_pos)
  {
    assert(mesh.vPos4f.size() >= 3);

    float3 mesh_min, mesh_max;
    get_bbox(mesh, &mesh_min, &mesh_max);

    float3 mesh_size = mesh_max - mesh_min;
    float3 target_size = max_pos - min_pos;
    float3 scale3 = target_size/mesh_size;
    float scale = min(scale3.x, min(scale3.y, scale3.z));
    
    float4 scale_4 = float4(scale,scale,scale,1);
    float4 min_4 = to_float4(min_pos, 0.0f);
    float4 mesh_min_4 = to_float4(mesh_min, 0.0f);

    //changing poditions, .w coord is preserved
    for (float4 &p : mesh.vPos4f)
      p = min_4 + scale_4*(p - mesh_min_4);

    //it is only move and rescale, so now changes to normals are required

    float4x4 trans = translate4x4(min_pos)*scale4x4(float3(scale))*translate4x4(-mesh_min);
    return trans;
  }

  bool triangle_aabb_intersect_SAT(const float3 &a, const float3 &b, const float3 &c, 
                                   const float3 &aabb_half_size, float3 axis)
  {
    float p0 = dot(a, axis);
    float p1 = dot(b, axis);
    float p2 = dot(c, axis);

    float r = aabb_half_size.x * abs(dot(float3(1, 0, 0), axis)) +
              aabb_half_size.y * abs(dot(float3(0, 1, 0), axis)) +
              aabb_half_size.z * abs(dot(float3(0, 0, 1), axis));

    float maxP = max(p0, max(p1, p2));
    float minP = min(p0, min(p1, p2));

    return !(max(-maxP, minP) > r);
  }

  bool triangle_aabb_intersect(const float3 &a, const float3 &b, const float3 &c, 
                               const float3 &aabb_center, const float3 &aabb_half_size)
  {
    float3 tri_a = a - aabb_center;
    float3 tri_b = b - aabb_center;
    float3 tri_c = c - aabb_center;

    float3 ab = normalize(tri_b - tri_a);
    float3 bc = normalize(tri_c - tri_b);
    float3 ca = normalize(tri_a - tri_c);

    //Cross ab, bc, and ca with (1, 0, 0)
    float3 a00 = float3(0.0, -ab.z, ab.y);
    float3 a01 = float3(0.0, -bc.z, bc.y);
    float3 a02 = float3(0.0, -ca.z, ca.y);

    //Cross ab, bc, and ca with (0, 1, 0)
    float3 a10 = float3(ab.z, 0.0, -ab.x);
    float3 a11 = float3(bc.z, 0.0, -bc.x);
    float3 a12 = float3(ca.z, 0.0, -ca.x);

    //Cross ab, bc, and ca with (0, 0, 1)
    float3 a20 = float3(-ab.y, ab.x, 0.0);
    float3 a21 = float3(-bc.y, bc.x, 0.0);
    float3 a22 = float3(-ca.y, ca.x, 0.0);

    if (
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a00) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a01) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a02) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a10) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a11) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a12) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a20) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a21) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, a22) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, float3(1, 0, 0)) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, float3(0, 1, 0)) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, float3(0, 0, 1)) ||
        !triangle_aabb_intersect_SAT(tri_a, tri_b, tri_c, aabb_half_size, cross(ab, bc))
    )
    {
        return false;
    }

    return true;
  }

  TriangleListGrid create_triangle_list_grid(const cmesh4::SimpleMesh &mesh, uint3 grid_size)
  {
    TriangleListGrid grid;
    grid.size = grid_size;
    assert(grid.size.x > 0 && grid.size.y > 0 && grid.size.z > 0 &&
           grid.size.x*grid.size.y*grid.size.z < 100'000'000);
    grid.nodes.resize(grid.size.x*grid.size.y*grid.size.z);

    get_bbox(mesh, &grid.min_pos, &grid.max_pos);

    unsigned triangles_count = mesh.TrianglesNum();
    float3 grid_size_f = float3(grid_size);
    float3 bbox_size = grid.max_pos - grid.min_pos;
    float3 node_size = bbox_size*float3(1.0f/grid_size.x, 1.0f/grid_size.y, 1.0f/grid_size.z);

    //#pragma omp parallel for
    for (int t_i=0;t_i<triangles_count;t_i++)
    {
      float3 a = to_float3(mesh.vPos4f[mesh.indices[3*t_i+0]]);
      float3 b = to_float3(mesh.vPos4f[mesh.indices[3*t_i+1]]);
      float3 c = to_float3(mesh.vPos4f[mesh.indices[3*t_i+2]]);

      float3 min_f = min(a, min(b,c));
      float3 max_f = max(a, max(b,c));

      uint3 min_i = uint3(floor(((min_f - grid.min_pos) / bbox_size) * grid_size_f));
      uint3 max_i = uint3(floor(((max_f - grid.min_pos) / bbox_size) * grid_size_f));

      //printf("%d (%u %u %u)-(%u %u %u)\n", t_i, min_i.x, min_i.y, min_i.z, max_i.x, max_i.y, max_i.z);

      for (int i=min_i.x; i<=max_i.x; i++)
      {
        for (int j=min_i.y; j<=max_i.y; j++)
        {
          for (int k=min_i.z; k<=max_i.z; k++)
          {
            float3 node_center = grid.min_pos + float3(i+0.5f, j+0.5f, k+0.5f)*node_size;
            bool intersect = triangle_aabb_intersect(a, b, c, node_center, 0.5f*node_size);
            if (intersect)
            {
              grid.nodes[i*grid_size.y*grid_size.z + j*grid_size.z + k].triangle_ids.push_back(t_i);
              //printf("added %d\n", t_i);
            }
          }
        }
      }
    }

      for (int i=0; i<grid_size.x; i++)
      {
        for (int j=0; j<grid_size.y; j++)
        {
          for (int k=0; k<grid_size.z; k++)
          {
            //if (grid.nodes[i*grid_size.y*grid_size.z + j*grid_size.z + k].triangle_ids.size() > 0)
            //printf("(%u %u %u) %d triangles\n",i,j,k,(int)grid.nodes[i*grid_size.y*grid_size.z + j*grid_size.z + k].triangle_ids.size());
          }
        }
      }

    return grid;
  }

  float3 closest_point_triangle(const float3 &p, const float3 &a, const float3 &b, const float3 &c)
  {
    // implementation taken from Embree library
    const float3 ab = b - a;
    const float3 ac = c - a;
    const float3 ap = p - a;

    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f)
      return a; // #1

    const float3 bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3)
      return b; // #2

    const float3 cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6)
      return c; // #3

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
      const float v = d1 / (d1 - d3);
      return a + v * ab; // #4
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
      const float v = d2 / (d2 - d6);
      return a + v * ac; // #5
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
      const float v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      return b + v * (c - b); // #6
    }

    const float denom = 1.f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + v * ab + w * ac; // #0
  }

  float get_signed_distance(const cmesh4::SimpleMesh &mesh, const TriangleListGrid &grid, const float3 &pos)
  {
    float3 bbox_size = grid.max_pos - grid.min_pos;
    float3 grid_size_f = float3(grid.size);
    int3 cell_start = int3(floor(((pos - grid.min_pos) / bbox_size) * grid_size_f - float3(0.5, 0.5, 0.5)));

    float dist = 0.5f/grid_size_f.x;
    for (int i=0;i<8;i++)
    {
      int3 cell_i = cell_start + int3((i & 4) >> 2, (i & 2) >> 1, i & 1);
      if (cell_i.x<0||cell_i.y<0||cell_i.z<0||cell_i.x>=grid.size.x||cell_i.y>=grid.size.y||cell_i.z>=grid.size.z)
        continue;
      const auto &tris = grid.nodes[cell_i.x*grid.size.y*grid.size.z + cell_i.y*grid.size.z + cell_i.z].triangle_ids;
      if (tris.empty())
        continue;
      else
      {
        for (auto &t_i : tris)
        {
          float3 a = to_float3(mesh.vPos4f[mesh.indices[3*t_i+0]]);
          float3 b = to_float3(mesh.vPos4f[mesh.indices[3*t_i+1]]);
          float3 c = to_float3(mesh.vPos4f[mesh.indices[3*t_i+2]]);       

          float3 pt = closest_point_triangle(pos, a, b, c);
          dist = std::min(dist, length(pos - pt)); 
        }
      }
    }

    return dist;
  }

  //thread-local function
  void create_triangle_list_octree_rec(const cmesh4::SimpleMesh &mesh, 
                                       std::vector<uint32_t> &tri_ids,
                                       std::vector<TriangleListOctree::Node> &nodes,
                                       unsigned max_depth, unsigned max_triangles_per_leaf, float search_range_mult,
                                       const std::vector<uint32_t> &node_tri_ids,
                                       unsigned idx, float3 p, float d, unsigned level)
  {
    if (level >= max_depth || node_tri_ids.size() <= max_triangles_per_leaf)
    {
      //make this node a leaf
      nodes[idx].offset = 0;
      nodes[idx].tid_count = node_tri_ids.size();
      nodes[idx].tid_offset = tri_ids.size();
      for (auto &tri_idx : node_tri_ids) 
      {
        tri_ids.push_back(tri_idx);
        if (tri_idx == 174649)
          printf("udx %u ch node (%f %f %f)-%f %u\n", idx, p.x, p.y, p.z, d, level);
          
      }
      
      //printf("created %u-leaf %u on p=(%f %f %f) with %d tris\n", level, idx, p.x, p.y, p.z, (int)node_tri_ids.size());
    }
    else
    {
      //go deeper
      unsigned ch_idx = nodes.size();
      nodes[idx].offset = ch_idx;
      nodes[idx].tid_count = node_tri_ids.size();
      nodes[idx].tid_offset = tri_ids.size();

      for (int i=0;i<8;i++)
        nodes.emplace_back();
      
      std::vector<uint32_t> child_tri_ids;
      for (int i=0;i<8;i++)
      {
        child_tri_ids.resize(0);
        child_tri_ids.reserve(node_tri_ids.size());

        float ch_d = d/2;
        float3 ch_p = 2*p + float3((i & 4) >> 2, (i & 2) >> 1, i & 1);
        float3 ch_center = 2.0f*((ch_p + float3(0.5, 0.5, 0.5))*ch_d) - 1.0f;
        float3 ch_half_size = search_range_mult*float3(ch_d);

        //printf("ch node (%f %f %f)-(%f %f %f)\n", ch_center.x, ch_center.y, ch_center.z, ch_half_size.x, ch_half_size.y, ch_half_size.z);

        for (int t_i : node_tri_ids)
        {
          float3 a = to_float3(mesh.vPos4f[mesh.indices[3*t_i+0]]);
          float3 b = to_float3(mesh.vPos4f[mesh.indices[3*t_i+1]]);
          float3 c = to_float3(mesh.vPos4f[mesh.indices[3*t_i+2]]);

          if (triangle_aabb_intersect(a, b, c, ch_center, ch_half_size))
            child_tri_ids.push_back(t_i);
        }

        create_triangle_list_octree_rec(mesh, tri_ids, nodes, max_depth, max_triangles_per_leaf, search_range_mult,
                                        child_tri_ids, ch_idx+i, ch_p, ch_d, level+1);
      }
    }
  }

  TriangleListOctree create_triangle_list_octree(const cmesh4::SimpleMesh &mesh, unsigned max_depth, 
                                                 unsigned max_triangles_per_leaf, float search_range_mult)
  {
    std::vector<unsigned> all_tri_ids(mesh.TrianglesNum());
    for (int t_i=0;t_i<mesh.TrianglesNum();t_i++)
      all_tri_ids[t_i] = t_i;

    constexpr int NUM_THREADS = 8;
    std::vector<std::vector<uint32_t>> thread_local_tri_ids(NUM_THREADS);
    std::vector<std::vector<TriangleListOctree::Node>> thread_local_nodes(NUM_THREADS, {TriangleListOctree::Node()});

    #pragma omp parallel for num_threads(NUM_THREADS)
    for (int i=0;i<NUM_THREADS;i++)
    {
      create_triangle_list_octree_rec(mesh, thread_local_tri_ids[i], thread_local_nodes[i], max_depth, max_triangles_per_leaf,
                                      search_range_mult, all_tri_ids, 0, float3((i & 4) >> 2, (i & 2) >> 1, i & 1), 0.5f, 1);
    }

    TriangleListOctree octree;
    unsigned octree_total_nodes = 1;
    unsigned octree_total_tri_ids = 0;

    for (int i=0;i<NUM_THREADS;i++)
    {
      octree_total_nodes += thread_local_nodes[i].size();
      octree_total_tri_ids += thread_local_tri_ids[i].size();
    }

    octree.nodes.resize(octree_total_nodes);
    octree.triangle_ids.resize(octree_total_tri_ids);
    octree.nodes[0].offset = 1;

    unsigned node_ofs = 9;
    unsigned tri_ids_ofs = 0;
    for (int i=0;i<NUM_THREADS;i++)
    {
      bool empty_root = thread_local_nodes[i][0].offset == 0;
      octree.nodes[i+1].offset = empty_root ? 0 : node_ofs;
      octree.nodes[i+1].tid_count = empty_root ? thread_local_nodes[i][0].tid_count : 0;
      octree.nodes[i+1].tid_offset = empty_root ? thread_local_nodes[i][0].tid_offset + tri_ids_ofs : 0;

      for (int j=1;j<thread_local_nodes[i].size();j++)
      {
        octree.nodes[node_ofs+j-1] = thread_local_nodes[i][j];
        if (thread_local_nodes[i][j].offset == 0)
          octree.nodes[node_ofs+j-1].tid_offset += tri_ids_ofs;
        else
          octree.nodes[node_ofs+j-1].offset += node_ofs-1;
      }

      for (int j=0;j<thread_local_tri_ids[i].size();j++)
        octree.triangle_ids[tri_ids_ofs + j] = thread_local_tri_ids[i][j];

      node_ofs += thread_local_nodes[i].size() - 1;
      tri_ids_ofs += thread_local_tri_ids[i].size();
    }
    printf("created octee with %d nodes and %d tri ids\n", (int)octree.nodes.size(), (int)octree.triangle_ids.size());
    return octree;
  }

  void fix_normals(cmesh4::SimpleMesh &mesh)
  {
    {
      int broken_normals_cnt = 0;
      std::vector<bool> broken_normals(mesh.vNorm4f.size(), false);
      for (int i=0;i<mesh.vNorm4f.size();i++)
      {
        float3 n = to_float3(mesh.vNorm4f[i]);
        float len = dot(n, n);
        if (len < 1e-6 || isnan(len) || isinf(len))
        {
          broken_normals[i] = true;
          broken_normals_cnt++;
        } 
        else
          mesh.vNorm4f[i] = to_float4(n/sqrt(len), 1);
      }
      if (broken_normals_cnt > 0)
      {
        printf("WARNING: mesh has %d broken normals\n", broken_normals_cnt);
        //TODO: fix broken normals
      }
      else
        printf("OK: mesh has no broken normals\n");
    }
    {
      int flipped_normals = 0;
      std::vector<std::vector<unsigned>> edges(mesh.vPos4f.size(), std::vector<unsigned>());
      std::vector<bool> vertex_visited(mesh.vPos4f.size(), false);

      for (int i=0;i<mesh.indices.size();i+=3)
      {
        unsigned a = mesh.indices[i];
        unsigned b = mesh.indices[i+1];
        unsigned c = mesh.indices[i+2];
        edges[a].push_back(b);
        edges[a].push_back(c);
        edges[b].push_back(a);
        edges[b].push_back(c);
        edges[c].push_back(a);
        edges[c].push_back(b);
      }

      //TODO: check if the first normal in pointing in the right direction

      std::stack<unsigned> stack;
      stack.push(0);
      while (!stack.empty())
      {
        unsigned v = stack.top();
        stack.pop();
        if (!vertex_visited[v])
        {
          vertex_visited[v] = true;
          float3 n = to_float3(mesh.vNorm4f[v]);
          for (auto &v2 : edges[v])
          {
            if (!vertex_visited[v2])
            {
              float3 n2 = to_float3(mesh.vNorm4f[v2]);
              if (dot(n, n2) < 0)
              {
                flipped_normals++;
                mesh.vNorm4f[v2] = to_float4(-n2, 1);
              }
              stack.push(v2);
            }
          }
        }
      }

      if (flipped_normals == 0)
        printf("OK: all normals pointing in the same direction\n");
      else
        printf("WARNING: %d/%d were pointing in the wrong direction and were fixed\n", flipped_normals, (int)mesh.vNorm4f.size());
    }
  }

  LiteMath::float4x4 normalize_mesh(cmesh4::SimpleMesh &mesh)
  {
    LiteMath::float4x4 transform = rescale_mesh(mesh, 0.999f*float3(-1, -1, -1), 0.999f*float3(1, 1, 1));

    bool is_watertight = check_watertight_mesh(mesh);
    if (is_watertight)
      printf("OK: mesh is watertight\n");
    else
      printf("WARNING: mesh is not watertight\n");

    fix_normals(mesh);

    return transform;
  }
}
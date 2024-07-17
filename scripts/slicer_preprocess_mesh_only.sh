
start_dir=$PWD
echo "start_dir: $start_dir"
cd $1

$2 $start_dir/Renderer/eye_ray.cpp $start_dir/BVH/BVH2Common.cpp \
-mainClass MultiRenderer \
-composInterface ISceneObject \
-composImplementation BVHRT \
-stdlibfolder $PWD/TINYSTL \
-pattern rtv \
-I$PWD/TINYSTL                  ignore \
-I$PWD/apps                     ignore \
-I$PWD/apps/LiteMath            ignore \
-I$start_dir                   process \
-I$start_dir/Renderer          process \
-I$start_dir/BVH               process \
-I$start_dir/sdfScene           ignore \
-shaderCC glsl \
-suffix _GPU \
-megakernel 1 \
-DPUGIXML_NO_EXCEPTIONS -DKERNEL_SLICER -v \
-DDISABLE_SDF_OCTREE \
-DDISABLE_SDF_GRID \
-DDISABLE_SDF_FRAME_OCTREE \
-DDISABLE_SDF_SVS \
-DDISABLE_SDF_SBS \
-DDISABLE_SDF_HP \
-DDISABLE_RF_GRID \
-DDISABLE_GS_PRIMITIVE

cd $start_dir
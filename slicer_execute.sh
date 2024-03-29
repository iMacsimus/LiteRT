#!/bin/bash
# slicer_execute absolute/path/to/slicer/folder absolute/path/to/slicer/executable
#e.g. bash slicer_execute.sh ~/kernel_slicer/ ~/kernel_slicer/kslicer
start_dir=$PWD
cd $1

$2 $start_dir/Renderer/eye_ray.cpp $start_dir/BVH/BVH2Common.cpp $start_dir/sdfScene/gpuReady/sdf_scene.cpp \
-mainClass EyeRayCaster \
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
-I$start_dir/sdfScene/gpuReady process \
-shaderCC glsl \
-suffix _GPU \
-megakernel 1 \
-DPUGIXML_NO_EXCEPTIONS -DKERNEL_SLICER -v

cd $start_dir
cd Renderer/shaders_gpu
bash build.sh
cd ../..
rm -r shaders_gpu
mv Renderer/shaders_gpu/ shaders_gpu
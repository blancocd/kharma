#!/bin/bash

# Make script for KHARMA
# Used to decide flags and call cmake
# TODO autodetection would be fun here, Parthenon may do some in future too.

# Set the correct compiler on Fedora machines
if [[ $(hostname) == "toolbox" ]]; then
  export NVCC_WRAPPER_DEFAULT_COMPILER=cuda-g++
fi

# Make conda go away.  Bad libraries. Bad.
source deactivate
echo $(which python)

# Some OSes name modern CMake differently
if command -v cmake3 > /dev/null 2>&1; then
  CMAKE=cmake3
else
  CMAKE=cmake
fi

# Only use icc on Stampede
if [[ $(hostname) == *"stampede2"* ]]; then
  CC_NATIVE=icc
  CXX_NATIVE=icpc
else
  CC_NATIVE=gcc
  CXX_NATIVE=g++
fi

if [[ "$*" == *"debug"* ]]; then
  TYPE=Debug
else
  TYPE=Release
fi

if [[ "$*" == *"cuda"* ]]; then
  USE_CUDA=1
else
  USE_CUDA=0
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

cd $SCRIPT_DIR
if [[ "$*" == *"clean"* ]]; then
  rm -rf build kharm.* core.*
fi
mkdir -p build

cd build


if [[ "$*" == *"clean"* ]]; then
  if [[ "$*" == *"cuda"* ]]; then # CUDA BUILD
    # TODO unify MPI flags
    $CMAKE ..\
    -DCMAKE_CXX_COMPILER=$PWD/../external/parthenon/external/Kokkos/bin/nvcc_wrapper \
    -DCMAKE_BUILD_TYPE=$TYPE \
    -DCMAKE_PREFIX_PATH=/usr/lib64/mpich \
    -DPAR_LOOP_LAYOUT="MANUAL1D_LOOP" \
    -DPAR_LOOP_INNER_LAYOUT="TVR_INNER_LOOP" \
    -DBUILD_TESTING=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DPARTHENON_DISABLE_MPI=OFF \
    -DPARTHENON_NGHOST=4 \
    -DPARTHENON_LINT_DEFAULT=OFF \
    -DENABLE_COMPILER_WARNINGS=OFF \
    -DKokkos_ENABLE_OPENMP=ON \
    -DKokkos_ENABLE_CUDA=ON \
    -DKokkos_ENABLE_HWLOC=ON \
    -DKokkos_ARCH_WSM=OFF \
    -DKokkos_ARCH_HSW=OFF \
    -DKokkos_ARCH_BDW=OFF \
    -DKokkos_ARCH_SKX=OFF \
    -DKokkos_ARCH_AMDAVX=ON \
    -DKokkos_ARCH_POWER9=OFF \
    -DKokkos_ARCH_KEPLER35=OFF \
    -DKokkos_ARCH_PASCAL60=OFF \
    -DKokkos_ARCH_VOLTA70=OFF \
    -DKokkos_ARCH_TURING75=ON \
    -DKokkos_ENABLE_CUDA_LAMBDA=ON \
    -DKokkos_ENABLE_CUDA_CONSTEXPR=ON
  else #OpenMP BUILD
    $CMAKE ..\
    -DCMAKE_C_COMPILER=$CC_NATIVE \
    -DCMAKE_CXX_COMPILER=$CXX_NATIVE \
    -DCMAKE_BUILD_TYPE=$TYPE \
    -DCMAKE_PREFIX_PATH=/usr/lib64/mpich \
    -DPAR_LOOP_LAYOUT="MANUAL1D_LOOP" \
    -DPAR_LOOP_INNER_LAYOUT="SIMDFOR_INNER_LOOP" \
    -DBUILD_TESTING=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DPARTHENON_DISABLE_MPI=OFF \
    -DPARTHENON_NGHOST=4 \
    -DPARTHENON_LINT_DEFAULT=OFF \
    -DENABLE_COMPILER_WARNINGS=OFF \
    -DKokkos_ENABLE_OPENMP=ON \
    -DKokkos_ENABLE_CUDA=OFF \
    -DKokkos_ENABLE_HWLOC=ON \
    -DKokkos_ARCH_HSW=OFF \
    -DKokkos_ARCH_BDW=OFF \
    -DKokkos_ARCH_SKX=OFF \
    -DKokkos_ARCH_KNL=ON \
    -DKokkos_ARCH_ARMV8_THUNDERX2=OFF \
    -DKokkos_ARCH_AMDAVX=OFF \
    -DKokkos_ARCH_EPYC=OFF
  fi
fi

make -j
cp kharma/kharma.* ..

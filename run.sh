#!/bin/bash

# OpenMP
export OMP_PROC_BIND=spread
export OMP_PLACES=threads
export OMP_NUM_THREADS=28

# Cuda: 1 device
export CUDA_LAUNCH_BLOCKING=0
export KOKKOS_DEVICE_ID=0

# Attempt at a personal 2-gpu config
#export KOKKOS_NUM_DEVICES=2
#export OMPI_MCA_btl_vader_single_copy_mechanism=none

# TODO longhorn

# Optionally use the Kokkos tools to profile
#export KOKKOS_PROFILE_LIBRARY=../kokkos-tools/kp_kernel_timer.so

KHARMA_DIR="$(dirname $0)"
if [ -f $KHARMA_DIR/kharma.cuda ]; then
  EXE_NAME=kharma.cuda
elif [ -f $KHARMA_DIR/kharma.host ]; then
  EXE_NAME=kharma.host
else
  echo "KHARMA executable not found!"
  exit
fi

# TODO options based on hostname etc here
$KHARMA_DIR/external/hpcbind/hpcbind --whole-system -- $KHARMA_DIR/$EXE_NAME "$@"
#mpirun -n 4 $KHARMA_DIR/$EXE_NAME "$@"
#mpirun -n 2 $KHARMA_DIR/external/hpcbind/hpcbind --whole-system --distribute=2 -- $KHARMA_DIR/$EXE_NAME "$@"

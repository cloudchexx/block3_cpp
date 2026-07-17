#!/usr/bin/env bash
set -euo pipefail
cd Z:/wutan/block3d-data
RUN_TEST=Z:/wutan/block3d-cpp/build/Release/run_test.exe
SCRUB=Z:/wutan/block3d-cache/scrub.bin
COMMON="--axis all --mode all --random-count 100 --seq-count 10 --cache-mode both --cold-method scrub --cold-scrub-file $SCRUB --cold-scrub-ratio 1.5 --cold-scrub-passes 1 --cold-settle-ms 1000 --cold-isolation suite --warmup-scope workload --batch-read fused --pipeline on --pipeline-memory 256 --read-dispatch round-robin"
for round in 1 2 3; do
  echo "===== test18 legacy round ${round} ====="
  "$RUN_TEST" --datasets test18 --block-size 56 $COMMON
  echo "===== test18 micro8 round ${round} ====="
  "$RUN_TEST" --datasets test18 --b3d-file Z:/wutan/block3d-data/test18_micro8.b3d $COMMON
  echo "===== test50 legacy round ${round} ====="
  "$RUN_TEST" --datasets test50 --block-size 64 $COMMON
  echo "===== test50 micro8 round ${round} ====="
  "$RUN_TEST" --datasets test50 --b3d-file Z:/wutan/block3d-data/test50_micro8.b3d $COMMON
 done

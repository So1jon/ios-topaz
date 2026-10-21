#!/bin/bash
# Build and run the DW-MSHC driver against the host simulator (macOS or Linux, any C compiler).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/dwmmc_sim_test
cc -O1 -Wall -Wextra -DDWMMC_SIM -I. dwmmc.c dwmmc_sim_test.c -o "$OUT"
"$OUT"

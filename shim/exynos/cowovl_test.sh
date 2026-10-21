#!/bin/bash
# Host test of the RAM copy-on-write overlay (cowovl.c).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/cowovl_test
cc -O1 -Wall -Wextra -I. cowovl.c cowovl_test.c -o "$OUT"
"$OUT"

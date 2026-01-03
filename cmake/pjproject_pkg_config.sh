#!/bin/sh
set -e

pkg-config "$@" | awk '{
  for (i = 1; i <= NF; i++) {
    if (!seen[$i]++) {
      out = out (out ? OFS : "") $i
    }
  }
}
END { print out }'

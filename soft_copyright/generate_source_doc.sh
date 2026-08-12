#!/bin/bash
# Generate source code document for software copyright application
# Format: A4, ~50 lines/page, front-to-back
#
# If total pages <= 60: submit all pages
# If total pages > 60: submit first 30 + last 30 pages

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
OUT=$DIR/taskhealth_source_code.txt
cd "$ROOT"
PAGE=1
LINE=0
MAX_PER_PAGE=50

rm -f $OUT

page_header() {
  echo "==============================================" >> $OUT
  echo "  软件名称: TaskHealth" >> $OUT
  echo "  版本号:   0.1.0" >> $OUT
  echo "  第 $PAGE 页" >> $OUT
  echo "==============================================" >> $OUT
  echo "" >> $OUT
}

file_header() {
  echo "" >> $OUT
  echo "  ── 文件: $1 ──" >> $OUT
  echo "" >> $OUT
}

write_file() {
  local f=$1
  local basename=$(basename $f)
  local n=0

  file_header "$basename"

  while IFS= read -r line; do
    # skip blank lines
    [ -z "$line" ] && continue
    if [ $LINE -ge $MAX_PER_PAGE ]; then
      PAGE=$((PAGE + 1))
      LINE=0
      page_header
      file_header "$basename (续)"
    fi
    echo "$line" >> $OUT
    LINE=$((LINE + 1))
    n=$((n + 1))
  done < "$f"
}

# First page header
page_header

# Shared protocol header (included by both daemon and client)
PROTO="src/protocol.h"

# Client library
CLIENT_SRC=(
  "src/taskhealth.h"
  "src/taskhealth.c"
  "src/taskhealth_mutex.h"
  "src/taskhealth_mutex.c"
)

# Daemon
DAEMON_SRC=(
  "daemon/main.c"
  "daemon/server.h"
  "daemon/server.c"
  "daemon/registry.h"
  "daemon/registry.c"
  "daemon/watchdog.h"
  "daemon/watchdog.c"
  "daemon/probe.h"
  "daemon/probe.c"
  "daemon/alert.h"
  "daemon/alert.c"
)

# Test & Demo (excluded from soft copyright submission)
# TEST_DEMO=(
#   "test/test.c"
#   "demo/demo.c"
# )

# Build system
BUILD=(
  "Makefile"
)

ALL_FILES=(
  "$PROTO"
  "${CLIENT_SRC[@]}"
  "${DAEMON_SRC[@]}"
  "${BUILD[@]}"
)

for f in "${ALL_FILES[@]}"; do
  if [ -f "$f" ]; then
    write_file "$f"
  fi
done

echo "" >> $OUT
echo "  总行数: $(wc -l < $OUT)" >> $OUT
echo "  总页数: $PAGE" >> $OUT
echo ""
echo "生成完成: $OUT ($PAGE 页)"

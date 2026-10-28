#!/usr/bin/env bash
#
# seekdb 一键编译脚本
#
# 用法:
#   ./build_seekdb.sh                 # 首次全量编译（初始化依赖 + 编译）
#   ./build_seekdb.sh --no-init       # 增量编译（依赖已就绪，跳过初始化）
#   ./build_seekdb.sh sanity          # 编译 sanity 模式
#   ./build_seekdb.sh clean           # 清理所有受管构建目录
#   JOBS=16 ./build_seekdb.sh         # 自定义并行编译任务数
#
# 可用环境变量:
#   JOBS            并行任务数，默认按 CPU/内存自动估算
#   USE_MIRROR      是否启用清华镜像加速 rustup/crates.io，默认 1（0 关闭）
#   DEP_CACHE_DIR   第三方依赖共享缓存目录，默认不启用
#   EXTRA_CMAKE_ARGS 追加给 build.sh 的 -D 参数（用空格分隔，可选）

set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$TOPDIR"

log()  { printf '\033[1;32m[build_seekdb]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[build_seekdb][WARN]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[build_seekdb][ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,16p' "${BASH_SOURCE[0]}"
}

# ---------- 1. 参数解析 ----------
MODE=release
DO_INIT=1
CMAKE_EXTRA=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --no-init|--skip-init)
      DO_INIT=0
      ;;
    release|sanity)
      MODE="$1"
      ;;
    clean)
      ./build.sh clean
      exit $?
      ;;
    -D*)
      CMAKE_EXTRA+=("$1")
      ;;
    *)
      die "未知参数: $1"
      ;;
  esac
  shift
done

if [[ -n "${EXTRA_CMAKE_ARGS:-}" ]]; then
  read -r -a _extra <<< "$EXTRA_CMAKE_ARGS"
  CMAKE_EXTRA+=("${_extra[@]}")
fi

# ---------- 2. Rust / Cargo 环境 ----------
# rustup 与 cargo 默认安装在 ~/.cargo 和 ~/.rustup，先确保其在 PATH 中。
CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
RUSTUP_HOME="${RUSTUP_HOME:-$HOME/.rustup}"
export CARGO_HOME RUSTUP_HOME

case ":$PATH:" in
  *":$CARGO_HOME/bin:"*) ;;
  *) export PATH="$CARGO_HOME/bin:$PATH" ;;
esac

command -v rustup >/dev/null 2>&1 || \
  die "未找到 rustup，请先安装 Rust: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
command -v cargo >/dev/null 2>&1 || \
  die "未找到 cargo，请执行: source \"\$HOME/.cargo/env\""

# 宿主机基础工具检查（缺失时仅提示，具体错误以 build.sh 输出为准）
missing_host_tools=()
for _t in git wget curl rpm rpm2cpio cpio make python3; do
  command -v "$_t" >/dev/null 2>&1 || missing_host_tools+=("$_t")
done
if (( ${#missing_host_tools[@]} > 0 )); then
  warn "缺少宿主机工具: ${missing_host_tools[*]}"
  warn "Ubuntu/Debian 可执行: sudo apt-get update && sudo apt-get install git wget curl rpm rpm2cpio cpio make build-essential binutils m4 file python3"
fi

# ---------- 3. 中国大陆网络加速（可选，失败不中断） ----------
USE_MIRROR="${USE_MIRROR:-1}"
if [[ "$USE_MIRROR" == "1" ]]; then
  export RUSTUP_DIST_SERVER="${RUSTUP_DIST_SERVER:-https://mirrors.tuna.tsinghua.edu.cn/rustup}"
  export RUSTUP_UPDATE_ROOT="${RUSTUP_UPDATE_ROOT:-https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup}"
  CARGO_CONF="$CARGO_HOME/config.toml"
  if [[ -f "$CARGO_CONF" ]]; then
    warn "检测到已有 $CARGO_CONF，跳过写入 crates.io 镜像；如需加速请自行确认其中的镜像配置"
  elif mkdir -p "$CARGO_HOME" 2>/dev/null && cat > "$CARGO_CONF" 2>/dev/null <<'EOF'
[source.crates-io]
replace-with = "tuna"

[source.tuna]
registry = "sparse+https://mirrors.tuna.tsinghua.edu.cn/crates.io-index/"

[registries.tuna]
index = "sparse+https://mirrors.tuna.tsinghua.edu.cn/crates.io-index/"
EOF
  then
    log "已写入 crates.io 清华镜像配置: $CARGO_CONF"
  else
    warn "无法写入 $CARGO_CONF，将使用 crates.io 默认源继续"
  fi
fi

# ---------- 4. 依赖共享缓存（可选） ----------
if [[ -n "${DEP_CACHE_DIR:-}" ]]; then
  export DEP_CACHE_DIR
  mkdir -p "$DEP_CACHE_DIR"
fi

# ---------- 5. 并行任务数 ----------
default_jobs() {
  local nproc mem_gb jobs
  nproc="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
  if [[ -r /proc/meminfo ]]; then
    mem_gb="$(awk '/MemTotal/ {printf "%d", $2/1024/1024}' /proc/meminfo)"
  else
    mem_gb=64
  fi
  if (( mem_gb < 12 )); then
    jobs=6
  elif (( mem_gb < 24 )); then
    jobs=12
  else
    jobs=32
  fi
  (( jobs > nproc )) && jobs="$nproc"
  printf '%s\n' "$jobs"
}
JOBS="${JOBS:-$(default_jobs)}"

# ---------- 6. 执行编译 ----------
BUILD_ARGS=("$MODE")
[[ "$DO_INIT" == "1" ]] && BUILD_ARGS+=(--init)
BUILD_ARGS+=("${CMAKE_EXTRA[@]}")
BUILD_ARGS+=(--make -j"$JOBS")

log "编译模式: $MODE, 并行任务: $JOBS, 初始化: $([[ $DO_INIT == 1 ]] && echo yes || echo no)"
./build.sh "${BUILD_ARGS[@]}"

log "编译完成。产物路径:"
case "$MODE" in
  release) log "  $TOPDIR/build_release/src/observer/seekdb" ;;
  sanity)  log "  $TOPDIR/build_sanity/src/observer/seekdb" ;;
esac

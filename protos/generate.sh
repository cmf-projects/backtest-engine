#!/usr/bin/env bash
# Generate C++ (and optional gRPC) stubs into protos/gen/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PROTO_DIR="$ROOT"
GEN_CPP="$ROOT/gen/cpp"

mkdir -p "$GEN_CPP"
# Drop previous stubs so renames don't leave orphans.
rm -f "$GEN_CPP"/*.pb.h "$GEN_CPP"/*.pb.cc "$GEN_CPP"/*.grpc.pb.h "$GEN_CPP"/*.grpc.pb.cc

if ! command -v protoc >/dev/null; then
  echo "error: protoc not found" >&2
  exit 1
fi

PROTOS=(
  "$PROTO_DIR/messages.proto"
  "$PROTO_DIR/rpc.proto"
)

echo "==> protoc messages -> $GEN_CPP"
protoc \
  -I "$PROTO_DIR" \
  --cpp_out="$GEN_CPP" \
  "${PROTOS[@]}"

if command -v grpc_cpp_plugin >/dev/null; then
  echo "==> protoc gRPC -> $GEN_CPP"
  # Service stubs only needed from rpc.proto; plugin still sees imports.
  protoc \
    -I "$PROTO_DIR" \
    --grpc_out="$GEN_CPP" \
    --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" \
    "$PROTO_DIR/rpc.proto"
else
  echo "==> skip gRPC stubs (grpc_cpp_plugin not installed)"
fi

echo "==> done"
ls -la "$GEN_CPP"

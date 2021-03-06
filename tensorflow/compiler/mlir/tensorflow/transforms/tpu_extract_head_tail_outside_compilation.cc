/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <type_traits>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Block.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Transforms/RegionUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_structs.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/device_util.h"

namespace mlir {
namespace TFTPU {

// This pass extracts a CPU computation cluster with `_xla_outside_compilation`
// annotation from the head or tail of a TPU cluster.

namespace {

constexpr char kXlaOutsideCompilationAttr[] = "_xla_outside_compilation";

bool HasOutsideCompilationAttribute(Operation* op) {
  return op->getAttrOfType<StringAttr>(kXlaOutsideCompilationAttr) != nullptr;
}

// Returns whether all operands of `op` are from values inside the
// `input_value_set`.
bool OpContainsOperandsFromSet(Operation* op,
                               const llvm::SetVector<Value>& input_value_set) {
  for (auto operand : op->getOperands())
    if (input_value_set.count(operand) == 0) return false;

  return true;
}

void RecordOutsideCompiledOpsAndUsages(
    Operation* op, llvm::SmallSetVector<Operation*, 4>* outside_compiled_ops,
    llvm::SetVector<Value>* outside_compiled_op_usages) {
  if (HasOutsideCompilationAttribute(op) &&
      OpContainsOperandsFromSet(op, *outside_compiled_op_usages)) {
    outside_compiled_ops->insert(op);
    outside_compiled_op_usages->insert(op->getResults().begin(),
                                       op->getResults().end());
  }
}

// Traverses the MLIR graph and returns a set of ops that
// are connected to inputs of TPU computation and outside compiled.
void ExtractOutsideCompiledOpsConnectedToHead(
    Value input_value, llvm::SetVector<Value>* values_used_in_host_cluster,
    llvm::SmallSetVector<Operation*, 4>* outside_compiled_ops) {
  llvm::SmallSetVector<Operation*, 4> parent_outside_compiled_ops_at_head;
  for (auto& usage : input_value.getUses()) {
    auto head_operation = usage.getOwner();
    RecordOutsideCompiledOpsAndUsages(head_operation,
                                      &parent_outside_compiled_ops_at_head,
                                      values_used_in_host_cluster);
  }

  // Traverse the graph and find all outside compiled ops connected from
  // the `input_value`.
  while (!parent_outside_compiled_ops_at_head.empty()) {
    llvm::SmallSetVector<Operation*, 4> connected_outside_compiled_ops;
    for (auto head_outside_compiled_op : parent_outside_compiled_ops_at_head) {
      auto op_results = head_outside_compiled_op->getOpResults();
      for (auto op_result : op_results) {
        for (auto& use : op_result.getUses()) {
          auto connected_op = use.getOwner();
          RecordOutsideCompiledOpsAndUsages(connected_op,
                                            &connected_outside_compiled_ops,
                                            values_used_in_host_cluster);
        }
      }
    }

    outside_compiled_ops->insert(parent_outside_compiled_ops_at_head.begin(),
                                 parent_outside_compiled_ops_at_head.end());
    std::swap(parent_outside_compiled_ops_at_head,
              connected_outside_compiled_ops);
  }
}

// TODO(hongjunchoi): Also handle ops without inputs that are outside
// compiled.
//
// Returns set of ops that are outside compiled and are directly connected
// to inputs to the TPU computation.
llvm::SmallSetVector<Operation*, 4> IdentifyOutsideCompiledOpsAtHead(
    tf_device::ClusterOp tpu_cluster) {
  llvm::SmallSetVector<Operation*, 4> outside_compiled_at_head_ops;
  llvm::SetVector<Value> values_used_in_cluster;
  auto& cluster_region = tpu_cluster.body();
  getUsedValuesDefinedAbove(cluster_region, cluster_region,
                            values_used_in_cluster);

  auto input_value_list = llvm::to_vector<8>(values_used_in_cluster);
  for (auto input_value : input_value_list)
    ExtractOutsideCompiledOpsConnectedToHead(
        input_value, &values_used_in_cluster, &outside_compiled_at_head_ops);
  return outside_compiled_at_head_ops;
}

// Returns output values of extracted outside compiled cluster at head that
// are used by the TPU computation.
llvm::SmallVector<Value, 8> GetHeadExtractedClusterOutputs(
    const llvm::SmallSetVector<Operation*, 4>& head_outside_compiled_ops) {
  llvm::SmallVector<Value, 8> outputs;
  outputs.reserve(head_outside_compiled_ops.size());

  for (auto op : head_outside_compiled_ops) {
    for (Operation* user : op->getUsers()) {
      if (!head_outside_compiled_ops.count(user)) {
        outputs.append(op->result_begin(), op->result_end());
        break;
      }
    }
  }

  return outputs;
}

// Creates new tf_device.launch op with outside compiled ops extracted
// from the head of TPU computation.
llvm::Optional<tf_device::LaunchOp> IsolateHeadExtractedOpsToLaunchOp(
    OpBuilder* builder, tf_device::ClusterOp cluster,
    const llvm::SmallSetVector<Operation*, 4>& head_outside_compiled_ops) {
  if (head_outside_compiled_ops.empty())
    return llvm::Optional<tf_device::LaunchOp>();

  // Create tf_device.launch op to separate all extracted outside compiled ops
  // before the tf_device.cluster.
  auto output_values =
      GetHeadExtractedClusterOutputs(head_outside_compiled_ops);

  llvm::SmallVector<Type, 8> output_return_types;
  output_return_types.reserve(output_values.size());
  for (auto output : output_values)
    output_return_types.emplace_back(output.getType());

  builder->setInsertionPoint(cluster);
  auto host_launch_op = builder->create<tf_device::LaunchOp>(
      cluster.getLoc(), builder->getStringAttr(""), output_return_types);

  // Replace all usages of outside compiled ops that are used in TPU
  // computation with the results of the above created launch op.
  for (auto output_and_index : llvm::enumerate(output_values)) {
    auto output_index = output_and_index.index();
    auto output = output_and_index.value();
    for (auto& use : output.getUses()) {
      if (!head_outside_compiled_ops.count(use.getOwner()))
        use.set(host_launch_op.getResult(output_index));
    }
  }

  // Create terminator op for the newly created launch op.
  host_launch_op.body().push_back(new Block());
  builder->setInsertionPointToEnd(&host_launch_op.GetBody());
  auto terminator = builder->create<tf_device::ReturnOp>(
      host_launch_op.getLoc(), output_values);

  // Move all outside compile ops from cluster op to launch op.
  for (auto outside_compiled_op : head_outside_compiled_ops)
    outside_compiled_op->moveBefore(terminator);

  return host_launch_op;
}

struct TPUExtractHeadTailOutsideCompilation
    : public PassWrapper<TPUExtractHeadTailOutsideCompilation,
                         OperationPass<ModuleOp>> {
  void runOnOperation() override;
};

void TPUExtractHeadTailOutsideCompilation::runOnOperation() {
  // Get runtime devices information from the closest parent module.
  auto module = getOperation();
  mlir::TF::RuntimeDevices devices;
  if (failed(tensorflow::GetDevicesFromOp(module, &devices)))
    return signalPassFailure();

  OpBuilder builder(&getContext());
  module.walk([&](tf_device::ClusterOp cluster) {
    auto head_outside_compiled_ops = IdentifyOutsideCompiledOpsAtHead(cluster);
    IsolateHeadExtractedOpsToLaunchOp(&builder, cluster,
                                      head_outside_compiled_ops);

    // TODO(b/156030523): Update device attribute of newly created host launch
    // op as well as enclosing Replicate op (if TPU computation is replicated)
    // with host device names.

    // TODO(b/155115766): Implement tail outside compiled op extraction.
  });
}

}  // anonymous namespace

std::unique_ptr<OperationPass<ModuleOp>>
CreateTPUExtractHeadTailOutsideCompilationPass() {
  return std::make_unique<TPUExtractHeadTailOutsideCompilation>();
}

static PassRegistration<TPUExtractHeadTailOutsideCompilation> pass(
    "tf-tpu-extract-head-tail-outside-compilation",
    "Extracts TPU head or tail outside compilation to separate "
    "parallel_execute.");

}  // namespace TFTPU
}  // namespace mlir

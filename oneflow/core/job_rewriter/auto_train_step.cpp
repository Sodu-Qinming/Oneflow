/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/job_rewriter/op_graph_pass.h"
#include "oneflow/core/job/job.pb.h"
#include "oneflow/core/job/foreign_callback.h"
#include "oneflow/core/framework/framework.h"

namespace oneflow {

namespace {

class AutoTrainStep final : public OpGraphPass {
 public:
  OF_DISALLOW_COPY_AND_MOVE(AutoTrainStep);
  AutoTrainStep() = default;
  ~AutoTrainStep() override = default;

  bool IsEnabled() const override { return GlobalJobDesc().IsTrain(); }

  Maybe<void> Apply(const OpGraph& op_graph, Job* job) const override;
};

Maybe<void> AutoTrainStep::Apply(const OpGraph& op_graph, Job* job) const {
  if (job->job_conf().train_conf().has_train_step_lbn()) { return Maybe<void>::Ok(); }
  OperatorConf variable_op_conf{};
  const std::string train_step_name = "System-Train-TrainStep-" + job->job_conf().job_name();
  variable_op_conf.set_name(train_step_name);
  VariableOpConf* variable_conf = variable_op_conf.mutable_variable_conf();
  variable_conf->set_out("out");
  *variable_conf->mutable_shape()->mutable_dim()->Add() = 1;
  variable_conf->set_data_type(DataType::kInt64);
  variable_conf->mutable_split_axis()->clear_value();
  variable_conf->mutable_initializer()->mutable_constant_int_conf()->set_value(0);

  OperatorConf identity_op_conf{};
  identity_op_conf.set_name(train_step_name + "-Identity");
  IdentityOpConf* identity_conf = identity_op_conf.mutable_identity_conf();
  identity_conf->set_in(GenLogicalBlobName(variable_op_conf.name(), variable_conf->out()));
  identity_conf->set_out("out");
  const std::string& train_step_lbn =
      GenLogicalBlobName(identity_op_conf.name(), identity_conf->out());

  auto scalar_add_op = user_op::UserOpConfWrapperBuilder(train_step_name + "-ScalarAdd")
                           .Op("scalar_add")
                           .Input("in", train_step_lbn)
                           .Output("out")
                           .Attr<bool>("has_float_operand", false)
                           .Attr<double>("float_operand", 0)
                           .Attr<bool>("has_int_operand", true)
                           .Attr<int64_t>("int_operand", 1)
                           .Build();

  auto assign_op =
      user_op::UserOpConfWrapperBuilder(train_step_name + "-Assign")
          .Op("assign")
          .Input("ref", GenLogicalBlobName(variable_op_conf.name(), variable_conf->out()))
          .Input("value", scalar_add_op.output("out", 0))
          .Build();

  JobBuilder job_builder(job);
  const ParallelConf& parallel_conf = GenParallelConfOfCpuZeroOnMaster();
  int64_t scope_symbol_id = Global<ForeignCallback>::Get()->MakeScopeSymbol(
      job->job_conf().DebugString(), parallel_conf.DebugString(), false);
  OperatorConf scalar_add_op_conf(scalar_add_op.op_conf());
  OperatorConf assign_op_conf(assign_op.op_conf());
  variable_op_conf.set_scope_symbol_id(scope_symbol_id);
  identity_op_conf.set_scope_symbol_id(scope_symbol_id);
  scalar_add_op_conf.set_scope_symbol_id(scope_symbol_id);
  assign_op_conf.set_scope_symbol_id(scope_symbol_id);
  job_builder.AddOps(parallel_conf,
                     {variable_op_conf, identity_op_conf, scalar_add_op_conf, assign_op_conf});
  job->mutable_job_conf()->mutable_train_conf()->set_train_step_lbn(train_step_lbn);
  return Maybe<void>::Ok();
}

REGISTER_FUNCTION_PASS("AutoTrainStep", AutoTrainStep);

}  // namespace

}  // namespace oneflow

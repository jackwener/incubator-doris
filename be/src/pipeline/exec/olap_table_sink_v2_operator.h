// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "operator.h"
#include "pipeline/pipeline_x/operator.h"
#include "vec/sink/vtablet_sink_v2.h"

namespace doris {

namespace pipeline {

class OlapTableSinkV2OperatorBuilder final
        : public DataSinkOperatorBuilder<vectorized::VOlapTableSinkV2> {
public:
    OlapTableSinkV2OperatorBuilder(int32_t id, DataSink* sink)
            : DataSinkOperatorBuilder(id, "OlapTableSinkV2Operator", sink) {}

    OperatorPtr build_operator() override;
};

class OlapTableSinkV2Operator final : public DataSinkOperator<OlapTableSinkV2OperatorBuilder> {
public:
    OlapTableSinkV2Operator(OperatorBuilderBase* operator_builder, DataSink* sink)
            : DataSinkOperator(operator_builder, sink) {}

    bool can_write() override { return true; } // TODO: need use mem_limit
};

class OlapTableSinkV2OperatorX;

class OlapTableSinkV2LocalState final
        : public AsyncWriterSink<vectorized::VTabletWriterV2, OlapTableSinkV2OperatorX> {
public:
    using Base = AsyncWriterSink<vectorized::VTabletWriterV2, OlapTableSinkV2OperatorX>;
    using Parent = OlapTableSinkV2OperatorX;
    ENABLE_FACTORY_CREATOR(OlapTableSinkV2LocalState);
    OlapTableSinkV2LocalState(DataSinkOperatorXBase* parent, RuntimeState* state)
            : Base(parent, state) {};
    Status init(RuntimeState* state, LocalSinkStateInfo& info) override;
    Status open(RuntimeState* state) override {
        SCOPED_TIMER(exec_time_counter());
        SCOPED_TIMER(_open_timer);
        return Base::open(state);
    }

    Status close(RuntimeState* state, Status exec_status) override;
    friend class OlapTableSinkV2OperatorX;

private:
    Status _close_status = Status::OK();
};

class OlapTableSinkV2OperatorX final : public DataSinkOperatorX<OlapTableSinkV2LocalState> {
public:
    using Base = DataSinkOperatorX<OlapTableSinkV2LocalState>;
    OlapTableSinkV2OperatorX(ObjectPool* pool, int operator_id, const RowDescriptor& row_desc,
                             const std::vector<TExpr>& t_output_expr, bool group_commit)
            : Base(operator_id, 0),
              _row_desc(row_desc),
              _t_output_expr(t_output_expr),
              _group_commit(group_commit),
              _pool(pool) {};

    Status init(const TDataSink& thrift_sink) override {
        RETURN_IF_ERROR(Base::init(thrift_sink));
        // From the thrift expressions create the real exprs.
        RETURN_IF_ERROR(vectorized::VExpr::create_expr_trees(_t_output_expr, _output_vexpr_ctxs));
        return Status::OK();
    }

    Status prepare(RuntimeState* state) override {
        RETURN_IF_ERROR(Base::prepare(state));
        return vectorized::VExpr::prepare(_output_vexpr_ctxs, state, _row_desc);
    }

    Status open(RuntimeState* state) override {
        RETURN_IF_ERROR(Base::open(state));
        return vectorized::VExpr::open(_output_vexpr_ctxs, state);
    }

    Status sink(RuntimeState* state, vectorized::Block* in_block,
                SourceState source_state) override {
        auto& local_state = get_local_state(state);
        SCOPED_TIMER(local_state.exec_time_counter());
        COUNTER_UPDATE(local_state.rows_input_counter(), (int64_t)in_block->rows());
        return local_state.sink(state, in_block, source_state);
    }

private:
    friend class OlapTableSinkV2LocalState;
    template <typename Writer, typename Parent>
    friend class AsyncWriterSink;
    const RowDescriptor& _row_desc;
    vectorized::VExprContextSPtrs _output_vexpr_ctxs;
    const std::vector<TExpr>& _t_output_expr;
    const bool _group_commit;
    ObjectPool* _pool = nullptr;
};

} // namespace pipeline
} // namespace doris
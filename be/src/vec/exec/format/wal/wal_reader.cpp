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

#include "wal_reader.h"

#include "common/logging.h"
#include "gutil/strings/split.h"
#include "olap/wal/wal_manager.h"
#include "runtime/runtime_state.h"
#include "vec/data_types/data_type_string.h"

namespace doris::vectorized {
WalReader::WalReader(RuntimeState* state) : _state(state) {
    _wal_id = state->wal_id();
}

WalReader::~WalReader() {
    if (_wal_reader.get() != nullptr) {
        static_cast<void>(_wal_reader->finalize());
    }
}

Status WalReader::init_reader(const TupleDescriptor* tuple_descriptor) {
    _tuple_descriptor = tuple_descriptor;
    RETURN_IF_ERROR(_state->exec_env()->wal_mgr()->get_wal_path(_wal_id, _wal_path));
    _wal_reader = std::make_shared<doris::WalReader>(_wal_path);
    RETURN_IF_ERROR(_wal_reader->init());
    return Status::OK();
}

Status WalReader::get_next_block(Block* block, size_t* read_rows, bool* eof) {
    //read src block
    PBlock pblock;
    auto st = _wal_reader->read_block(pblock);
    if (st.is<ErrorCode::END_OF_FILE>()) {
        LOG(INFO) << "read eof on wal:" << _wal_path;
        *read_rows = 0;
        *eof = true;
        return Status::OK();
    }
    if (!st.ok()) {
        LOG(WARNING) << "Failed to read wal on path = " << _wal_path;
        return st;
    }
    vectorized::Block src_block;
    RETURN_IF_ERROR(src_block.deserialize(pblock));
    //convert to dst block
    vectorized::Block dst_block;
    int index = 0;
    auto columns = block->get_columns_with_type_and_name();
    CHECK(columns.size() == _tuple_descriptor->slots().size());
    for (auto slot_desc : _tuple_descriptor->slots()) {
        auto pos = _column_pos_map[slot_desc->col_unique_id()];
        vectorized::ColumnPtr column_ptr = src_block.get_by_position(pos).column;
        if (column_ptr != nullptr && slot_desc->is_nullable()) {
            column_ptr = make_nullable(column_ptr);
        }
        dst_block.insert(
                index, vectorized::ColumnWithTypeAndName(std::move(column_ptr), columns[index].type,
                                                         columns[index].name));
        index++;
    }
    block->swap(dst_block);
    *read_rows = block->rows();
    VLOG_DEBUG << "read block rows:" << *read_rows;
    return Status::OK();
}

Status WalReader::get_columns(std::unordered_map<std::string, TypeDescriptor>* name_to_type,
                              std::unordered_set<std::string>* missing_cols) {
    std::string col_ids;
    RETURN_IF_ERROR(_wal_reader->read_header(col_ids));
    std::vector<std::string> column_id_vector =
            strings::Split(col_ids, ",", strings::SkipWhitespace());
    try {
        int64_t pos = 0;
        for (auto col_id_str : column_id_vector) {
            auto col_id = std::strtoll(col_id_str.c_str(), NULL, 10);
            _column_pos_map.emplace(col_id, pos);
            pos++;
        }
    } catch (const std::invalid_argument& e) {
        return Status::InvalidArgument("Invalid format, {}", e.what());
    }
    return Status::OK();
}

} // namespace doris::vectorized
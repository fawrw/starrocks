// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exec/hash_join_node.cpp

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

#include "exec/hash_join_node.h"

#include <sstream>

#include "exec/hash_table.hpp"
#include "exprs/expr.h"
#include "exprs/in_predicate.h"
#include "exprs/slot_ref.h"
#include "gen_cpp/PlanNodes_types.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "util/runtime_profile.h"

namespace starrocks {

HashJoinNode::HashJoinNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ExecNode(pool, tnode, descs),
          _join_op(tnode.hash_join_node.join_op),
          _probe_eos(false),
          _anti_join_last_pos(NULL) {
    _match_all_probe = (_join_op == TJoinOp::LEFT_OUTER_JOIN || _join_op == TJoinOp::FULL_OUTER_JOIN);
    _match_one_build = (_join_op == TJoinOp::LEFT_SEMI_JOIN);
    _match_all_build = (_join_op == TJoinOp::RIGHT_OUTER_JOIN || _join_op == TJoinOp::FULL_OUTER_JOIN);
    _is_push_down = tnode.hash_join_node.is_push_down;
    _build_unique = _join_op == TJoinOp::LEFT_ANTI_JOIN || _join_op == TJoinOp::LEFT_SEMI_JOIN;
}

HashJoinNode::~HashJoinNode() {
    // _probe_batch must be cleaned up in close() to ensure proper resource freeing.
    DCHECK(_probe_batch == NULL);
}

Status HashJoinNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));
    DCHECK(tnode.__isset.hash_join_node);

    if (tnode.hash_join_node.__isset.sql_join_predicates) {
        _runtime_profile->add_info_string("JoinPredicates", tnode.hash_join_node.sql_join_predicates);
    }
    if (tnode.hash_join_node.__isset.sql_predicates) {
        _runtime_profile->add_info_string("Predicates", tnode.hash_join_node.sql_predicates);
    }

    const std::vector<TEqJoinCondition>& eq_join_conjuncts = tnode.hash_join_node.eq_join_conjuncts;

    for (int i = 0; i < eq_join_conjuncts.size(); ++i) {
        ExprContext* ctx = NULL;
        RETURN_IF_ERROR(Expr::create_expr_tree(_pool, eq_join_conjuncts[i].left, &ctx));
        _probe_expr_ctxs.push_back(ctx);
        RETURN_IF_ERROR(Expr::create_expr_tree(_pool, eq_join_conjuncts[i].right, &ctx));
        _build_expr_ctxs.push_back(ctx);
        if (eq_join_conjuncts[i].__isset.opcode && eq_join_conjuncts[i].opcode == TExprOpcode::EQ_FOR_NULL) {
            _is_null_safe_eq_join.push_back(true);
        } else {
            _is_null_safe_eq_join.push_back(false);
        }
    }

    RETURN_IF_ERROR(
            Expr::create_expr_trees(_pool, tnode.hash_join_node.other_join_conjuncts, &_other_join_conjunct_ctxs));

    if (!_other_join_conjunct_ctxs.empty()) {
        // If LEFT SEMI JOIN/LEFT ANTI JOIN with not equal predicate,
        // build table should not be deduplicated.
        _build_unique = false;
    }
    return Status::OK();
}

Status HashJoinNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::prepare(state));

    _build_pool.reset(new MemPool(mem_tracker()));
    _build_timer = ADD_TIMER(runtime_profile(), "BuildTime");
    _push_down_timer = ADD_TIMER(runtime_profile(), "PushDownTime");
    _push_compute_timer = ADD_TIMER(runtime_profile(), "PushDownComputeTime");
    _probe_timer = ADD_TIMER(runtime_profile(), "ProbeTime");
    _build_rows_counter = ADD_COUNTER(runtime_profile(), "BuildRows", TUnit::UNIT);
    _build_buckets_counter = ADD_COUNTER(runtime_profile(), "BuildBuckets", TUnit::UNIT);
    _probe_rows_counter = ADD_COUNTER(runtime_profile(), "ProbeRows", TUnit::UNIT);
    _hash_tbl_load_factor_counter = ADD_COUNTER(runtime_profile(), "LoadFactor", TUnit::DOUBLE_VALUE);

    // build and probe exprs are evaluated in the context of the rows produced by our
    // right and left children, respectively
    RETURN_IF_ERROR(Expr::prepare(_build_expr_ctxs, state, child(1)->row_desc(), expr_mem_tracker()));
    RETURN_IF_ERROR(Expr::prepare(_probe_expr_ctxs, state, child(0)->row_desc(), expr_mem_tracker()));

    // _other_join_conjuncts are evaluated in the context of the rows produced by this node
    RETURN_IF_ERROR(Expr::prepare(_other_join_conjunct_ctxs, state, _row_descriptor, expr_mem_tracker()));

    _result_tuple_row_size = _row_descriptor.tuple_descriptors().size() * sizeof(Tuple*);

    int num_left_tuples = child(0)->row_desc().tuple_descriptors().size();
    int num_build_tuples = child(1)->row_desc().tuple_descriptors().size();
    _probe_tuple_row_size = num_left_tuples * sizeof(Tuple*);
    _build_tuple_row_size = num_build_tuples * sizeof(Tuple*);

    // pre-compute the tuple index of build tuples in the output row
    _build_tuple_size = num_build_tuples;
    _build_tuple_idx.reserve(_build_tuple_size);

    for (int i = 0; i < _build_tuple_size; ++i) {
        TupleDescriptor* build_tuple_desc = child(1)->row_desc().tuple_descriptors()[i];
        _build_tuple_idx.push_back(_row_descriptor.get_tuple_idx(build_tuple_desc->id()));
    }

    // TODO: default buckets
    const bool stores_nulls = _join_op == TJoinOp::RIGHT_OUTER_JOIN || _join_op == TJoinOp::FULL_OUTER_JOIN ||
                              _join_op == TJoinOp::RIGHT_ANTI_JOIN || _join_op == TJoinOp::RIGHT_SEMI_JOIN ||
                              (std::find(_is_null_safe_eq_join.begin(), _is_null_safe_eq_join.end(), true) !=
                               _is_null_safe_eq_join.end());
    _hash_tbl.reset(new HashTable(_build_expr_ctxs, _probe_expr_ctxs, _build_tuple_size, stores_nulls,
                                  _is_null_safe_eq_join, id(), mem_tracker(), 1024));

    _probe_batch.reset(new RowBatch(child(0)->row_desc(), state->batch_size(), mem_tracker()));

    return Status::OK();
}

Status HashJoinNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }

    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::CLOSE));
    // Must reset _probe_batch in close() to release resources
    _probe_batch.reset(NULL);

    if (_hash_tbl.get() != NULL) {
        _hash_tbl->close();
    }
    if (_build_pool.get() != NULL) {
        _build_pool->free_all();
    }

    Expr::close(_build_expr_ctxs, state);
    Expr::close(_probe_expr_ctxs, state);
    Expr::close(_other_join_conjunct_ctxs, state);
#if 0
    for (auto iter : _push_down_expr_ctxs) {
        iter->close(state);
    }
#endif

    return ExecNode::close(state);
}

void HashJoinNode::build_side_thread(RuntimeState* state, boost::promise<Status>* status) {
    status->set_value(construct_hash_table(state));
    // Release the thread token as soon as possible (before the main thread joins
    // on it).  This way, if we had a chain of 10 joins using 1 additional thread,
    // we'd keep the additional thread busy the whole time.
    state->resource_pool()->release_thread_token(false);
}

Status HashJoinNode::construct_hash_table(RuntimeState* state) {
    // Do a full scan of child(1) and store everything in _hash_tbl
    // The hash join node needs to keep in memory all build tuples, including the tuple
    // row ptrs.  The row ptrs are copied into the hash table's internal structure so they
    // don't need to be stored in the _build_pool.
    RowBatch build_batch(child(1)->row_desc(), state->batch_size(), mem_tracker());
    RETURN_IF_ERROR(child(1)->open(state));

    while (true) {
        RETURN_IF_CANCELLED(state);
        bool eos = true;
        RETURN_IF_ERROR(child(1)->get_next(state, &build_batch, &eos));
        SCOPED_TIMER(_build_timer);
        // take ownership of tuple data of build_batch
        _build_pool->acquire_data(build_batch.tuple_data_pool(), false);
        RETURN_IF_LIMIT_EXCEEDED(state, "Hash join, while constructing the hash table.");

        process_build_batch(&build_batch);

        VLOG_ROW << _hash_tbl->debug_string(true, &child(1)->row_desc());

        COUNTER_SET(_build_rows_counter, _hash_tbl->size());
        COUNTER_SET(_build_buckets_counter, _hash_tbl->num_buckets());
        COUNTER_SET(_hash_tbl_load_factor_counter, _hash_tbl->load_factor());
        build_batch.reset();

        if (eos) {
            break;
        }
    }

    return Status::OK();
}

Status HashJoinNode::open(RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::open(state));
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::OPEN));
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(Expr::open(_build_expr_ctxs, state));
    RETURN_IF_ERROR(Expr::open(_probe_expr_ctxs, state));
    RETURN_IF_ERROR(Expr::open(_other_join_conjunct_ctxs, state));

    _eos = false;

    // TODO: fix problems with asynchronous cancellation
    // Kick-off the construction of the build-side table in a separate
    // thread, so that the left child can do any initialisation in parallel.
    // Only do this if we can get a thread token.  Otherwise, do this in the
    // main thread
    boost::promise<Status> thread_status;

    if (state->resource_pool()->try_acquire_thread_token()) {
        add_runtime_exec_option("Hash Table Built Asynchronously");
        boost::thread(bind(&HashJoinNode::build_side_thread, this, state, &thread_status));
    } else {
        thread_status.set_value(construct_hash_table(state));
    }

    if (_children[0]->type() == TPlanNodeType::EXCHANGE_NODE && _children[1]->type() == TPlanNodeType::EXCHANGE_NODE) {
        _is_push_down = false;
    }

    // The predicate could not be pushed down when there is Null-safe equal operator.
    // The in predicate will filter the null value in child[0] while it is needed in the Null-safe equal join.
    // For example: select * from a join b where a.id<=>b.id
    // the null value in table a should be return by scan node instead of filtering it by In-predicate.
    if (std::find(_is_null_safe_eq_join.begin(), _is_null_safe_eq_join.end(), true) != _is_null_safe_eq_join.end()) {
        _is_push_down = false;
    }

    // If child scan node is vectorized, disable scalar expr push down
    if (_check_has_vectorized_scan_child()) {
        _is_push_down = false;
    }

    if (_is_push_down) {
        // Blocks until ConstructHashTable has returned, after which
        // the hash table is fully constructed and we can start the probe
        // phase.
        RETURN_IF_ERROR(thread_status.get_future().get());

        if (_hash_tbl->size() == 0 && _join_op == TJoinOp::INNER_JOIN) {
            // Hash table size is zero
            LOG(INFO) << "No element need to push down, no need to read probe table";
            RETURN_IF_ERROR(child(0)->open(state));
            _probe_batch_pos = 0;
            _hash_tbl_iterator = _hash_tbl->begin();
            _eos = true;
            return Status::OK();
        }

        if (_hash_tbl->size() > 1024) {
            _is_push_down = false;
        }

        // TODO: this is used for Code Check, Remove this later
        if (_is_push_down || 0 != child(1)->conjunct_ctxs().size()) {
            for (int i = 0; i < _probe_expr_ctxs.size(); ++i) {
                TExprNode node;
                node.__set_node_type(TExprNodeType::IN_PRED);
                TScalarType tscalar_type;
                tscalar_type.__set_type(TPrimitiveType::BOOLEAN);
                TTypeNode ttype_node;
                ttype_node.__set_type(TTypeNodeType::SCALAR);
                ttype_node.__set_scalar_type(tscalar_type);
                TTypeDesc t_type_desc;
                t_type_desc.types.push_back(ttype_node);
                node.__set_type(t_type_desc);
                node.in_predicate.__set_is_not_in(false);
                node.__set_opcode(TExprOpcode::FILTER_IN);
                node.__isset.vector_opcode = true;
                node.__set_vector_opcode(to_in_opcode(_probe_expr_ctxs[i]->root()->type().type));
                // NOTE(zc): in predicate only used here, no need prepare.
                InPredicate* in_pred = _pool->add(new InPredicate(node));
                RETURN_IF_ERROR(in_pred->prepare(state, _probe_expr_ctxs[i]->root()->type()));
                in_pred->add_child(Expr::copy(_pool, _probe_expr_ctxs[i]->root()));
                ExprContext* ctx = _pool->add(new ExprContext(in_pred));
                _push_down_expr_ctxs.push_back(ctx);
            }

            {
                SCOPED_TIMER(_push_compute_timer);
                HashTable::Iterator iter = _hash_tbl->begin();

                while (iter.has_next()) {
                    TupleRow* row = iter.get_row();
                    std::list<ExprContext*>::iterator ctx_iter = _push_down_expr_ctxs.begin();

                    for (int i = 0; i < _build_expr_ctxs.size(); ++i, ++ctx_iter) {
                        void* val = _build_expr_ctxs[i]->get_value(row);
                        InPredicate* in_pre = (InPredicate*)((*ctx_iter)->root());
                        in_pre->insert(val);
                    }

                    SCOPED_TIMER(_build_timer);
                    iter.next<false>();
                }
            }

            SCOPED_TIMER(_push_down_timer);
            push_down_predicate(state, &_push_down_expr_ctxs, false);
        }

        // Open the probe-side child so that it may perform any initialisation in parallel.
        // Don't exit even if we see an error, we still need to wait for the build thread
        // to finish.
        Status open_status = child(0)->open(state);
        RETURN_IF_ERROR(open_status);
    } else {
        // Open the probe-side child so that it may perform any initialisation in parallel.
        // Don't exit even if we see an error, we still need to wait for the build thread
        // to finish.
        Status open_status = child(0)->open(state);

        // Blocks until ConstructHashTable has returned, after which
        // the hash table is fully constructed and we can start the probe
        // phase.
        RETURN_IF_ERROR(thread_status.get_future().get());

        // ISSUE-1247, check open_status after buildThread execute.
        // If this return first, build thread will use 'thread_status'
        // which is already destructor and then coredump.
        RETURN_IF_ERROR(open_status);
    }

    // seed probe batch and _current_probe_row, etc.
    while (true) {
        RETURN_IF_ERROR(child(0)->get_next(state, _probe_batch.get(), &_probe_eos));
        COUNTER_UPDATE(_probe_rows_counter, _probe_batch->num_rows());
        _probe_batch_pos = 0;

        if (_probe_batch->num_rows() == 0) {
            if (_probe_eos) {
                _hash_tbl_iterator = _hash_tbl->begin();
                _eos = true;
                break;
            }

            _probe_batch->reset();
            continue;
        } else {
            _current_probe_row = _probe_batch->get_row(_probe_batch_pos++);
            VLOG_ROW << "probe row: " << get_probe_row_output_string(_current_probe_row);
            _matched_probe = false;
            _hash_tbl_iterator = _hash_tbl->find(_current_probe_row);
            break;
        }
    }

    return Status::OK();
}

Status HashJoinNode::get_next(RuntimeState* state, RowBatch* out_batch, bool* eos) {
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::GETNEXT));
    RETURN_IF_CANCELLED(state);
    SCOPED_TIMER(_runtime_profile->total_time_counter());

    if (reached_limit()) {
        *eos = true;
        return Status::OK();
    }

    // These cases are simpler and use a more efficient processing loop
    if (!(_match_all_build || _join_op == TJoinOp::RIGHT_SEMI_JOIN || _join_op == TJoinOp::RIGHT_ANTI_JOIN)) {
        if (_eos) {
            *eos = true;
            return Status::OK();
        }

        return left_join_get_next(state, out_batch, eos);
    }

    ExprContext* const* other_conjunct_ctxs = &_other_join_conjunct_ctxs[0];
    int num_other_conjunct_ctxs = _other_join_conjunct_ctxs.size();

    ExprContext* const* conjunct_ctxs = &_conjunct_ctxs[0];
    int num_conjunct_ctxs = _conjunct_ctxs.size();

    // Explicitly manage the timer counter to avoid measuring time in the child
    // GetNext call.
    ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);

    while (!_eos) {
        // create output rows as long as:
        // 1) we haven't already created an output row for the probe row and are doing
        //    a semi-join;
        // 2) there are more matching build rows
        VLOG_ROW << "probe row: " << get_probe_row_output_string(_current_probe_row);
        while (_hash_tbl_iterator.has_next()) {
            TupleRow* matched_build_row = _hash_tbl_iterator.get_row();
            VLOG_ROW << "matched_build_row: " << matched_build_row->to_string(child(1)->row_desc());

            if ((_join_op == TJoinOp::RIGHT_ANTI_JOIN || _join_op == TJoinOp::RIGHT_SEMI_JOIN) &&
                _hash_tbl_iterator.matched()) {
                // We have already matched this build row, continue to next match.
                // _hash_tbl_iterator.next<true>();
                _hash_tbl_iterator.next<true>();
                continue;
            }

            int row_idx = out_batch->add_row();
            TupleRow* out_row = out_batch->get_row(row_idx);

            // right anti join
            // 1. find pos in hash table which meets equi-join
            // 2. judge if set matched with other join predicates
            // 3. scans hash table to choose row which is't set matched and meets conjuncts
            if (_join_op == TJoinOp::RIGHT_ANTI_JOIN) {
                create_output_row(out_row, _current_probe_row, matched_build_row);
                if (eval_conjuncts(other_conjunct_ctxs, num_other_conjunct_ctxs, out_row)) {
                    _hash_tbl_iterator.set_matched();
                }
                _hash_tbl_iterator.next<true>();
                continue;
            } else {
                // right semi join
                // 1. find pos in hash table which meets equi-join and set_matched
                // 2. check if the row meets other join predicates
                // 3. check if the row meets conjuncts
                // right join and full join
                // 1. find pos in hash table which meets equi-join
                // 2. check if the row meets other join predicates
                // 3. check if the row meets conjuncts
                // 4. output left and right meeting other predicates and conjuncts
                // 5. if full join, output left meeting and right no meeting other
                // join predicates and conjuncts
                // 6. output left no meeting and right meeting other join predicate
                // and conjuncts
                create_output_row(out_row, _current_probe_row, matched_build_row);
            }

            if (!eval_conjuncts(other_conjunct_ctxs, num_other_conjunct_ctxs, out_row)) {
                _hash_tbl_iterator.next<true>();
                continue;
            }

            if (_join_op == TJoinOp::RIGHT_SEMI_JOIN) {
                _hash_tbl_iterator.set_matched();
            }

            // we have a match for the purpose of the (outer?) join as soon as we
            // satisfy the JOIN clause conjuncts
            _matched_probe = true;

            if (_match_all_build) {
                // remember that we matched this build row
                _joined_build_rows.insert(matched_build_row);
                VLOG_ROW << "joined build row: " << matched_build_row;
            }

            _hash_tbl_iterator.next<true>();
            if (eval_conjuncts(conjunct_ctxs, num_conjunct_ctxs, out_row)) {
                out_batch->commit_last_row();
                VLOG_ROW << "match row: " << out_row->to_string(row_desc());
                ++_num_rows_returned;
                COUNTER_SET(_rows_returned_counter, _num_rows_returned);

                if (out_batch->is_full() || reached_limit()) {
                    *eos = reached_limit();
                    return Status::OK();
                }
            }
        }

        // check whether we need to output the current probe row before
        // getting a new probe batch
        if (_match_all_probe && !_matched_probe) {
            int row_idx = out_batch->add_row();
            TupleRow* out_row = out_batch->get_row(row_idx);
            create_output_row(out_row, _current_probe_row, NULL);

            if (eval_conjuncts(conjunct_ctxs, num_conjunct_ctxs, out_row)) {
                out_batch->commit_last_row();
                VLOG_ROW << "match row: " << out_row->to_string(row_desc());
                ++_num_rows_returned;
                COUNTER_SET(_rows_returned_counter, _num_rows_returned);
                _matched_probe = true;

                if (out_batch->is_full() || reached_limit()) {
                    *eos = reached_limit();
                    return Status::OK();
                }
            }
        }

        if (_probe_batch_pos == _probe_batch->num_rows()) {
            // pass on resources, out_batch might still need them
            _probe_batch->transfer_resource_ownership(out_batch);
            _probe_batch_pos = 0;

            if (out_batch->is_full() || out_batch->at_resource_limit()) {
                return Status::OK();
            }

            // get new probe batch
            if (!_probe_eos) {
                while (true) {
                    probe_timer.stop();
                    RETURN_IF_ERROR(child(0)->get_next(state, _probe_batch.get(), &_probe_eos));
                    probe_timer.start();

                    if (_probe_batch->num_rows() == 0) {
                        // Empty batches can still contain IO buffers, which need to be passed up to
                        // the caller; transferring resources can fill up out_batch.
                        _probe_batch->transfer_resource_ownership(out_batch);

                        if (_probe_eos) {
                            _eos = true;
                            break;
                        }

                        if (out_batch->is_full() || out_batch->at_resource_limit()) {
                            return Status::OK();
                        }

                        continue;
                    } else {
                        COUNTER_UPDATE(_probe_rows_counter, _probe_batch->num_rows());
                        break;
                    }
                }
            } else {
                _eos = true;
            }

            // finish up right outer join
            if (_eos && (_match_all_build || _join_op == TJoinOp::RIGHT_ANTI_JOIN)) {
                _hash_tbl_iterator = _hash_tbl->begin();
            }
        }

        if (_eos) {
            break;
        }

        // join remaining rows in probe _batch
        _current_probe_row = _probe_batch->get_row(_probe_batch_pos++);
        VLOG_ROW << "probe row: " << get_probe_row_output_string(_current_probe_row);
        _matched_probe = false;
        _hash_tbl_iterator = _hash_tbl->find(_current_probe_row);
    }

    *eos = true;
    if (_match_all_build || _join_op == TJoinOp::RIGHT_ANTI_JOIN) {
        // output remaining unmatched build rows
        TupleRow* build_row = NULL;
        if (_join_op == TJoinOp::RIGHT_ANTI_JOIN) {
            if (_anti_join_last_pos != NULL) {
                _hash_tbl_iterator = *_anti_join_last_pos;
            } else {
                _hash_tbl_iterator = _hash_tbl->begin();
            }
        }
        while (!out_batch->is_full() && _hash_tbl_iterator.has_next()) {
            build_row = _hash_tbl_iterator.get_row();

            if (_match_all_build) {
                if (_joined_build_rows.find(build_row) != _joined_build_rows.end()) {
                    _hash_tbl_iterator.next<false>();
                    continue;
                }
            } else if (_join_op == TJoinOp::RIGHT_ANTI_JOIN) {
                if (_hash_tbl_iterator.matched()) {
                    _hash_tbl_iterator.next<false>();
                    continue;
                }
            }

            int row_idx = out_batch->add_row();
            TupleRow* out_row = out_batch->get_row(row_idx);
            create_output_row(out_row, NULL, build_row);
            if (eval_conjuncts(conjunct_ctxs, num_conjunct_ctxs, out_row)) {
                out_batch->commit_last_row();
                VLOG_ROW << "match row: " << out_row->to_string(row_desc());
                ++_num_rows_returned;
                COUNTER_SET(_rows_returned_counter, _num_rows_returned);

                if (reached_limit()) {
                    *eos = true;
                    return Status::OK();
                }
            }
            _hash_tbl_iterator.next<false>();
        }
        if (_join_op == TJoinOp::RIGHT_ANTI_JOIN) {
            _anti_join_last_pos = &_hash_tbl_iterator;
        }
        // we're done if there are no more rows left to check
        *eos = !_hash_tbl_iterator.has_next();
    }

    return Status::OK();
}

Status HashJoinNode::left_join_get_next(RuntimeState* state, RowBatch* out_batch, bool* eos) {
    *eos = _eos;

    ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);

    while (!_eos) {
        // Compute max rows that should be added to out_batch
        int64_t max_added_rows = out_batch->capacity() - out_batch->num_rows();

        if (limit() != -1) {
            max_added_rows = std::min(max_added_rows, limit() - rows_returned());
        }

        // Continue processing this row batch
        _num_rows_returned += process_probe_batch(out_batch, _probe_batch.get(), max_added_rows);
        COUNTER_SET(_rows_returned_counter, _num_rows_returned);

        if (reached_limit() || out_batch->is_full()) {
            *eos = reached_limit();
            break;
        }

        // Check to see if we're done processing the current probe batch
        if (!_hash_tbl_iterator.has_next() && _probe_batch_pos == _probe_batch->num_rows()) {
            _probe_batch->transfer_resource_ownership(out_batch);
            _probe_batch_pos = 0;

            if (out_batch->is_full() || out_batch->at_resource_limit()) {
                break;
            }

            if (_probe_eos) {
                *eos = _eos = true;
                break;
            } else {
                probe_timer.stop();
                RETURN_IF_ERROR(child(0)->get_next(state, _probe_batch.get(), &_probe_eos));
                probe_timer.start();
                COUNTER_UPDATE(_probe_rows_counter, _probe_batch->num_rows());
            }
        }
    }

    return Status::OK();
}

std::string HashJoinNode::get_probe_row_output_string(TupleRow* probe_row) {
    std::stringstream out;
    out << "[";
    int* _build_tuple_idx_ptr = &_build_tuple_idx[0];

    for (int i = 0; i < row_desc().tuple_descriptors().size(); ++i) {
        if (i != 0) {
            out << " ";
        }

        int* is_build_tuple = std::find(_build_tuple_idx_ptr, _build_tuple_idx_ptr + _build_tuple_size, i);

        if (is_build_tuple != _build_tuple_idx_ptr + _build_tuple_size) {
            out << Tuple::to_string(NULL, *row_desc().tuple_descriptors()[i]);
        } else {
            out << Tuple::to_string(probe_row->get_tuple(i), *row_desc().tuple_descriptors()[i]);
        }
    }

    out << "]";
    return out.str();
}

void HashJoinNode::debug_string(int indentation_level, std::stringstream* out) const {
    *out << string(indentation_level * 2, ' ');
    *out << "_hashJoin(eos=" << (_eos ? "true" : "false") << " probe_batch_pos=" << _probe_batch_pos << " hash_tbl=";
    *out << string(indentation_level * 2, ' ');
    *out << "HashTbl(";
    // << " build_exprs=" << Expr::debug_string(_build_expr_ctxs)
    // << " probe_exprs=" << Expr::debug_string(_probe_expr_ctxs);
    *out << ")";
    ExecNode::debug_string(indentation_level, out);
    *out << ")";
}

// This function is replaced by codegen
void HashJoinNode::create_output_row(TupleRow* out, TupleRow* probe, TupleRow* build) {
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(out);
    if (probe == NULL) {
        memset(out_ptr, 0, _probe_tuple_row_size);
    } else {
        memcpy(out_ptr, probe, _probe_tuple_row_size);
    }

    if (build == NULL) {
        memset(out_ptr + _probe_tuple_row_size, 0, _build_tuple_row_size);
    } else {
        memcpy(out_ptr + _probe_tuple_row_size, build, _build_tuple_row_size);
    }
}

} // namespace starrocks
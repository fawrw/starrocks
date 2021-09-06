// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/data_stream_sender.cpp

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

#include "runtime/data_stream_sender.h"

#include <arpa/inet.h>
#include <thrift/protocol/TDebugProtocol.h>

#include <algorithm>
#include <boost/thread/thread.hpp>
#include <functional>
#include <iostream>
#include <memory>

#include "column/chunk.h"
#include "common/logging.h"
#include "exprs/expr.h"
#include "gen_cpp/BackendService.h"
#include "gen_cpp/Types_types.h"
#include "runtime/client_cache.h"
#include "runtime/descriptors.h"
#include "runtime/dpp_sink_internal.h"
#include "runtime/exec_env.h"
#include "runtime/mem_tracker.h"
#include "runtime/raw_value.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "runtime/tuple_row.h"
#include "service/brpc.h"
#include "util/block_compression.h"
#include "util/brpc_stub_cache.h"
#include "util/compression_utils.h"
#include "util/debug_util.h"
#include "util/network_util.h"
#include "util/ref_count_closure.h"
#include "util/thrift_client.h"
#include "util/thrift_util.h"

namespace starrocks {

// A channel sends data asynchronously via calls to transmit_data
// to a single destination ipaddress/node.
// It has a fixed-capacity buffer and allows the caller either to add rows to
// that buffer individually (AddRow()), or circumvent the buffer altogether and send
// TRowBatches directly (SendBatch()). Either way, there can only be one in-flight RPC
// at any one time (ie, sending will block if the most recent rpc hasn't finished,
// which allows the receiver node to throttle the sender by withholding acks).
// *Not* thread-safe.
class DataStreamSender::Channel {
public:
    // Create channel to send data to particular ipaddress/port/query/node
    // combination. buffer_size is specified in bytes and a soft limit on
    // how much tuple data is getting accumulated before being sent; it only applies
    // when data is added via add_row() and not sent directly via send_batch().
    Channel(DataStreamSender* parent, const RowDescriptor& row_desc, const TNetworkAddress& brpc_dest,
            const TUniqueId& fragment_instance_id, PlanNodeId dest_node_id, int buffer_size, bool is_transfer_chain,
            bool send_query_statistics_with_every_batch)
            : _parent(parent),
              _buffer_size(buffer_size),
              _row_desc(row_desc),
              _fragment_instance_id(fragment_instance_id),
              _dest_node_id(dest_node_id),
              _num_data_bytes_sent(0),
              _request_seq(0),
              _need_close(false),
              _brpc_dest_addr(brpc_dest),
              _is_transfer_chain(is_transfer_chain),
              _send_query_statistics_with_every_batch(send_query_statistics_with_every_batch) {}

    virtual ~Channel() {
        if (_closure != nullptr && _closure->unref()) {
            delete _closure;
        }
        // release this before request desctruct
        _brpc_request.release_finst_id();

        if (_chunk_closure != nullptr && _chunk_closure->unref()) {
            delete _chunk_closure;
        }
        _chunk_request.release_finst_id();
    }

    // Initialize channel.
    // Returns OK if successful, error indication otherwise.
    Status init(RuntimeState* state);

    // Copies a single row into this channel's output buffer and flushes buffer
    // if it reaches capacity.
    // Returns error status if any of the preceding rpcs failed, OK otherwise.
    Status add_row(TupleRow* row);

    // Used when doing shuffle.
    // This function will copy selective rows in chunks to batch.
    // indexes contains row index of chunk and this function will copy from input
    // 'from' and copy 'size' rows
    Status add_rows_selective(vectorized::Chunk* chunk, const uint32_t* row_indexes, uint32_t from, uint32_t size);

    // Asynchronously sends a row batch.
    // Returns the status of the most recently finished transmit_data
    // rpc (or OK if there wasn't one that hasn't been reported yet).
    // if batch is nullptr, send the eof packet
    Status send_batch(PRowBatch* batch, bool eos = false);

    // Send one chunk to remote, this chunk may be batched in this channel.
    // When the chunk is sent really rather than bachend, *is_real_sent will
    // be set to true.
    Status send_one_chunk(const vectorized::Chunk* chunk, bool eos, bool* is_real_sent);

    // Channel will sent input request directly without batch it.
    // This function is only used when broadcast, because request can be reused
    // by all the channels.
    Status send_chunk_request(PTransmitChunkParams* params, const butil::IOBuf& attachment);

    // Flush buffered rows and close channel. This function don't wait the response
    // of close operation, client should call close_wait() to finish channel's close.
    // We split one close operation into two phases in order to make multiple channels
    // can run parallel.
    void close(RuntimeState* state);

    // Get close wait's response, to finish channel close operation.
    void close_wait(RuntimeState* state);

    int64_t num_data_bytes_sent() const { return _num_data_bytes_sent; }

    PRowBatch* pb_batch() { return &_pb_batch; }

    std::string get_fragment_instance_id_str() {
        UniqueId uid(_fragment_instance_id);
        return uid.to_string();
    }

    TUniqueId get_fragment_instance_id() { return _fragment_instance_id; }

private:
    inline Status _wait_last_brpc() {
        auto cntl = &_closure->cntl;
        brpc::Join(cntl->call_id());
        if (cntl->Failed()) {
            LOG(WARNING) << "fail to send brpc batch, error=" << berror(cntl->ErrorCode())
                         << ", error_text=" << cntl->ErrorText();
            return Status::ThriftRpcError("fail to send batch");
        } else {
            return {_closure->result.status()};
        }
    }

    inline Status _wait_prev_request() {
        SCOPED_TIMER(_parent->_wait_response_timer);
        if (_request_seq == 0) {
            return Status::OK();
        }
        auto cntl = &_chunk_closure->cntl;
        brpc::Join(cntl->call_id());
        if (cntl->Failed()) {
            LOG(WARNING) << "fail to send brpc batch, error=" << berror(cntl->ErrorCode())
                         << ", error_text=" << cntl->ErrorText();
            return Status::ThriftRpcError("fail to send batch");
        }
        return {_chunk_closure->result.status()};
    }

private:
    // Serialize _batch into _thrift_batch and send via send_batch().
    // Returns send_batch() status.
    Status send_current_batch(bool eos = false);
    Status _send_current_chunk(bool eos);

    Status _do_send_chunk_rpc(PTransmitChunkParams* request, const butil::IOBuf& attachment);

    Status close_internal();

    DataStreamSender* _parent;
    int _buffer_size;

    const RowDescriptor& _row_desc;
    TUniqueId _fragment_instance_id;
    PlanNodeId _dest_node_id;

    // the number of TRowBatch.data bytes sent successfully
    int64_t _num_data_bytes_sent;
    int64_t _request_seq;

    // we're accumulating rows into this batch
    std::unique_ptr<RowBatch> _batch;
    std::unique_ptr<vectorized::Chunk> _chunk;
    bool _is_first_chunk = true;

    bool _need_close;

    TNetworkAddress _brpc_dest_addr;

    // TODO(zc): initused for brpc
    PUniqueId _finst_id;
    PRowBatch _pb_batch;

    PTransmitDataParams _brpc_request;

    // Used to transmit chunk. We use this struct in a round robin way.
    // When one request is being send, producer will construct the other one.
    // Which one is used is decided by _request_seq.
    PTransmitChunkParams _chunk_request;
    RefCountClosure<PTransmitChunkResult>* _chunk_closure = nullptr;

    size_t _current_request_bytes = 0;

    PBackendService_Stub* _brpc_stub = nullptr;
    RefCountClosure<PTransmitDataResult>* _closure = nullptr;

    int32_t _brpc_timeout_ms = 500;
    // whether the dest can be treated as query statistics transfer chain.
    bool _is_transfer_chain;
    bool _send_query_statistics_with_every_batch;
    bool _is_inited = false;
};

Status DataStreamSender::Channel::init(RuntimeState* state) {
    if (_is_inited) {
        return Status::OK();
    }
    // TODO: figure out how to size _batch
    int capacity = std::max(1, _buffer_size / std::max(_row_desc.get_row_size(), 1));
    _batch = std::make_unique<RowBatch>(_row_desc, capacity, _parent->_mem_tracker.get());

    if (_brpc_dest_addr.hostname.empty()) {
        LOG(WARNING) << "there is no brpc destination address's hostname"
                        ", maybe version is not compatible.";
        return Status::InternalError("no brpc destination");
    }

    // initialize brpc request
    _finst_id.set_hi(_fragment_instance_id.hi);
    _finst_id.set_lo(_fragment_instance_id.lo);
    _brpc_request.set_allocated_finst_id(&_finst_id);
    _brpc_request.set_node_id(_dest_node_id);
    _brpc_request.set_sender_id(_parent->_sender_id);
    _brpc_request.set_be_number(_parent->_be_number);

    _chunk_request.set_allocated_finst_id(&_finst_id);
    _chunk_request.set_node_id(_dest_node_id);
    _chunk_request.set_sender_id(_parent->_sender_id);
    _chunk_request.set_be_number(_parent->_be_number);

    _chunk_closure = new RefCountClosure<PTransmitChunkResult>();
    _chunk_closure->ref();

    _brpc_timeout_ms = std::min(3600, state->query_options().query_timeout) * 1000;
    // For bucket shuffle, the dest is unreachable, there is no need to establish a connection
    if (_fragment_instance_id.lo == -1) {
        _is_inited = true;
        return Status::OK();
    }
    _brpc_stub = state->exec_env()->brpc_stub_cache()->get_stub(_brpc_dest_addr);

    _need_close = true;
    _is_inited = true;
    return Status::OK();
}

Status DataStreamSender::Channel::send_batch(PRowBatch* batch, bool eos) {
    if (_closure == nullptr) {
        _closure = new RefCountClosure<PTransmitDataResult>();
        _closure->ref();
    } else {
        RETURN_IF_ERROR(_wait_last_brpc());
        _closure->cntl.Reset();
    }
    VLOG_ROW << "Channel::send_batch() instance_id=" << _fragment_instance_id << " dest_node=" << _dest_node_id;
    if (_is_transfer_chain && (_send_query_statistics_with_every_batch || eos)) {
        auto statistic = _brpc_request.mutable_query_statistics();
        _parent->_query_statistics->to_pb(statistic);
    }

    _brpc_request.set_eos(eos);
    if (batch != nullptr) {
        _brpc_request.set_allocated_row_batch(batch);
    }
    _brpc_request.set_packet_seq(_request_seq++);

    _closure->ref();
    _closure->cntl.set_timeout_ms(_brpc_timeout_ms);
    _brpc_stub->transmit_data(&_closure->cntl, &_brpc_request, &_closure->result, _closure);
    if (batch != nullptr) {
        _brpc_request.release_row_batch();
    }
    return Status::OK();
}

Status DataStreamSender::Channel::send_one_chunk(const vectorized::Chunk* chunk, bool eos, bool* is_real_sent) {
    *is_real_sent = false;

    // If chunk is not null, append it to request
    if (chunk != nullptr) {
        auto pchunk = _chunk_request.add_chunks();
        RETURN_IF_ERROR(_parent->serialize_chunk(chunk, pchunk, &_is_first_chunk));
        _current_request_bytes += pchunk->data().size();
    }

    // Try to accumulate enough bytes before sending a RPC. When eos is true we should send
    // last packet
    if (_current_request_bytes > _parent->_request_bytes_threshold || eos) {
        // NOTE: Before we send current request, we must wait last RPC's result to make sure
        // it have finished. Because in some cases, receiver depend the order of sender data.
        // We can add KeepOrder flag in Frontend to tell sender if it can send packet before
        // last RPC return. Then we can have a better pipeline. Before that we make wait last
        // RPC first.
        RETURN_IF_ERROR(_wait_prev_request());
        _chunk_request.set_eos(eos);
        // we will send the current request now
        butil::IOBuf attachment;
        _parent->construct_brpc_attachment(&_chunk_request, &attachment);
        RETURN_IF_ERROR(_do_send_chunk_rpc(&_chunk_request, attachment));
        // lets request sequence increment
        _chunk_request.clear_chunks();
        _current_request_bytes = 0;
        *is_real_sent = true;
    }

    return Status::OK();
}

Status DataStreamSender::Channel::send_chunk_request(PTransmitChunkParams* params, const butil::IOBuf& attachment) {
    RETURN_IF_ERROR(_wait_prev_request());
    params->set_allocated_finst_id(&_finst_id);
    params->set_node_id(_dest_node_id);
    params->set_sender_id(_parent->_sender_id);
    params->set_be_number(_parent->_be_number);
    auto status = _do_send_chunk_rpc(params, attachment);
    params->release_finst_id();
    return status;
}

Status DataStreamSender::Channel::_do_send_chunk_rpc(PTransmitChunkParams* request, const butil::IOBuf& attachment) {
    SCOPED_TIMER(_parent->_send_request_timer);

    request->set_sequence(_request_seq);
    if (_is_transfer_chain && (_send_query_statistics_with_every_batch || request->eos())) {
        auto statistic = request->mutable_query_statistics();
        _parent->_query_statistics->to_pb(statistic);
    }
    _chunk_closure->ref();
    _chunk_closure->cntl.Reset();
    _chunk_closure->cntl.set_timeout_ms(_brpc_timeout_ms);
    _chunk_closure->cntl.request_attachment().append(attachment);
    _brpc_stub->transmit_chunk(&_chunk_closure->cntl, request, &_chunk_closure->result, _chunk_closure);
    _request_seq++;
    return Status::OK();
}

Status DataStreamSender::Channel::add_row(TupleRow* row) {
    int row_num = _batch->add_row();

    if (row_num == RowBatch::INVALID_ROW_INDEX) {
        // _batch is full, let's send it; but first wait for an ongoing
        // transmission to finish before modifying _thrift_batch
        RETURN_IF_ERROR(send_current_batch());
        row_num = _batch->add_row();
        DCHECK_NE(row_num, RowBatch::INVALID_ROW_INDEX);
    }

    TupleRow* dest = _batch->get_row(row_num);
    _batch->copy_row(row, dest);
    const std::vector<TupleDescriptor*>& descs = _row_desc.tuple_descriptors();

    for (int i = 0; i < descs.size(); ++i) {
        if (UNLIKELY(row->get_tuple(i) == NULL)) {
            dest->set_tuple(i, NULL);
        } else {
            dest->set_tuple(i, row->get_tuple(i)->deep_copy(*descs[i], _batch->tuple_data_pool()));
        }
    }

    _batch->commit_last_row();
    return Status::OK();
}

Status DataStreamSender::Channel::add_rows_selective(vectorized::Chunk* chunk, const uint32_t* indexes, uint32_t from,
                                                     uint32_t size) {
    // TODO(kks): find a way to remove this if condition
    if (UNLIKELY(_chunk == nullptr)) {
        _chunk = chunk->clone_empty_with_tuple();
    }

    if (_chunk->num_rows() + size > config::vector_chunk_size) {
        // _chunk is full, let's send it; but first wait for an ongoing
        // transmission to finish before modifying _pb_chunk
        RETURN_IF_ERROR(_send_current_chunk(false));
        DCHECK_EQ(0, _chunk->num_rows());
    }

    _chunk->append_selective(*chunk, indexes, from, size);
    return Status::OK();
}

Status DataStreamSender::Channel::send_current_batch(bool eos) {
    _parent->serialize_batch(_batch.get(), &_pb_batch, 1);
    _batch->reset();
    RETURN_IF_ERROR(send_batch(&_pb_batch, eos));
    return Status::OK();
}

Status DataStreamSender::Channel::_send_current_chunk(bool eos) {
    bool is_real_sent = false;
    RETURN_IF_ERROR(send_one_chunk(_chunk.get(), eos, &is_real_sent));

    // we only clear column data, because we need to reuse column schema
    for (ColumnPtr& column : _chunk->columns()) {
        column->resize(0);
    }
    return Status::OK();
}

Status DataStreamSender::Channel::close_internal() {
    if (!_need_close) {
        return Status::OK();
    }

    if (_parent->_is_vectorized) {
        VLOG_RPC << "_chunk Channel::close() instance_id=" << _fragment_instance_id << " dest_node=" << _dest_node_id
                 << " #rows= " << ((_chunk == nullptr) ? 0 : _chunk->num_rows());
        if (_chunk != nullptr && _chunk->num_rows() > 0) {
            RETURN_IF_ERROR(_send_current_chunk(true));
        } else {
            bool is_real_sent = false;
            RETURN_IF_ERROR(send_one_chunk(nullptr, true, &is_real_sent));
        }
    } else {
        VLOG_RPC << "Channel::close() instance_id=" << _fragment_instance_id << " dest_node=" << _dest_node_id
                 << " #rows= " << ((_batch == nullptr) ? 0 : _batch->num_rows());
        if (_batch != nullptr && _batch->num_rows() > 0) {
            RETURN_IF_ERROR(send_current_batch(true));
        } else {
            RETURN_IF_ERROR(send_batch(nullptr, true));
        }
    }
    // Don't wait for the last packet to finish, left it to close_wait.
    return Status::OK();
}

void DataStreamSender::Channel::close(RuntimeState* state) {
    state->log_error(close_internal().get_error_msg());
}

void DataStreamSender::Channel::close_wait(RuntimeState* state) {
    if (_need_close) {
        if (_parent->_is_vectorized) {
            auto st = _wait_prev_request();
            if (!st.ok()) {
                LOG(WARNING) << "fail to close channel, st=" << st.to_string()
                             << ", instance_id=" << print_id(_fragment_instance_id)
                             << ", dest=" << _brpc_dest_addr.hostname << ":" << _brpc_dest_addr.port;
                if (_parent->_close_status.ok()) {
                    _parent->_close_status = st;
                }
            }
        } else {
            state->log_error(_wait_last_brpc().get_error_msg());
        }
        _need_close = false;
    }
    _batch.reset();
    _chunk.reset();
}

DataStreamSender::DataStreamSender(ObjectPool* pool, bool is_vectorized, int sender_id, const RowDescriptor& row_desc,
                                   const TDataStreamSink& sink,
                                   const std::vector<TPlanFragmentDestination>& destinations,
                                   int per_channel_buffer_size, bool send_query_statistics_with_every_batch)
        : _is_vectorized(is_vectorized),
          _sender_id(sender_id),
          _pool(pool),
          _row_desc(row_desc),
          _current_channel_idx(0),
          _part_type(sink.output_partition.type),
          _ignore_not_found(!sink.__isset.ignore_not_found || sink.ignore_not_found),
          _current_pb_batch(&_pb_batch1),
          _profile(NULL),
          _serialize_batch_timer(NULL),
          _bytes_sent_counter(NULL),
          _dest_node_id(sink.dest_node_id) {
    DCHECK_GT(destinations.size(), 0);
    DCHECK(sink.output_partition.type == TPartitionType::UNPARTITIONED ||
           sink.output_partition.type == TPartitionType::HASH_PARTITIONED ||
           sink.output_partition.type == TPartitionType::RANDOM ||
           sink.output_partition.type == TPartitionType::RANGE_PARTITIONED ||
           sink.output_partition.type == TPartitionType::BUCKET_SHFFULE_HASH_PARTITIONED);
    // TODO: use something like google3's linked_ptr here (std::unique_ptr isn't copyable

    std::map<int64_t, int64_t> fragment_id_to_channel_index;
    for (int i = 0; i < destinations.size(); ++i) {
        // Select first dest as transfer chain.
        bool is_transfer_chain = (i == 0);
        const auto& fragment_instance_id = destinations[i].fragment_instance_id;
        if (fragment_id_to_channel_index.find(fragment_instance_id.lo) == fragment_id_to_channel_index.end()) {
            _channel_shared_ptrs.emplace_back(
                    new Channel(this, row_desc, destinations[i].brpc_server, fragment_instance_id, sink.dest_node_id,
                                per_channel_buffer_size, is_transfer_chain, send_query_statistics_with_every_batch));
            fragment_id_to_channel_index.insert({fragment_instance_id.lo, _channel_shared_ptrs.size() - 1});
            _channels.push_back(_channel_shared_ptrs.back().get());
        } else {
            _channel_shared_ptrs.emplace_back(
                    _channel_shared_ptrs[fragment_id_to_channel_index[fragment_instance_id.lo]]);
            _channels.push_back(_channel_shared_ptrs.back().get());
        }
    }
    _request_bytes_threshold = config::max_transmit_batched_bytes;
}

// We use the PartitionRange to compare here. It should not be a member function of PartitionInfo
// class because there are some other member in it.
static bool compare_part_use_range(const PartitionInfo* v1, const PartitionInfo* v2) {
    return v1->range() < v2->range();
}

Status DataStreamSender::init(const TDataSink& tsink) {
    RETURN_IF_ERROR(DataSink::init(tsink));
    const TDataStreamSink& t_stream_sink = tsink.stream_sink;
    if (_part_type == TPartitionType::HASH_PARTITIONED ||
        _part_type == TPartitionType::BUCKET_SHFFULE_HASH_PARTITIONED) {
        RETURN_IF_ERROR(
                Expr::create_expr_trees(_pool, t_stream_sink.output_partition.partition_exprs, &_partition_expr_ctxs));
    } else if (_part_type == TPartitionType::RANGE_PARTITIONED) {
        // Range partition
        // Partition Exprs
        RETURN_IF_ERROR(
                Expr::create_expr_trees(_pool, t_stream_sink.output_partition.partition_exprs, &_partition_expr_ctxs));
        // Partition infos
        int num_parts = t_stream_sink.output_partition.partition_infos.size();
        if (num_parts == 0) {
            return Status::InternalError("Empty partition info.");
        }
        for (int i = 0; i < num_parts; ++i) {
            PartitionInfo* info = _pool->add(new PartitionInfo());
            RETURN_IF_ERROR(PartitionInfo::from_thrift(_pool, t_stream_sink.output_partition.partition_infos[i], info));
            _partition_infos.push_back(info);
        }
        // partitions should be in ascending order
        std::sort(_partition_infos.begin(), _partition_infos.end(), compare_part_use_range);
    } else {
    }

    _partitions_columns.resize(_partition_expr_ctxs.size());
    return Status::OK();
}

Status DataStreamSender::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(DataSink::prepare(state));
    _state = state;
    _be_number = state->be_number();

    // Set compression type according to query options
    if (state->query_options().__isset.transmission_compression_type) {
        _compress_type = CompressionUtils::to_compression_pb(state->query_options().transmission_compression_type);
    } else if (config::compress_rowbatches) {
        // If transmission_compression_type is not set, use compress_rowbatches to check if
        // compress transmitted data.
        _compress_type = CompressionTypePB::LZ4;
    }
    RETURN_IF_ERROR(get_block_compression_codec(_compress_type, &_compress_codec));

    std::string instances;
    for (const auto& channel : _channels) {
        if (instances.empty()) {
            instances = channel->get_fragment_instance_id_str();
        } else {
            instances += ", ";
            instances += channel->get_fragment_instance_id_str();
        }
    }
    std::stringstream title;
    title << "DataStreamSender (dst_id=" << _dest_node_id << ", dst_fragments=[" << instances << "])";
    _profile = _pool->add(new RuntimeProfile(title.str()));
    SCOPED_TIMER(_profile->total_time_counter());
    _mem_tracker = std::make_unique<MemTracker>(_profile, -1, "DataStreamSender", state->instance_mem_tracker());
    _profile->add_info_string("PartType", _TPartitionType_VALUES_TO_NAMES.at(_part_type));
    if (_part_type == TPartitionType::UNPARTITIONED || _part_type == TPartitionType::RANDOM) {
        // Randomize the order we open/transmit to channels to avoid thundering herd problems.
        srand(reinterpret_cast<uint64_t>(this));
        std::random_shuffle(_channels.begin(), _channels.end());
    } else if (_part_type == TPartitionType::HASH_PARTITIONED ||
               _part_type == TPartitionType::BUCKET_SHFFULE_HASH_PARTITIONED) {
        RETURN_IF_ERROR(Expr::prepare(_partition_expr_ctxs, state, _row_desc, _expr_mem_tracker.get()));
    } else {
        RETURN_IF_ERROR(Expr::prepare(_partition_expr_ctxs, state, _row_desc, _expr_mem_tracker.get()));
        for (auto iter : _partition_infos) {
            RETURN_IF_ERROR(iter->prepare(state, _row_desc, _expr_mem_tracker.get()));
        }
    }

    _bytes_sent_counter = ADD_COUNTER(profile(), "BytesSent", TUnit::BYTES);
    _uncompressed_bytes_counter = ADD_COUNTER(profile(), "UncompressedBytes", TUnit::BYTES);
    _ignore_rows = ADD_COUNTER(profile(), "IgnoreRows", TUnit::UNIT);
    _serialize_batch_timer = ADD_TIMER(profile(), "SerializeBatchTime");
    _compress_timer = ADD_TIMER(profile(), "CompressTime");
    _send_request_timer = ADD_TIMER(profile(), "SendRequestTime");
    _wait_response_timer = ADD_TIMER(profile(), "WaitResponseTime");
    _shuffle_dispatch_timer = ADD_TIMER(profile(), "ShuffleDispatchTime");
    _shuffle_hash_timer = ADD_TIMER(profile(), "ShuffleHashTime");
    _overall_throughput = profile()->add_derived_counter(
            "OverallThroughput", TUnit::BYTES_PER_SECOND,
            std::bind<int64_t>(&RuntimeProfile::units_per_second, _bytes_sent_counter, profile()->total_time_counter()),
            "");
    for (int i = 0; i < _channels.size(); ++i) {
        RETURN_IF_ERROR(_channels[i]->init(state));
    }

    // set eos for all channels.
    // It will be set to true when closing.
    _chunk_request.set_eos(false);

    _row_indexes.resize(config::vector_chunk_size);

    return Status::OK();
}

DataStreamSender::~DataStreamSender() {
    // TODO: check that sender was either already closed() or there was an error
    // on some channel
    _channel_shared_ptrs.clear();
}

Status DataStreamSender::open(RuntimeState* state) {
    DCHECK(state != NULL);
    RETURN_IF_ERROR(Expr::open(_partition_expr_ctxs, state));
    for (auto iter : _partition_infos) {
        RETURN_IF_ERROR(iter->open(state));
    }
    return Status::OK();
}

Status DataStreamSender::send(RuntimeState* state, RowBatch* batch) {
    SCOPED_TIMER(_profile->total_time_counter());

    // Unpartition or _channel size
    if (_part_type == TPartitionType::UNPARTITIONED || _channels.size() == 1) {
        RETURN_IF_ERROR(serialize_batch(batch, _current_pb_batch, _channels.size()));
        for (auto channel : _channels) {
            RETURN_IF_ERROR(channel->send_batch(_current_pb_batch));
        }
        _current_pb_batch = (_current_pb_batch == &_pb_batch1 ? &_pb_batch2 : &_pb_batch1);
    } else if (_part_type == TPartitionType::RANDOM) {
        // Round-robin batches among channels. Wait for the current channel to finish its
        // rpc before overwriting its batch.
        Channel* current_channel = _channels[_current_channel_idx];
        RETURN_IF_ERROR(serialize_batch(batch, current_channel->pb_batch()));
        RETURN_IF_ERROR(current_channel->send_batch(current_channel->pb_batch()));
        _current_channel_idx = (_current_channel_idx + 1) % _channels.size();
    } else if (_part_type == TPartitionType::HASH_PARTITIONED) {
        // hash-partition batch's rows across channels
        int num_channels = _channels.size();

        for (int i = 0; i < batch->num_rows(); ++i) {
            TupleRow* row = batch->get_row(i);
            size_t hash_val = 0;

            for (auto ctx : _partition_expr_ctxs) {
                void* partition_val = ctx->get_value(row);
                // We can't use the crc hash function here because it does not result
                // in uncorrelated hashes with different seeds.  Instead we must use
                // fvn hash.
                // TODO: fix crc hash/GetHashValue()
                hash_val = RawValue::get_hash_value_fvn(partition_val, ctx->root()->type(), hash_val);
            }
            auto target_channel_id = hash_val % num_channels;
            RETURN_IF_ERROR(_channels[target_channel_id]->add_row(row));
        }
    } else {
        // Range partition
        int num_channels = _channels.size();
        int ignore_rows = 0;
        for (int i = 0; i < batch->num_rows(); ++i) {
            TupleRow* row = batch->get_row(i);
            size_t hash_val = 0;
            bool ignore = false;
            RETURN_IF_ERROR(compute_range_part_code(state, row, &hash_val, &ignore));
            if (ignore) {
                // skip this row
                ignore_rows++;
                continue;
            }
            RETURN_IF_ERROR(_channels[hash_val % num_channels]->add_row(row));
        }
        COUNTER_UPDATE(_ignore_rows, ignore_rows);
    }

    return Status::OK();
}

Status DataStreamSender::send_chunk(RuntimeState* state, vectorized::Chunk* chunk) {
    SCOPED_TIMER(_profile->total_time_counter());
    uint16_t num_rows = chunk->num_rows();
    if (num_rows == 0) {
        return Status::OK();
    }
    // Unpartition or _channel size
    if (_part_type == TPartitionType::UNPARTITIONED || _channels.size() == 1) {
        // We use sender request to avoid serialize chunk many times.
        // 1. create a new chunk PB to serialize
        ChunkPB* pchunk = _chunk_request.add_chunks();
        // 2. serialize input chunk to pchunk
        RETURN_IF_ERROR(serialize_chunk(chunk, pchunk, &_is_first_chunk, _channels.size()));
        _current_request_bytes += pchunk->data().size();
        // 3. if request bytes exceede the threshold, send current request
        if (_current_request_bytes > _request_bytes_threshold) {
            butil::IOBuf attachment;
            construct_brpc_attachment(&_chunk_request, &attachment);
            for (auto channel : _channels) {
                RETURN_IF_ERROR(channel->send_chunk_request(&_chunk_request, attachment));
            }
            _current_request_bytes = 0;
            _chunk_request.clear_chunks();
        }
    } else if (_part_type == TPartitionType::RANDOM) {
        // Round-robin batches among channels. Wait for the current channel to finish its
        // rpc before overwriting its batch.
        // 1. Get request of that channel
        Channel* channel = _channels[_current_channel_idx];
        bool real_sent = false;
        RETURN_IF_ERROR(channel->send_one_chunk(chunk, false, &real_sent));
        if (real_sent) {
            _current_channel_idx = (_current_channel_idx + 1) % _channels.size();
        }
    } else if (_part_type == TPartitionType::HASH_PARTITIONED ||
               _part_type == TPartitionType::BUCKET_SHFFULE_HASH_PARTITIONED) {
        SCOPED_TIMER(_shuffle_dispatch_timer);
        // hash-partition batch's rows across channels
        int num_channels = _channels.size();

        {
            SCOPED_TIMER(_shuffle_hash_timer);
            for (size_t i = 0; i < _partitions_columns.size(); ++i) {
                _partitions_columns[i] = _partition_expr_ctxs[i]->evaluate(chunk);
                DCHECK(_partitions_columns[i] != nullptr);
            }

            if (_part_type == TPartitionType::HASH_PARTITIONED) {
                _hash_values.assign(num_rows, HashUtil::FNV_SEED);
                for (const ColumnPtr& column : _partitions_columns) {
                    column->fvn_hash(&_hash_values[0], 0, num_rows);
                }
            } else {
                // The data distribution was calculated using CRC32_HASH,
                // and bucket shuffle need to use the same hash function when sending data
                _hash_values.assign(num_rows, 0);
                for (const ColumnPtr& column : _partitions_columns) {
                    column->crc32_hash(&_hash_values[0], 0, num_rows);
                }
            }

            // compute row indexes for each channel
            _channel_row_idx_start_points.assign(num_channels + 1, 0);
            for (uint16_t i = 0; i < num_rows; ++i) {
                uint16_t channel_index = _hash_values[i] % num_channels;
                _channel_row_idx_start_points[channel_index]++;
                _hash_values[i] = channel_index;
            }
            // NOTE:
            // we make the last item equal with number of rows of this chunk
            for (int i = 1; i <= num_channels; ++i) {
                _channel_row_idx_start_points[i] += _channel_row_idx_start_points[i - 1];
            }

            for (int i = num_rows - 1; i >= 0; --i) {
                _row_indexes[_channel_row_idx_start_points[_hash_values[i]] - 1] = i;
                _channel_row_idx_start_points[_hash_values[i]]--;
            }
        }

        for (int i = 0; i < num_channels; ++i) {
            size_t from = _channel_row_idx_start_points[i];
            size_t size = _channel_row_idx_start_points[i + 1] - from;
            if (size == 0) {
                // no data for this channel continue;
                continue;
            }
            if (_channels[i]->get_fragment_instance_id().lo == -1) {
                // dest bucket is no used, continue
                continue;
            }
            RETURN_IF_ERROR(_channels[i]->add_rows_selective(chunk, _row_indexes.data(), from, size));
        }
    } else {
        DCHECK(false) << "shouldn't go to here";
    }

    return Status::OK();
}

int DataStreamSender::binary_find_partition(const PartRangeKey& key) const {
    int low = 0;
    int high = _partition_infos.size() - 1;

    VLOG_ROW << "range key: " << key.debug_string() << std::endl;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = _partition_infos[mid]->range().compare_key(key);
        if (cmp == 0) {
            return mid;
        } else if (cmp < 0) { // current < partition[mid]
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return -1;
}

Status DataStreamSender::find_partition(RuntimeState* state, TupleRow* row, PartitionInfo** info, bool* ignore) {
    (void)state;
    if (_partition_expr_ctxs.empty()) {
        *info = _partition_infos[0];
        return Status::OK();
    } else {
        *ignore = false;
        // use binary search to get the right partition.
        ExprContext* ctx = _partition_expr_ctxs[0];
        void* partition_val = ctx->get_value(row);
        // construct a PartRangeKey
        PartRangeKey tmpPartKey;
        if (NULL != partition_val) {
            RETURN_IF_ERROR(PartRangeKey::from_value(ctx->root()->type().type, partition_val, &tmpPartKey));
        } else {
            tmpPartKey = PartRangeKey::neg_infinite();
        }

        int part_index = binary_find_partition(tmpPartKey);
        if (part_index < 0) {
            if (_ignore_not_found) {
                // TODO(zc): add counter to compute its
                std::stringstream error_log;
                error_log << "there is no corresponding partition for this key: ";
                ctx->print_value(row, &error_log);
                LOG(INFO) << error_log.str();
                *ignore = true;
                return Status::OK();
            } else {
                std::stringstream error_log;
                error_log << "there is no corresponding partition for this key: ";
                ctx->print_value(row, &error_log);
                return Status::InternalError(error_log.str());
            }
        }
        *info = _partition_infos[part_index];
    }
    return Status::OK();
}

Status DataStreamSender::process_distribute(RuntimeState* state, TupleRow* row, const PartitionInfo* part,
                                            size_t* code) {
    (void)state;
    uint32_t hash_val = 0;
    for (auto& ctx : part->distributed_expr_ctxs()) {
        void* partition_val = ctx->get_value(row);
        if (partition_val != NULL) {
            hash_val = RawValue::zlib_crc32(partition_val, ctx->root()->type(), hash_val);
        } else {
            //NULL is treat as 0 when hash
            static const int INT_VALUE = 0;
            static const TypeDescriptor INT_TYPE(TYPE_INT);
            hash_val = RawValue::zlib_crc32(&INT_VALUE, INT_TYPE, hash_val);
        }
    }
    hash_val %= part->distributed_bucket();

    int64_t part_id = part->id();
    *code = RawValue::get_hash_value_fvn(&part_id, TypeDescriptor(TYPE_BIGINT), hash_val);

    return Status::OK();
}

Status DataStreamSender::compute_range_part_code(RuntimeState* state, TupleRow* row, size_t* hash_value, bool* ignore) {
    // process partition
    PartitionInfo* part = nullptr;
    RETURN_IF_ERROR(find_partition(state, row, &part, ignore));
    if (*ignore) {
        return Status::OK();
    }
    // process distribute
    RETURN_IF_ERROR(process_distribute(state, row, part, hash_value));
    return Status::OK();
}

Status DataStreamSender::close(RuntimeState* state, Status exec_status) {
    ScopedTimer<MonotonicStopWatch> close_timer(_profile != nullptr ? _profile->total_time_counter() : nullptr);
    // TODO: only close channels that didn't have any errors
    // make all channels close parallel

    // If broadcast is used, _chunk_request may contain some data which should
    // be sent to receiver.
    if (_current_request_bytes > 0) {
        _chunk_request.set_eos(true);
        butil::IOBuf attachment;
        construct_brpc_attachment(&_chunk_request, &attachment);
        for (int i = 0; i < _channels.size(); ++i) {
            _channels[i]->send_chunk_request(&_chunk_request, attachment);
        }
    } else {
        for (int i = 0; i < _channels.size(); ++i) {
            _channels[i]->close(state);
        }
    }

    // wait all channels to finish
    for (int i = 0; i < _channels.size(); ++i) {
        _channels[i]->close_wait(state);
    }
    for (auto iter : _partition_infos) {
        auto st = iter->close(state);
        if (!st.ok()) {
            LOG(WARNING) << "fail to close sender partition, st=" << st.to_string();
            if (_close_status.ok()) {
                _close_status = st;
            }
        }
    }
    Expr::close(_partition_expr_ctxs, state);

    return _close_status;
}

template <typename T>
Status DataStreamSender::serialize_batch(RowBatch* src, T* dest, int num_receivers) {
    VLOG_ROW << "serializing " << src->num_rows() << " rows";
    {
        // TODO(zc)
        // SCOPED_TIMER(_profile->total_time_counter());
        SCOPED_TIMER(_serialize_batch_timer);
        // TODO(zc)
        // RETURN_IF_ERROR(src->serialize(dest));
        int uncompressed_bytes = src->serialize(dest);
        int bytes = RowBatch::get_batch_size(*dest);
        // TODO(zc)
        // int uncompressed_bytes = bytes - dest->tuple_data.size() + dest->uncompressed_size;
        // The size output_batch would be if we didn't compress tuple_data (will be equal to
        // actual batch size if tuple_data isn't compressed)
        COUNTER_UPDATE(_bytes_sent_counter, bytes * num_receivers);
        COUNTER_UPDATE(_uncompressed_bytes_counter, uncompressed_bytes * num_receivers);
    }

    return Status::OK();
}

Status DataStreamSender::serialize_chunk(const vectorized::Chunk* src, ChunkPB* dst, bool* is_first_chunk,
                                         int num_receivers) {
    VLOG_ROW << "serializing " << src->num_rows() << " rows";

    size_t uncompressed_size = 0;
    {
        SCOPED_TIMER(_serialize_batch_timer);
        dst->set_compress_type(CompressionTypePB::NO_COMPRESSION);
        // We only serialize chunk meta for first chunk
        if (*is_first_chunk) {
            uncompressed_size = src->serialize_with_meta(dst);
            *is_first_chunk = false;
        } else {
            dst->clear_is_nulls();
            dst->clear_is_consts();
            dst->clear_slot_id_map();
            uncompressed_size = src->serialize_size();
            // TODO(kks): resize without initializing the new bytes
            dst->mutable_data()->resize(uncompressed_size);
            src->serialize((uint8_t*)dst->mutable_data()->data());
        }
    }

    if (_compress_codec != nullptr && _compress_codec->exceed_max_input_size(uncompressed_size)) {
        return Status::InternalError("The input size for compression should be less than " +
                                     _compress_codec->max_input_size());
    }

    dst->set_uncompressed_size(uncompressed_size);
    // try compress the ChunkPB data
    if (_compress_codec != nullptr && uncompressed_size > 0) {
        SCOPED_TIMER(_compress_timer);

        // Try compressing data to _compression_scratch, swap if compressed data is smaller
        int max_compressed_size = _compress_codec->max_compressed_len(uncompressed_size);

        if (_compression_scratch.size() < max_compressed_size) {
            _compression_scratch.resize(max_compressed_size);
        }

        Slice compressed_slice{_compression_scratch.data(), _compression_scratch.size()};
        _compress_codec->compress(dst->data(), &compressed_slice);
        double compress_ratio = (static_cast<double>(uncompressed_size)) / compressed_slice.size;
        if (LIKELY(compress_ratio > config::rpc_compress_ratio_threshold)) {
            _compression_scratch.resize(compressed_slice.size);
            dst->mutable_data()->swap(reinterpret_cast<std::string&>(_compression_scratch));
            dst->set_compress_type(_compress_type);
        }

        VLOG_ROW << "uncompressed size: " << uncompressed_size << ", compressed size: " << compressed_slice.size;
    }
    size_t chunk_size = dst->data().size();
    VLOG_ROW << "chunk data size " << chunk_size;

    COUNTER_UPDATE(_bytes_sent_counter, chunk_size * num_receivers);
    COUNTER_UPDATE(_uncompressed_bytes_counter, uncompressed_size * num_receivers);
    return Status::OK();
}

void DataStreamSender::construct_brpc_attachment(PTransmitChunkParams* params, butil::IOBuf* attachment) {
    for (int i = 0; i < params->chunks().size(); ++i) {
        auto chunk = params->mutable_chunks(i);
        chunk->set_data_size(chunk->data().size());
        attachment->append(chunk->data());
        chunk->clear_data();
    }
}

int64_t DataStreamSender::get_num_data_bytes_sent() const {
    // TODO: do we need synchronization here or are reads & writes to 8-byte ints
    // atomic?
    int64_t result = 0;

    for (int i = 0; i < _channels.size(); ++i) {
        result += _channels[i]->num_data_bytes_sent();
    }

    return result;
}

} // namespace starrocks
// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/pipeline/fragment_executor.h"

#include <unordered_map>

#include "exec/exchange_node.h"
#include "exec/pipeline/driver_source.h"
#include "exec/pipeline/exchange/exchange_sink_operator.h"
#include "exec/pipeline/exchange/local_exchange_sink_operator.h"
#include "exec/pipeline/exchange/local_exchange_source_operator.h"
#include "exec/pipeline/exchange/sink_buffer.h"
#include "exec/pipeline/morsel.h"
#include "exec/pipeline/pipeline_builder.h"
#include "exec/pipeline/result_sink_operator.h"
#include "exec/scan_node.h"
#include "gen_cpp/starrocks_internal_service.pb.h"
#include "gutil/casts.h"
#include "gutil/map_util.h"
#include "runtime/data_stream_sender.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/result_sink.h"
#include "util/pretty_printer.h"
#include "util/uid_util.h"

namespace starrocks::pipeline {

Morsels convert_scan_range_to_morsel(const std::vector<TScanRangeParams>& scan_ranges, int node_id) {
    Morsels morsels;
    for (auto scan_range : scan_ranges) {
        morsels.emplace_back(std::make_shared<OlapMorsel>(node_id, scan_range));
    }
    return morsels;
}

Status FragmentExecutor::prepare(ExecEnv* exec_env, const TExecPlanFragmentParams& request) {
    const TPlanFragmentExecParams& params = request.params;
    auto& query_id = params.query_id;
    auto& fragment_id = params.fragment_instance_id;
    _query_ctx = QueryContextManager::instance()->get_or_register(query_id);
    if (params.__isset.instances_number) {
        _query_ctx->set_num_fragments(params.instances_number);
    }
    _fragment_ctx = FragmentContextManager::instance()->get_or_register(fragment_id);
    _fragment_ctx->set_query_id(query_id);
    _fragment_ctx->set_fragment_instance_id(fragment_id);
    _fragment_ctx->set_fe_addr(request.coord);

    LOG(INFO) << "Prepare(): query_id=" << print_id(query_id)
              << " fragment_instance_id=" << print_id(params.fragment_instance_id)
              << " backend_num=" << request.backend_num;

    _fragment_ctx->set_runtime_state(
            std::make_unique<RuntimeState>(request, request.query_options, request.query_globals, exec_env));
    auto* runtime_state = _fragment_ctx->runtime_state();

    int64_t bytes_limit = request.query_options.mem_limit;
    // NOTE: this MemTracker only for olap
    _fragment_ctx->set_mem_tracker(
            std::make_unique<MemTracker>(bytes_limit, "fragment mem-limit", exec_env->query_pool_mem_tracker(), true));
    auto mem_tracker = _fragment_ctx->mem_tracker();

    runtime_state->set_batch_size(config::vector_chunk_size);
    RETURN_IF_ERROR(runtime_state->init_mem_trackers(query_id));
    runtime_state->set_be_number(request.backend_num);
    runtime_state->set_fragment_mem_tracker(mem_tracker);

    LOG(INFO) << "Using query memory limit: " << PrettyPrinter::print(bytes_limit, TUnit::BYTES);

    // Set up desc tbl
    auto* obj_pool = runtime_state->obj_pool();
    DescriptorTbl* desc_tbl = NULL;
    DCHECK(request.__isset.desc_tbl);
    RETURN_IF_ERROR(DescriptorTbl::create(obj_pool, request.desc_tbl, &desc_tbl));
    runtime_state->set_desc_tbl(desc_tbl);
    // Set up plan
    ExecNode* plan = nullptr;
    DCHECK(request.__isset.fragment);
    RETURN_IF_ERROR(ExecNode::create_tree(runtime_state, obj_pool, request.fragment.plan, *desc_tbl, &plan));
    runtime_state->set_fragment_root_id(plan->id());
    _fragment_ctx->set_plan(plan);

    // Set senders of exchange nodes before pipeline build
    std::vector<ExecNode*> exch_nodes;
    plan->collect_nodes(TPlanNodeType::EXCHANGE_NODE, &exch_nodes);
    for (auto* exch_node : exch_nodes) {
        int num_senders = FindWithDefault(params.per_exch_num_senders, exch_node->id(), 0);
        static_cast<ExchangeNode*>(exch_node)->set_num_senders(num_senders);
    }

    int32_t driver_instance_count = 1;
    if (request.query_options.__isset.query_threads) {
        driver_instance_count = request.query_options.query_threads;
    }
    PipelineBuilderContext context(*_fragment_ctx, driver_instance_count);
    PipelineBuilder builder(context);
    _fragment_ctx->set_pipelines(builder.build(*_fragment_ctx, plan));
    // Set up sink, if required
    std::unique_ptr<DataSink> sink;
    if (request.fragment.__isset.output_sink) {
        RowDescriptor row_desc;
        RETURN_IF_ERROR(DataSink::create_data_sink(obj_pool, request.fragment.output_sink,
                                                   request.fragment.output_exprs, params, row_desc, &sink));
        RuntimeProfile* sink_profile = sink->profile();
        if (sink_profile != nullptr) {
            runtime_state->runtime_profile()->add_child(sink_profile, true, nullptr);
        }
        _convert_data_sink_to_operator(params, &context, sink.get());
    }

    // set scan ranges
    std::vector<ExecNode*> scan_nodes;
    std::vector<TScanRangeParams> no_scan_ranges;
    plan->collect_scan_nodes(&scan_nodes);

    std::unordered_map<int32_t, DriverSourcePtr> sources;
    for (int i = 0; i < scan_nodes.size(); ++i) {
        ScanNode* scan_node = down_cast<ScanNode*>(scan_nodes[i]);
        const std::vector<TScanRangeParams>& scan_ranges =
                FindWithDefault(params.per_node_scan_ranges, scan_node->id(), no_scan_ranges);
        Morsels morsels = convert_scan_range_to_morsel(scan_ranges, scan_node->id());
        sources.emplace(scan_node->id(), std::make_unique<DriverSource>(morsels, scan_node->id()));
    }

    Drivers drivers;
    std::map<int32_t, Pipeline*> source_to_pipelines;
    const auto& pipelines = _fragment_ctx->pipelines();
    const size_t num_pipelines = pipelines.size();
    for (auto n = 0; n < num_pipelines; ++n) {
        const auto& pipeline = pipelines[n];
        // the last pipeline in _fragment_ctx->pipelines is the root pipeline, the root pipeline
        // means that it comes from the root ExecNode of the fragment instance. because pipelines
        // are constructed from fragment instance that is organized as an ExecNode tree via
        // post-order traversal.
        const bool is_root = (n == num_pipelines - 1);

        if (pipeline->get_op_factories()[0]->is_source()) {
            auto source_id = pipeline->get_op_factories()[0]->plan_node_id();
            auto& source = sources[source_id];
            const auto morsel_size = source->get_morsels().size();
            // for a leaf pipeline(contains ScanOperator), the parallelism degree is morse_size,
            if (is_root) {
                _fragment_ctx->set_num_root_drivers(morsel_size);
            }
            for (auto i = 0; i < morsel_size; ++i) {
                Operators operators;
                for (const auto& factory : pipeline->get_op_factories()) {
                    operators.emplace_back(factory->create(morsel_size, i));
                }
                DriverPtr driver = std::make_shared<PipelineDriver>(operators, _query_ctx, _fragment_ctx, 0, is_root);
                driver->set_morsel(source->get_morsels()[i]);
                drivers.emplace_back(std::move(driver));
            }
        } else {
            // for a non-leaf pipeline(contains no ScanOperator), the parallelism degree is
            // driver_instance_count.
            const auto driver_instance_count = pipeline->get_driver_instance_count();
            if (is_root) {
                _fragment_ctx->set_num_root_drivers(driver_instance_count);
            }
            for (auto i = 0; i < driver_instance_count; ++i) {
                Operators operators;
                for (const auto& factory : pipeline->get_op_factories()) {
                    operators.emplace_back(factory->create(driver_instance_count, i));
                }
                DriverPtr driver = std::make_shared<PipelineDriver>(operators, _query_ctx, _fragment_ctx, i, is_root);
                drivers.emplace_back(driver);
            }
        }
    }
    _fragment_ctx->set_drivers(std::move(drivers));
    return Status::OK();
}
Status FragmentExecutor::execute(ExecEnv* exec_env) {
    for (auto driver : _fragment_ctx->drivers()) {
        RETURN_IF_ERROR(driver->prepare(_fragment_ctx->runtime_state()));
    }
    for (auto driver : _fragment_ctx->drivers()) {
        exec_env->driver_dispatcher()->dispatch(driver);
    }
    return Status::OK();
}

void FragmentExecutor::_convert_data_sink_to_operator(const TPlanFragmentExecParams& params,
                                                      PipelineBuilderContext* context, DataSink* datasink) {
    if (typeid(*datasink) == typeid(starrocks::ResultSink)) {
        starrocks::ResultSink* result_sink = down_cast<starrocks::ResultSink*>(datasink);
        // Result sink doesn't have plan node id;
        OpFactoryPtr op = std::make_shared<ResultSinkOperatorFactory>(
                context->next_operator_id(), -1, result_sink->get_sink_type(), result_sink->get_output_exprs());
        // Add result sink operator to last pipeline
        _fragment_ctx->pipelines().back()->add_op_factory(op);
    } else if (typeid(*datasink) == typeid(starrocks::DataStreamSender)) {
        starrocks::DataStreamSender* sender = down_cast<starrocks::DataStreamSender*>(datasink);
        std::shared_ptr<SinkBuffer> sink_buffer = std::make_shared<SinkBuffer>(sender->get_destinations_size());

        OpFactoryPtr exchange_sink = std::make_shared<ExchangeSinkOperatorFactory>(
                context->next_operator_id(), -1, sink_buffer, sender->get_partition_type(), params.destinations,
                params.sender_id, sender->get_dest_node_id(), sender->get_partition_exprs());
        _fragment_ctx->pipelines().back()->add_op_factory(exchange_sink);
    }
}

} // namespace starrocks::pipeline
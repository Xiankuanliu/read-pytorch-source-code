#include "function.h"

#include <string>

#include "torch/csrc/autograd/functions/special.h"
#include "torch/csrc/jit/ir.h"
#include "variable.h"

namespace torch {
namespace autograd {

// 如何根据 inputs 来计算 Function 的 flags
template <typename T>
auto makeFlags(const T& inputs) -> FunctionFlags {
    int num_inputs = inputs.size();
    FunctionFlags f;
    f.is_executable = false;
    f.is_volatile = false;
    f.next_functions.resize(num_inputs);
    {
        int i = 0;
        for (auto it = inputs.begin(); it != inputs.end(); ++it, ++i) {
            auto& var = *it;
            if (var.defined()) {
                f.is_executable |=
                    var.requires_grad();  // 只要有一个 var.requires_grad() = true, f.is_executable 就为 true
                f.is_volatile |= var.is_volatile();  // 只要有一个 var.is_voaltile()=true, f.is_volatile 就为 true

                //首先，这里可以看出， 有几个 inputs variables，就有几个 next_function , next_functions 中就记录着
                //反向传导时，下一步要操作的 function
                // 如果var不是叶子节点, 那么它是应该会有 grad_fn 的
                // 对于leaf-variable来说, 是没有 grad_fn 的, 这时候是用的 grad_accumulator 来操作的
                if (var.grad_fn()) {  // 如果 var 不是 叶子节点，
                    f.next_functions[i] = std::make_pair<>(var.grad_fn(), var.output_nr());
                } else {
                    f.next_functions[i] = std::make_pair<>(var.grad_accumulator(), 0);
                }
            }
        }
    }
    f.is_executable &=
        !f.is_volatile;  // 只有 f.is_executable=true 和 f.is_volatile=false 同时成立，f.is_executable=true
    // 只有 f.is_executable = true， 才可对 此函数求导！！！！
    return f;
}

auto Function::flags(const variable_list& inputs) -> FunctionFlags { return makeFlags(inputs); }

auto Function::flags(const std::initializer_list<Variable>& inputs) -> FunctionFlags { return makeFlags(inputs); }

auto Function::flags(at::TensorList inputs) -> FunctionFlags {
    // TODO: Eliminate the intermediate vector allocation
    return makeFlags(variable_list(inputs.begin(), inputs.end()));
}

auto Function::name() -> std::string { return std::string(typeid(*this).name()); }

// This function is analogous to make_trace which operates on PythonOp, but this
// function instead works for C++ implemented autograd Functions, which don't
// actually have any backing Python class. We still need to trace them!
variable_list Function::tracedApply(variable_list inputs) {
    using namespace torch::jit;
    // Traceable Functions are completely transparent to the JIT.
    if (is_traceable()) {
        return apply(inputs);
    }
    auto state = tracer::getTracingState(inputs);
    auto state_lock = state->lock();

    // Insert a CppOp in the trace.
    auto& graph = state->graph;
    auto* this_node = graph->createCppOp(getSharedPtr());
    for (auto& input : inputs) {
        this_node->addInput(tracer::getValueTrace(state, input));
    }
    graph->appendNode(this_node);

    // Finally apply this Function.
    state_lock.unlock();
    variable_list outputs = apply(inputs);
    state_lock.lock();

    // Set up output traces.
    int num_outputs = outputs.size();
    for (int i = 0; i < num_outputs; ++i) {
        auto& output = outputs[i];
        Node* sel = graph->appendNode(graph->createSelect(this_node, i));
        // TODO: At the moment, C++ does not track shared storage.  It
        // should.  Update this when that happens.
        if (output.defined()) {
            sel->inferTypeFrom(output.data());
            tracer::setValueTrace(state, output, sel);
        }
    }

    if (!passes_state_transparently()) {
        auto this_eval = dynamic_cast<Eval*>(this);
        // Evals consume handle from a context edge of forward node
        if (this_eval) this_node->addInput(this_eval->forward_ctx_select);
        // There's no point in wrapping functions in Eval, if we know they already are
        // part of another Eval subgraph. This is both a small optimization, and
        // it allows us to not implement saved_variables() in many functions.
        bool should_trace_backward = tracing_state->in_eval_subgraph;
        if (!should_trace_backward) {
            auto saved_vars = saved_variables();
            if (!saved_vars)
                throw std::runtime_error(std::string("saved_variables() needed but not implemented in ") + name());
            variable_list bw_subgraph_inputs(inputs);
            for (auto& saved_var : *saved_vars) {
                bw_subgraph_inputs.emplace_back(saved_var.unpack(getSharedPtr()));
            }
            tracer::nontraceableBackwardSubgraph(bw_subgraph_inputs, outputs);
        }
        bool has_backwards_eval = !should_trace_backward || this_eval;
        if (has_backwards_eval) setUpContextEdge(this_node, num_outputs, inputs, outputs);
    }
    return outputs;
}

void Function::setUpContextEdge(jit::Node* node, int ctx_output_nr, const variable_list& inputs,
                                const variable_list& outputs) {
    jit::Graph* graph = node->owningGraph();
    jit::Node* ctx_select = graph->appendNode(graph->createSelect(node, ctx_output_nr));
    ctx_select->setType(std::make_shared<jit::HandleType>());
    auto backward_eval = Eval::getBackwardEval(inputs, outputs);
    if (backward_eval) backward_eval->forward_ctx_select = ctx_select;
}

}  // namespace autograd
}  // namespace torch

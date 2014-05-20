#include "Caching.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Param.h"
#include "Var.h"

#include <map>

namespace Halide {
namespace Internal {

namespace {

class FindParameterDependencies : public IRGraphVisitor {
public:
    FindParameterDependencies() { }
    ~FindParameterDependencies() { }

    void visit_function(const Function &function) {
        if (function.has_pure_definition()) {
            const std::vector<Expr> &values = function.values();
            for (size_t i = 0; i < values.size(); i++) {
                values[i].accept(this);
            }
        }

        const std::vector<ReductionDefinition> &reductions =
            function.reductions();
        for (size_t i = 0; i < reductions.size(); i++) {
            const std::vector<Expr> &values = reductions[i].values;
            for (size_t j = 0; j < values.size(); j++) {
                values[j].accept(this);
            }

            const std::vector<Expr> &args = reductions[i].args;
            for (size_t j = 0; j < args.size(); j++) {
                args[j].accept(this);
            }
        
            if (reductions[i].domain.defined()) {
                const std::vector<ReductionVariable> &rvars =
                    reductions[i].domain.domain();
                for (size_t j = 0; j < rvars.size(); j++) {
                    rvars[j].min.accept(this);
                    rvars[j].extent.accept(this);
                }
            }
        }

        if (function.has_extern_definition()) {
            const std::vector<ExternFuncArgument> &extern_args =
                function.extern_arguments();
            for (size_t i = 0; i < extern_args.size(); i++) {
                if (extern_args[i].is_func()) {
                    visit_function(extern_args[i].func);
                } else if (extern_args[i].is_expr()) {
                    extern_args[i].expr.accept(this);
                } else if (extern_args[i].is_buffer()) {
                    // Function with an extern definition
                    record(Halide::Internal::Parameter(extern_args[i].buffer.type(), true,
                                                       extern_args[i].buffer.name()));
                } else if (extern_args[i].is_image_param()) {
                    record(extern_args[i].image_param);
                } else {
                    assert(!extern_args[i].defined() && "Unexpected ExternFunctionArgument type.");
                }
            }
        }
        const std::vector<Parameter> &output_buffers =
            function.output_buffers();
        for (size_t i = 0; i < output_buffers.size(); i++) {
            for (int j = 0; j < std::min(function.dimensions(), 4); j++) {
                if (output_buffers[i].min_constraint(i).defined()) {
                    output_buffers[i].min_constraint(i).accept(this);
                }
                if (output_buffers[i].stride_constraint(i).defined()) {
                    output_buffers[i].stride_constraint(i).accept(this);
                }
                if (output_buffers[i].extent_constraint(i).defined()) {
                    output_buffers[i].extent_constraint(i).accept(this);
                }
            }
        }
    }

    using IRGraphVisitor::visit;

    void visit(const Call *call) {
        if (call->param.defined()) {
            record(call->param);
        }

        if (call->call_type == Call::Intrinsic && call->name == Call::cache_expr) {
            internal_assert(call->args.size() == 1);
            record(call->args[0]);
        } else {
            // Do not look at anything inside a cache_expr bracket.
            visit_function(call->func);
            IRGraphVisitor::visit(call);
        }
    }


    void visit(const Load *load) {
        if (load->param.defined()) {
            record(load->param);
        }
        IRGraphVisitor::visit(load);
    }

    void visit(const Variable *var) {
        if (var->param.defined()) {
            record(var->param);
        }
        IRGraphVisitor::visit(var);
    }

    void record(const Parameter &parameter) {
        struct DependencyInfo info;

        info.type = parameter.type();

        if (parameter.is_buffer()) {
            InternalError("Cannot yet cache computations which depend on buffer parameters");
        } else if (info.type.is_handle()) {
            InternalError("Cannot yet cache computations which depend on handle parameters");
        } else {
            info.size_expr = info.type.bytes();
            info.value_expr = Internal::Variable::make(info.type, parameter.name(), parameter);
        }

        dependency_info[DependencyKey(info.type.bytes(), parameter.name())] = info;
    }

    void record(const Expr &expr) {
        struct DependencyInfo info;
        info.type = expr.type();
        info.size_expr = info.type.bytes();
        info.value_expr = expr;
    }

#if 0
    void record(const Argument &) {
    }
#endif

    // Used to make sure larger parameters come before smaller ones
    // for alignment reasons.
    struct DependencyKey {
        uint32_t size;
        std::string name;

        bool operator<(const DependencyKey &rhs) const {
            if (size < rhs.size) {
                return true;
            } else if (size == rhs.size) {
                return name < rhs.name;
            }
            return false;
        }

        DependencyKey(uint32_t size_arg, const std::string &name_arg)
            : size(size_arg), name(name_arg) {
        }
    };

    struct DependencyInfo {
        Type type;
        Expr size_expr;
        Expr value_expr;
    };

    std::map<DependencyKey, DependencyInfo> dependency_info;
};

class KeyInfo {
    FindParameterDependencies dependencies;
    Expr key_size_expr;
    const std::string &top_level_name;
    const std::string &function_name;

    size_t parameters_alignment() {
        int32_t max_alignment = 0;
        std::map<FindParameterDependencies::DependencyKey,
                 FindParameterDependencies::DependencyInfo>::const_iterator iter;
        // Find maximum natural alignment needed.
        for (iter = dependencies.dependency_info.begin();
             iter != dependencies.dependency_info.end();
             iter++) {
            max_alignment = std::max(max_alignment, iter->second.type.bytes());
        }
        // Make sure max_alignment is a power of two and has maximum value of 32
        int i = 0;
        while (i < 4 && max_alignment > (1 << i)) {
            i = i + 1;
        }
        return (1 << i);
    }

    Stmt call_copy_memory(const std::string &key_name, const std::string &value, Expr index) {
        Expr dest = Call::make(Handle(), Call::address_of,
                                    vec(Load::make(UInt(8), key_name, index, Buffer(), Parameter())),
                                    Call::Intrinsic);
        Expr src = StringImm::make(value);
        Expr copy_size = (int32_t)value.size();
        
        return Evaluate::make(Call::make(UInt(8), Call::copy_memory,
                                         vec(dest, src, copy_size), Call::Intrinsic));
    }

public:
    KeyInfo(const Function &function, const std::string &name)
        : top_level_name(name), function_name(function.name())
    {
        dependencies.visit_function(function);
        std::map<FindParameterDependencies::DependencyKey,
                 FindParameterDependencies::DependencyInfo>::const_iterator iter;
        key_size_expr = 4 + (int32_t)((top_level_name.size() + 3) & ~3);
        
        size_t func_name_size = 4 + function_name.size();

        size_t needed_alignment = parameters_alignment();
        if (needed_alignment > 1) {
            func_name_size = (func_name_size + needed_alignment) & ~(needed_alignment - 1);
        }
        key_size_expr += (int32_t)func_name_size;
        
        for (iter = dependencies.dependency_info.begin();
             iter != dependencies.dependency_info.end();
             iter++) {
            key_size_expr += iter->second.size_expr;
        }
    }

    // Return the number of bytes needed to store the cache key
    // for the target function.
    Expr key_size() { return key_size_expr; };

    // Code to fill in the Allocation named key_name with the byte of
    // the key. The Allocation is guaranteed to be 1d, of type uint8_t
    // and of the size returned from key_size
    Stmt generate_key(std::string key_name) {
        std::vector<Stmt> writes;
        Expr index = Expr(0);

        // In code below, casts to vec type is done because stores to
        // the buffer can be unaligned.

        Expr top_level_name_size = (int32_t)top_level_name.size();
        writes.push_back(Store::make(key_name, Cast::make(Int(32), top_level_name_size), index));
        index += 4;
        writes.push_back(call_copy_memory(key_name, top_level_name, index));
        // Align to four byte boundary again.
        index += top_level_name_size;
        size_t alignment = top_level_name.size();
        while (alignment % 4) {
            writes.push_back(Store::make(key_name, Cast::make(UInt(8), 0), index));
            index = index + 1;
            alignment++;
        }

        Expr name_size = (int32_t)function_name.size();
        writes.push_back(Store::make(key_name, Cast::make(Int(32), name_size), index));
        index += 4;
        writes.push_back(call_copy_memory(key_name, function_name, index));
        index += name_size;

        alignment += 4 + function_name.size();
        size_t needed_alignment = parameters_alignment();
        if (needed_alignment > 1) {
            while (alignment % needed_alignment) {
                writes.push_back(Store::make(key_name, Cast::make(UInt(8), 0), index));
                index = index + 1;
                alignment++;
            }
        }
        
        std::map<FindParameterDependencies::DependencyKey,
                 FindParameterDependencies::DependencyInfo>::const_iterator iter;
        for (iter = dependencies.dependency_info.begin();
             iter != dependencies.dependency_info.end();
             iter++) {
            writes.push_back(Store::make(key_name, iter->second.value_expr, index));
            index += iter->second.size_expr;
        }
        Stmt blocks;
        for (size_t i = writes.size(); i > 0; i--) {
            blocks = Block::make(writes[i - 1], blocks);
        }

        return blocks;
    }

    // Returns a bool expression, which either evaluates to true,
    // in which case the Allocation named by storage will be computed,
    // or false, in which case it will be assumed the buffer was populated
    // by the code in this call.
    Expr generate_lookup(std::string key_allocation_name, std::string storage_allocation_name) {
        std::vector<Expr> args;
        args.push_back(Variable::make(type_of<void *>(), "__user_context", Parameter(Handle(), false, "__user_context")));
        args.push_back(Variable::make(type_of<uint8_t *>(), key_allocation_name + ".host"));
        args.push_back(key_size());
        args.push_back(Variable::make(type_of<buffer_t *>(), storage_allocation_name));

        return Call::make(Bool(1), "halide_cache_lookup", args, Call::Extern);
    }

    // Returns a statement which will store the result of a computation under this key
    Stmt store_computation(std::string key_allocation_name, std::string storage_allocation_name) {
        std::vector<Expr> args;
        args.push_back(Variable::make(type_of<void *>(), "__user_context", Parameter(Handle(), false, "__user_context")));
        args.push_back(Variable::make(type_of<uint8_t *>(), key_allocation_name + ".host"));
        args.push_back(key_size());
        args.push_back(Variable::make(type_of<buffer_t *>(), storage_allocation_name));

        // This is actually a void call. How to indicate that? Look at Extern_ stuff. */
        return Evaluate::make(Call::make(Bool(1), "halide_cache_store", args, Call::Extern));
    }
};

}

// Inject caching structure around compute_cached realizations.
class InjectCaching : public IRMutator {
public:
  const std::map<std::string, Function> &env;
  const std::string &top_level_name;

  InjectCaching(const std::map<std::string, Function> &e, const std::string &name) :
      env(e), top_level_name(name) {}
private:

    using IRMutator::visit;

    void visit(const Pipeline *op) {
        std::map<std::string, Function>::const_iterator f = env.find(op->name);
        if (f != env.end() &&
            f->second.schedule().cached) {
            Stmt produce = mutate(op->produce);
            Stmt update = mutate(op->update);
            Stmt consume = mutate(op->consume);

            KeyInfo key_info(f->second, top_level_name);

            std::string cache_key_name = op->name + ".cache_key";
            std::string cache_miss_name = op->name + ".cache_miss";
            std::string buffer_name = op->name + ".buffer";

            Expr cache_miss = Variable::make(UInt(1), cache_miss_name);
            Stmt mutated_produce =
                produce.defined() ? IfThenElse::make(cache_miss, produce) :
                                        produce;
            Stmt mutated_update =
                update.defined() ? IfThenElse::make(cache_miss, update) :
                                       update;
            Stmt cache_store_back =
                IfThenElse::make(cache_miss, key_info.store_computation(cache_key_name, buffer_name)); 
            Stmt mutated_consume = 
                consume.defined() ? Block::make(cache_store_back, consume) :
                                        cache_store_back;

            Stmt mutated_pipeline = Pipeline::make(op->name, mutated_produce, mutated_update, mutated_consume);
            Stmt cache_lookup = LetStmt::make(cache_miss_name, key_info.generate_lookup(cache_key_name, buffer_name), mutated_pipeline);
            Stmt generate_key = Block::make(key_info.generate_key(cache_key_name), cache_lookup);
            Stmt cache_key_alloc = Allocate::make(cache_key_name, UInt(8), vec(key_info.key_size()), generate_key);

            stmt = cache_key_alloc;
        } else {
            IRMutator::visit(op);
        }
    }
};

Stmt inject_caching(Stmt s, const std::map<std::string, Function> &env,
                    const std::string &name) {
    InjectCaching injector(env, name);

    return injector.mutate(s);
}

}
}
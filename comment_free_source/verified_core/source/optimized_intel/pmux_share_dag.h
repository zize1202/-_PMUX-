// SPDX-License-Identifier: MIT
// Bounded PMUX-DAG flattening with exact, mutually exclusive leaf guards.
// Arithmetic and state cells remain intact; there are no nested pass calls.
#ifndef PMUX_SHARE_DAG_20261101_H
#define PMUX_SHARE_DAG_20261101_H
#include "pmux_resource_share.h"
#include <tuple>

namespace pmux_dag_detail {
using namespace Yosys;
using pmux_resource_detail::Sig;
using pmux_resource_detail::Bit;
using pmux_resource_detail::Cell;
using pmux_resource_detail::Module;
using pmux_resource_detail::Context;
using pmux_resource_detail::SelectionProof;
using pmux_resource_detail::known_literals;
using pmux_resource_detail::protected_object;

struct Edge {
    Sig value;
    Bit condition;
    bool is_default = false;
    Cell *child = nullptr;
};
struct Node {
    Cell *cell = nullptr;
    Sig output, select;
    std::vector<Edge> edges;
    bool full = false;
    int height = 1, expanded = 0;
};

                                                                               
                                                                                 
struct BoolDag {
    enum Kind { ZERO, ONE, INPUT, NOT, AND, OR };
    struct Expr { Kind kind; int a = -1, b = -1; Bit bit; };
    std::vector<Expr> expressions;
    std::map<Bit, int> inputs;
    std::map<std::tuple<int, int, int>, int> cache;
    BoolDag() { expressions.push_back({ZERO}); expressions.push_back({ONE}); }

    int input(Bit bit) {
        if (bit == Bit(State::S0)) return 0;
        if (bit == Bit(State::S1)) return 1;
        auto found = inputs.find(bit);
        if (found != inputs.end()) return found->second;
        int id = int(expressions.size());
        Expr expression{INPUT}; expression.bit = bit;
        expressions.push_back(expression); inputs.emplace(bit, id); return id;
    }
    int make(Kind kind, int a, int b = -1) {
        if (kind == NOT) {
            if (a < 2) return 1-a;
            if (expressions[a].kind == NOT) return expressions[a].a;
        } else {
            if (a > b) std::swap(a, b);
            if (a == b) return a;
            if (kind == AND) { if (a == 0) return 0; if (a == 1) return b; }
            if (kind == OR) { if (a == 0) return b; if (a == 1) return 1; }
            if ((expressions[a].kind == NOT && expressions[a].a == b) ||
                (expressions[b].kind == NOT && expressions[b].a == a)) return kind == AND ? 0 : 1;
        }
        auto key = std::make_tuple(int(kind), a, b);
        auto found = cache.find(key);
        if (found != cache.end()) return found->second;
        int id = int(expressions.size()); expressions.push_back({kind, a, b}); cache.emplace(key, id); return id;
    }
    int merge(std::vector<int> terms) {
        if (terms.empty()) return 0;
        std::sort(terms.begin(), terms.end()); terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        while (terms.size() > 1) {
            std::vector<int> next;
            for (size_t i = 0; i < terms.size(); i += 2)
                next.push_back(i+1 == terms.size() ? terms[i] : make(OR, terms[i], terms[i+1]));
            terms.swap(next);
        }
        return terms.front();
    }
    void visit(int id, std::set<int> &live) const {
        if (expressions[id].kind <= INPUT || !live.insert(id).second) return;
        visit(expressions[id].a, live);
        if (expressions[id].b >= 0) visit(expressions[id].b, live);
    }
    int gates(const std::vector<int> &roots) const {
        std::set<int> live;
        for (int id : roots) visit(id, live);
        return int(live.size());
    }
};

struct Plan {
    Cell *root = nullptr;
    Sig output;
    int width = 0, input_arms = 0, expanded_leaves = 0;
    int old_estimate = 0, new_estimate = 0, control_gates = 0, saving = 0;
    std::map<Cell *, Node> nodes;
    std::vector<Cell *> postorder;
    std::vector<Sig> leaves;
    std::vector<int> guards;
    BoolDag controls;
};

struct Collector {
    Context &context;
    const pool<Cell *> &selected;
    Plan &plan;
    pool<Cell *> active;
    Collector(Context &ctx, const pool<Cell *> &sel, Plan &p) : context(ctx), selected(sel), plan(p) {}

    bool collect(Cell *cell) {
        if (active.count(cell)) return false;
        if (plan.nodes.count(cell)) return true;
        if (plan.nodes.size() >= 8 || active.size() >= 4 || protected_object(cell)) return false;
        const int w = cell->getParam(ID::WIDTH).as_int(), n = cell->getParam(ID::S_WIDTH).as_int();
        if (w != plan.width || n < 1 || n > 16) return false;
        Sig a = context.sigmap(cell->getPort(ID::A)), b = context.sigmap(cell->getPort(ID::B));
        Sig s = context.sigmap(cell->getPort(ID::S)), y = context.sigmap(cell->getPort(ID::Y));
        if (GetSize(a) != w || GetSize(b) != w*n || GetSize(s) != n || GetSize(y) != w ||
            !known_literals(s) || context.protected_signal(y) || context.whole_driver(y) != cell) return false;
        SelectionProof proof;
        if (!pmux_resource_detail::prove_selection(context, cell, s, proof)) return false;
        Node node; node.cell = cell; node.output = y; node.select = s; node.full = proof.full;
        if (!proof.full) node.edges.push_back({a, Bit(State::S0), true, nullptr});
        for (int i = 0; i < n; i++) if (proof.reachable[i])
            node.edges.push_back({b.extract(i*w, w), s[i], false, nullptr});
        if (node.edges.empty()) return false;
        plan.nodes.emplace(cell, node);
        active.insert(cell);
        for (auto &edge : plan.nodes.at(cell).edges) {
            if (!known_literals(edge.value) || context.protected_signal(edge.value)) return false;
            auto child = context.whole_driver(edge.value);
            if (child && child->type == ID($pmux) && selected.count(child) &&
                child->getParam(ID::WIDTH).as_int() == w) {
                if (!collect(child)) return false;
                edge.child = child;
                plan.nodes.at(cell).height = std::max(plan.nodes.at(cell).height, plan.nodes.at(child).height+1);
                plan.nodes.at(cell).expanded += plan.nodes.at(child).expanded;
            } else plan.nodes.at(cell).expanded++;
            if (plan.nodes.at(cell).height > 4 || plan.nodes.at(cell).expanded > 32) return false;
        }
        active.erase(cell); plan.postorder.push_back(cell); return true;
    }

    bool closed() const {
        pool<Bit> all_outputs;
        for (auto &entry : plan.nodes) for (auto bit : entry.second.output) all_outputs.insert(bit);
        for (auto &entry : plan.nodes) {
            const auto &node = entry.second;
                                                                             
                                                                              
            for (auto bit : node.select) if (all_outputs.count(bit)) return false;
            for (auto &edge : node.edges) if (!edge.child)
                for (auto bit : edge.value) if (all_outputs.count(bit)) return false;
            if (entry.first == plan.root) continue;
            for (auto bit : node.output) {
                if (context.output_bits.count(bit) || context.protected_bits.count(bit) || context.multiple.count(bit)) return false;
                if (context.consumers.count(bit)) for (auto user : context.consumers.at(bit))
                    if (!plan.nodes.count(user)) return false;
            }
        }
        return true;
    }
};

inline int bank_cost(int leaves) {
    if (leaves <= 1) return 0;
    int pair_terms = (leaves+1)/2;
    return pair_terms + (pair_terms+1)/3;
}
inline int nor_cost(int terms) { return terms <= 1 ? (terms == 1 ? 1 : 0) : (terms+1)/3; }

inline bool make_plan(Context &context, const pool<Cell *> &selected, Cell *root, Plan &plan)
{
    plan.root = root; plan.width = root->getParam(ID::WIDTH).as_int();
    if (plan.width < 4 || plan.width > 128 || root->get_bool_attribute(ID(pmux_dag_flattened))) return false;
    plan.output = context.sigmap(root->getPort(ID::Y));
    Collector collector(context, selected, plan);
    if (!collector.collect(root) || plan.nodes.size() < 2 || !collector.closed()) return false;
    plan.expanded_leaves = plan.nodes.at(root).expanded;
    std::map<Sig, int> leaf_index;
    std::map<Cell *, std::vector<int>> incoming;
    std::vector<std::vector<int>> leaf_terms;
    incoming[root].push_back(1);
    for (auto it = plan.postorder.rbegin(); it != plan.postorder.rend(); ++it) {
        const auto &node = plan.nodes.at(*it);
        int activation = plan.controls.merge(incoming[*it]);
        std::vector<int> selection;
        for (auto bit : node.select) selection.push_back(plan.controls.input(bit));
        int default_condition = node.full ? 0 : plan.controls.make(BoolDag::NOT, plan.controls.merge(selection));
        plan.input_arms += int(node.edges.size());
        plan.old_estimate += plan.width*bank_cost(int(node.edges.size()));
        if (!node.full) plan.old_estimate += nor_cost(GetSize(node.select));
        for (auto &edge : node.edges) {
            int condition = edge.is_default ? default_condition : plan.controls.input(edge.condition);
            int guard = plan.controls.make(BoolDag::AND, activation, condition);
            if (edge.child) incoming[edge.child].push_back(guard);
            else {
                auto found = leaf_index.find(edge.value);
                int index;
                if (found == leaf_index.end()) {
                    index = int(plan.leaves.size()); leaf_index.emplace(edge.value, index);
                    plan.leaves.push_back(edge.value); leaf_terms.emplace_back();
                } else index = found->second;
                leaf_terms[size_t(index)].push_back(guard);
            }
        }
    }
                                                                            
                                                                               
    int unique = int(plan.leaves.size());
    if (unique < 8 || unique > 24 || unique*4 > plan.input_arms*3 || unique+2 > plan.input_arms) return false;
                                                                                
                                                                            
    for (int i = 0; i+1 < unique; i++) plan.guards.push_back(plan.controls.merge(leaf_terms[size_t(i)]));
    plan.control_gates = plan.controls.gates(plan.guards);
    plan.new_estimate = plan.width*bank_cost(unique) + nor_cost(unique-1) + plan.control_gates;
    plan.saving = plan.old_estimate - plan.new_estimate;
    return plan.saving >= plan.width;
}

struct Emitter {
    Module *module;
    const BoolDag &dag;
    std::vector<Bit> values;
    std::vector<bool> emitted;
    Emitter(Module *m, const BoolDag &d) : module(m), dag(d), values(d.expressions.size()), emitted(d.expressions.size(), false) {}
    Bit value(int id) {
        if (emitted[size_t(id)]) return values[size_t(id)];
        const auto &expression = dag.expressions[size_t(id)];
        Bit out;
        if (expression.kind == BoolDag::ZERO) out = Bit(State::S0);
        else if (expression.kind == BoolDag::ONE) out = Bit(State::S1);
        else if (expression.kind == BoolDag::INPUT) out = expression.bit;
        else {
            Bit a = value(expression.a);
            out = Bit(module->addWire(NEW_ID));
            if (expression.kind == BoolDag::NOT) module->addNot(NEW_ID, a, out);
            else {
                Bit b = value(expression.b);
                if (expression.kind == BoolDag::AND) module->addAnd(NEW_ID, a, b, out);
                else module->addOr(NEW_ID, a, b, out);
            }
        }
        emitted[size_t(id)] = true; values[size_t(id)] = out; return out;
    }
};

inline void emit(Module *module, const Plan &plan)
{
    Emitter emitter(module, plan.controls);
    Sig branches, selects;
    for (size_t i = 0; i < plan.guards.size(); i++) { branches.append(plan.leaves[i]); selects.append(emitter.value(plan.guards[i])); }
    auto replacement = module->addPmux(NEW_ID, plan.leaves.back(), branches, selects, plan.output);
    replacement->attributes = plan.root->attributes;
    replacement->set_bool_attribute(ID(pmux_dag_flattened));
    log("PMUX_DAG_SHARE module=%s root=%s width=%d nodes=%d input_arms=%d expanded_leaves=%d unique_leaves=%d control_gates=%d old_estimate=%d new_estimate=%d estimated_saved=%d\n",
        log_id(module), log_id(plan.root), plan.width, int(plan.nodes.size()), plan.input_arms,
        plan.expanded_leaves, int(plan.leaves.size()), plan.control_gates, plan.old_estimate, plan.new_estimate, plan.saving);
}
}                             

inline int pmux_share_dags(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_dag_detail;
    int changed = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        pool<Cell *> selected;
        std::vector<Cell *> roots;
        for (auto cell : module->selected_cells()) if (cell->type == ID($pmux)) { selected.insert(cell); roots.push_back(cell); }
        if (roots.size() < 2) continue;
        Context context(module);
        if (!context.complete) continue;
        std::vector<Plan> plans;
        int inspected = 0;
        for (auto root : roots) {
            if (++inspected > 256) break;
            Plan plan;
            if (make_plan(context, selected, root, plan)) plans.push_back(std::move(plan));
        }
        std::sort(plans.begin(), plans.end(), [](const Plan &a, const Plan &b) {
            if (a.nodes.size() != b.nodes.size()) return a.nodes.size() > b.nodes.size();
            if (a.saving != b.saving) return a.saving > b.saving;
            return a.root->name < b.root->name;
        });
        pool<Cell *> removed;
        for (auto &plan : plans) {
            bool overlaps = false;
            for (auto &entry : plan.nodes) if (removed.count(entry.first)) overlaps = true;
            if (overlaps) continue;
            emit(module, plan);
            for (auto &entry : plan.nodes) removed.insert(entry.first);
            changed++;
        }
        for (auto cell : removed) module->remove(cell);
    }
    return changed;
}
#endif

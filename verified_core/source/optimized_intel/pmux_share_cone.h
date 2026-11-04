// SPDX-License-Identifier: MIT
// V2: up to three selection layers, W>=3, and common XOR/AND/OR factors.
// Bounded, exact combinational resource sharing inside private selection cones.
// Include after pmux_resource_share.h (the pmux_share_resources.h component).
// No pass invocations, subprocesses, register rewrites, or case-name decisions.
#ifndef PMUX_SHARE_CONE_20261101_H
#define PMUX_SHARE_CONE_20261101_H
#include "kernel/yosys.h"
#include "pmux_guard_truth.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace pmux_cone_detail {
using namespace Yosys;
using namespace pmux_resource_detail;

struct Term { Cell *selection; int slot; };
struct Leaf {
    Sig value, left, right;
    std::vector<Term> path;
    Cell *operation = nullptr;
    RTLIL::IdString kind;
    int group = -1;
};
struct Group {
    // 0: ADD/SUB, 1: common-factor XOR, 2: untouched data, 3: AND, 4: OR.
    int kind;
    std::vector<int> leaves;
    std::vector<Sig> left, right;
    std::set<Cell *> operations;
    bool has_add = false, has_sub = false;
};
struct Plan {
    Cell *root = nullptr;
    int width = 0, old_selection_cost = 0, estimated_saved = 0;
    std::set<Cell *> selections;
    std::vector<Leaf> leaves;
    std::vector<Group> groups;
};

inline bool eligible_selection(Context &ctx, Cell *cell, int width)
{
    if (!cell || (cell->type != ID($pmux) && cell->type != ID($mux)) ||
        protected_object(cell) || cell->get_bool_attribute(ID(pmux_resource_operand)) ||
        cell->get_bool_attribute(ID(pmux_cone_operand))) return false;
    if (GetSize(ctx.sigmap(cell->getPort(ID::Y))) != width ||
        GetSize(ctx.sigmap(cell->getPort(ID::A))) != width ||
        ctx.protected_signal(ctx.sigmap(cell->getPort(ID::Y)))) return false;
    int count = GetSize(cell->getPort(ID::S));
    return count >= 1 && count <= 15 && GetSize(cell->getPort(ID::B)) == count*width &&
        (cell->type != ID($mux) || count == 1);
}

inline bool collect(Context &ctx, Plan &plan, Cell *cell, int depth,
                    const std::vector<Term> &path, std::set<Cell *> active, bool allow_dag_root = false)
{
    if (!eligible_selection(ctx, cell, plan.width) || active.count(cell)) return false;
    Sig s = ctx.sigmap(cell->getPort(ID::S));
    SelectionProof proof;
    // The attribute only restricts the second search. Every post-DAG root
    // must independently pass the bounded truth-table proof, even if the
    // earlier equality-decoder proof could also succeed.
    const bool post_dag_root = allow_dag_root && cell == plan.root &&
        cell->get_bool_attribute(ID(pmux_dag_flattened));
    bool proved = post_dag_root ? pmux_guard_truth_detail::prove(ctx, s, proof) :
        prove_selection(ctx, cell, s, proof);
    if (!proved) return false;
    active.insert(cell);
    if (plan.selections.insert(cell).second)
        plan.old_selection_cost += (GetSize(s) - (proof.full ? 1 : 0))*plan.width;
    Sig a = ctx.sigmap(cell->getPort(ID::A)), b = ctx.sigmap(cell->getPort(ID::B));
    for (int slot = -1; slot < GetSize(s); slot++) {
        if ((slot < 0 && proof.full) || (slot >= 0 && !proof.reachable[slot])) continue;
        Sig value = slot < 0 ? a : b.extract(slot*plan.width, plan.width);
        std::vector<Term> next = path;
        next.push_back({cell, slot});
        Cell *child = ctx.whole_driver(value);
        bool descend = depth < 2 && eligible_selection(ctx, child, plan.width) && ctx.private_to(child, cell);
        if (descend) {
            // Failure to prove a child does not authorize flattening it.
            SelectionProof child_proof;
            if (!prove_selection(ctx, child, ctx.sigmap(child->getPort(ID::S)), child_proof)) descend = false;
        }
        if (descend) {
            if (!collect(ctx, plan, child, depth+1, next, active, allow_dag_root)) return false;
        } else {
            Leaf leaf; leaf.value = value; leaf.path = next;
            plan.leaves.push_back(leaf);
            if (plan.leaves.size() > 32) return false;
        }
    }
    return true;
}

inline bool operation_private_to_cone(Context &ctx, Cell *op, const Plan &plan)
{
    if (!op || protected_object(op) || !op->hasPort(ID::Y)) return false;
    for (auto bit : ctx.sigmap(op->getPort(ID::Y))) {
        if (!bit.wire || ctx.output_bits.count(bit) || ctx.protected_bits.count(bit) ||
            ctx.multiple.count(bit) || !ctx.consumers.count(bit)) return false;
        for (auto user : ctx.consumers.at(bit)) if (!plan.selections.count(user)) return false;
    }
    return true;
}

inline void classify(Context &ctx, Plan &plan)
{
    for (auto &leaf : plan.leaves) {
        Cell *op = ctx.whole_driver(leaf.value);
        if (!op || !op->type.in(ID($add), ID($sub), ID($xor), ID($and), ID($or)) || !operation_private_to_cone(ctx, op, plan)) continue;
        Sig a = ctx.sigmap(op->getPort(ID::A)), b = ctx.sigmap(op->getPort(ID::B));
        // Equal input/output widths make signed and unsigned ADD/SUB identical
        // modulo 2^W; never move a truncation or infer an extension here.
        if (GetSize(a) != plan.width || GetSize(b) != plan.width ||
            op->getParam(ID::A_WIDTH).as_int() != plan.width ||
            op->getParam(ID::B_WIDTH).as_int() != plan.width ||
            op->getParam(ID::Y_WIDTH).as_int() != plan.width ||
            !known_literals(a) || !known_literals(b) ||
            ctx.protected_signal(a) || ctx.protected_signal(b)) continue;
        leaf.operation = op; leaf.kind = op->type; leaf.left = a; leaf.right = b;
    }
}

inline int operand_cost(const std::vector<Sig> &left, const std::vector<Sig> &right)
{
    return classes(left) + classes(right) - 2;
}

inline void orient_adds(Plan &plan, Group &group)
{
    std::vector<int> movable;
    for (int i = 0; i < int(group.leaves.size()); i++)
        if (plan.leaves[group.leaves[i]].kind == ID($add)) movable.push_back(i);
    // A deterministic bounded search on operands only, not on mapping results.
    // Enumerate at most ten choices; for bigger groups use two greedy sweeps.
    if (movable.size() <= 10) {
        auto original_left = group.left, original_right = group.right;
        auto best_left = group.left, best_right = group.right;
        int best = operand_cost(group.left, group.right);
        for (unsigned mask = 1; mask < (1u << movable.size()); mask++) {
            auto a = original_left, b = original_right;
            for (int j = 0; j < int(movable.size()); j++) if ((mask >> j) & 1u) std::swap(a[movable[j]], b[movable[j]]);
            int cost = operand_cost(a, b);
            if (cost < best) { best = cost; best_left = a; best_right = b; }
        }
        group.left = best_left; group.right = best_right;
    } else {
        for (int pass = 0; pass < 2; pass++) for (int i : movable) {
            int before = operand_cost(group.left, group.right);
            std::swap(group.left[i], group.right[i]);
            if (operand_cost(group.left, group.right) >= before) std::swap(group.left[i], group.right[i]);
        }
    }
}

inline bool make_plan(Context &ctx, Cell *root, Plan &plan, bool post_dag_only = false)
{
    if (post_dag_only && !root->get_bool_attribute(ID(pmux_dag_flattened))) return false;
    plan.root = root; plan.width = GetSize(root->getPort(ID::Y));
    // Narrow cones are allowed to compete on the same cost test. Their control
    // overhead is charged explicitly below, rather than hidden by a W>=6 gate.
    if (plan.width < 3 || plan.width > 128 || !eligible_selection(ctx, root, plan.width)) return false;
    Sig y = ctx.sigmap(root->getPort(ID::Y));
    std::set<Bit> unique;
    for (auto bit : y) if (!bit.wire || ctx.multiple.count(bit) || ctx.input_bits.count(bit) || !unique.insert(bit).second) return false;
    if (!collect(ctx, plan, root, 0, {}, {}, post_dag_only) ||
        (plan.selections.size() < 2 && !post_dag_only)) return false;
    classify(ctx, plan);

    Group arithmetic; arithmetic.kind = 0;
    for (int i = 0; i < int(plan.leaves.size()); i++) {
        auto &leaf = plan.leaves[i];
        if (leaf.operation && leaf.kind.in(ID($add), ID($sub))) {
            arithmetic.leaves.push_back(i); arithmetic.left.push_back(leaf.left); arithmetic.right.push_back(leaf.right);
            arithmetic.operations.insert(leaf.operation);
            arithmetic.has_add |= leaf.kind == ID($add); arithmetic.has_sub |= leaf.kind == ID($sub);
        }
    }
    if (arithmetic.operations.size() >= 3) {
        orient_adds(plan, arithmetic);
        plan.groups.push_back(arithmetic);
        for (int i : arithmetic.leaves) plan.leaves[i].group = 0;
    }

    // One exact common operand may be a current-state register word. Its value
    // is arbitrary; this does not use initialization or move a register.
    for (auto kind : {ID($xor),ID($and),ID($or)}) {
        std::set<Sig> candidates;
        for (auto &leaf : plan.leaves) if (leaf.group < 0 && leaf.operation && leaf.kind == kind) {
            candidates.insert(leaf.left); candidates.insert(leaf.right);
        }
        Group best;
        best.kind = kind == ID($xor) ? 1 : kind == ID($and) ? 3 : 4;
        for (auto common : candidates) {
            Group group; group.kind = best.kind;
            for (int i = 0; i < int(plan.leaves.size()); i++) {
                auto &leaf = plan.leaves[i];
                if (leaf.group >= 0 || !leaf.operation || leaf.kind != kind) continue;
                if (leaf.left != common && leaf.right != common) continue;
                group.leaves.push_back(i); group.operations.insert(leaf.operation);
                group.left.push_back(leaf.left == common ? leaf.right : leaf.left); group.right.push_back(common);
            }
            if (group.operations.size() > best.operations.size()) best = group;
        }
        if (best.operations.size() >= 2) {
            int index = int(plan.groups.size()); plan.groups.push_back(best);
            for (int i : best.leaves) plan.leaves[i].group = index;
        }
    }
    if (plan.groups.empty()) return false;
    Group passthrough; passthrough.kind = 2;
    for (int i = 0; i < int(plan.leaves.size()); i++) if (plan.leaves[i].group < 0) {
        passthrough.leaves.push_back(i); passthrough.left.push_back(plan.leaves[i].value);
    }
    if (!passthrough.leaves.empty()) plan.groups.push_back(passthrough);

    // A word-wide operation can also feed a sliced/permuted passthrough arm.
    // In that case its original output remains necessary, so it cannot be
    // counted as an eliminated operator. Reject rather than overstate savings.
    std::set<Cell *> shared_ops;
    for (auto &group : plan.groups) shared_ops.insert(group.operations.begin(),group.operations.end());
    for (auto &leaf : plan.leaves) for (auto bit : leaf.value)
        if (bit.wire && ctx.driver.count(bit) && shared_ops.count(ctx.driver.at(bit)) &&
            (leaf.group < 0 || leaf.operation != ctx.driver.at(bit))) return false;

    int old_cost = plan.old_selection_cost, new_cost = 0;
    int eliminated_operators = 0;
    for (auto &group : plan.groups) {
        if (group.kind == 2) { new_cost += (classes(group.left)-1)*plan.width; continue; }
        int unit = group.kind == 0 ? 2 : 1;
        old_cost += int(group.operations.size())*unit*plan.width;
        new_cost += (unit + operand_cost(group.left, group.right) +
            (group.kind == 0 && group.has_add && group.has_sub ? 1 : 0))*plan.width;
        eliminated_operators += int(group.operations.size())-1;
    }
    new_cost += (int(plan.groups.size())-1)*plan.width;
    // Explicit conservative cost for AND path terms, default predicates and
    // regrouping guards; real ABC/LUT mapping is still the acceptance test.
    new_cost += int(plan.leaves.size()) + 2*int(plan.selections.size());
    plan.estimated_saved = old_cost - new_cost;
    return eliminated_operators >= 2 && plan.estimated_saved >= 2*plan.width;
}

struct Emit {
    Context &ctx;
    Module *m;
    std::map<std::tuple<int,Bit,Bit>,Bit> boolean_cache;
    std::map<Cell *, Bit> defaults;
    explicit Emit(Context &ctx) : ctx(ctx), m(ctx.module) {}
    Bit gate(int kind, Bit a, Bit b)
    {
        Bit zero(State::S0), one(State::S1);
        if (kind == 0) { if (a == zero || b == zero) return zero; if (a == one) return b; if (b == one || a == b) return a; }
        else { if (a == one || b == one) return one; if (a == zero) return b; if (b == zero || a == b) return a; }
        if (b < a) std::swap(a,b);
        auto key = std::make_tuple(kind,a,b);
        if (boolean_cache.count(key)) return boolean_cache.at(key);
        Bit y(m->addWire(NEW_ID));
        if (kind == 0) m->addAnd(NEW_ID,a,b,y); else m->addOr(NEW_ID,a,b,y);
        boolean_cache[key] = y; return y;
    }
    Bit reduce_or(const std::vector<Bit> &bits)
    {
        Bit y(State::S0); for (auto bit : bits) y = gate(1,y,bit); return y;
    }
    Bit predicate(const Term &term)
    {
        Sig s = ctx.sigmap(term.selection->getPort(ID::S));
        if (term.slot >= 0) return s[term.slot];
        if (defaults.count(term.selection)) return defaults.at(term.selection);
        Bit out(m->addWire(NEW_ID)); m->addLogicNot(NEW_ID,s,out);
        defaults[term.selection] = out; return out;
    }
    Sig choose(const std::vector<Sig> &values, const std::vector<Bit> &guards)
    {
        std::map<Sig,std::vector<Bit>> merged;
        for (int i = 0; i < int(values.size()); i++) merged[values[i]].push_back(guards[i]);
        if (merged.size() == 1) return merged.begin()->first;
        // The group-inactive result is unobservable. Make the most frequent
        // existing word the default, so its entire guard union need not be
        // emitted. This changes no active selected value or literal X/Z.
        auto default_it = merged.begin();
        for (auto it = merged.begin(); it != merged.end(); ++it)
            if (it->second.size() > default_it->second.size()) default_it = it;
        Sig first = default_it->first, data, selects;
        for (auto &item : merged) {
            if (item.first == first) continue;
            data.append(item.first); selects.append(reduce_or(item.second));
        }
        Sig y(m->addWire(NEW_ID,GetSize(first)));
        auto cell = m->addPmux(NEW_ID,first,data,selects,y);
        cell->set_bool_attribute(ID(pmux_cone_operand));
        cell->set_bool_attribute(ID(pmux_resource_operand));
        return y;
    }
};

inline void emit_plan(Context &ctx, const Plan &plan)
{
    Emit emit(ctx);
    std::vector<Bit> guards;
    for (auto &leaf : plan.leaves) {
        Bit guard(State::S1);
        for (auto &term : leaf.path) guard = emit.gate(0,guard,emit.predicate(term));
        guards.push_back(guard);
    }
    std::vector<Sig> results; std::vector<Bit> hits;
    for (auto &group : plan.groups) {
        std::vector<Bit> group_guards;
        for (int index : group.leaves) group_guards.push_back(guards[index]);
        hits.push_back(emit.reduce_or(group_guards));
        Sig left = emit.choose(group.left,group_guards);
        if (group.kind == 2) { results.push_back(left); continue; }
        Sig right = emit.choose(group.right,group_guards);
        Sig out(ctx.module->addWire(NEW_ID,plan.width));
        if (group.kind == 1) ctx.module->addXor(NEW_ID,left,right,out);
        else if (group.kind == 3) ctx.module->addAnd(NEW_ID,left,right,out);
        else if (group.kind == 4) ctx.module->addOr(NEW_ID,left,right,out);
        else if (!group.has_sub) ctx.module->addAdd(NEW_ID,left,right,out);
        else if (!group.has_add) ctx.module->addSub(NEW_ID,left,right,out);
        else {
            std::vector<Bit> sub_guards;
            for (int index : group.leaves) if (plan.leaves[index].kind == ID($sub)) sub_guards.push_back(guards[index]);
            Bit sub = emit.reduce_or(sub_guards);
            auto alu = ctx.module->addCell(NEW_ID,ID($alu));
            alu->setParam(ID::A_WIDTH,plan.width); alu->setParam(ID::B_WIDTH,plan.width); alu->setParam(ID::Y_WIDTH,plan.width);
            alu->setParam(ID::A_SIGNED,false); alu->setParam(ID::B_SIGNED,false);
            alu->setPort(ID::A,left); alu->setPort(ID::B,right);
            alu->setPort(ID::BI,sub); alu->setPort(ID::CI,sub); alu->setPort(ID::Y,out);
            alu->setPort(ID::X,Sig(ctx.module->addWire(NEW_ID,plan.width)));
            alu->setPort(ID::CO,Sig(ctx.module->addWire(NEW_ID,plan.width)));
        }
        results.push_back(out);
    }
    // Exactly one complete path is selected in the proven two-state model.
    // Arbitrary group-inactive operand defaults never affect this result.
    Sig result = emit.choose(results,hits);
    ctx.module->connect(plan.root->getPort(ID::Y),result);
    log("PMUX_SHARE_CONE module=%s root=%s width=%d selection_nodes=%d leaves=%d groups=%d estimated_saved=%d\n",
        log_id(ctx.module),log_id(plan.root),plan.width,int(plan.selections.size()),
        int(plan.leaves.size()),int(plan.groups.size()),plan.estimated_saved);
}
} // namespace pmux_cone_detail

inline int pmux_share_cones(Yosys::RTLIL::Design *design, bool post_dag_only = false)
{
    using namespace Yosys;
    using namespace pmux_resource_detail;
    using namespace pmux_cone_detail;
    int changed = 0, inspected_post_dag = 0;
    for (auto module : design->selected_modules()) {
        if (post_dag_only && inspected_post_dag >= 8) break;
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        Context ctx(module);
        if (!ctx.complete) continue;
        std::vector<Plan> plans;
        for (auto root : module->selected_cells()) if (root->type.in(ID($pmux),ID($mux))) {
            if (post_dag_only && !root->get_bool_attribute(ID(pmux_dag_flattened))) continue;
            if (post_dag_only && ++inspected_post_dag > 8) break;
            Plan plan; if (make_plan(ctx,root,plan,post_dag_only)) plans.push_back(plan);
        }
        std::stable_sort(plans.begin(),plans.end(),[](const Plan &a,const Plan &b){return a.estimated_saved>b.estimated_saved;});
        std::set<Cell *> used;
        std::vector<Cell *> removed;
        for (auto &plan : plans) {
            if (changed >= 64) break;
            bool conflict = false;
            for (auto cell : plan.selections) if (used.count(cell)) conflict = true;
            for (auto &group : plan.groups) for (auto cell : group.operations) if (used.count(cell)) conflict = true;
            if (conflict) continue;
            emit_plan(ctx,plan); removed.push_back(plan.root); changed++;
            used.insert(plan.selections.begin(),plan.selections.end());
            for (auto &group : plan.groups) used.insert(group.operations.begin(),group.operations.end());
        }
        // Immutable driver/consumer pointers remain alive through all plans.
        // Standard downstream opt_clean reclaims now-unused private cones.
        for (auto root : removed) module->remove(root);
    }
    return changed;
}
#endif

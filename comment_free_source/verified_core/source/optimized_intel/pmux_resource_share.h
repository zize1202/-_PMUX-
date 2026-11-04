// SPDX-License-Identifier: MIT
// PMUX-local, fixed-width resource sharing. No subprocesses or nested passes.
// Acceptance still requires the original-to-final sequential EQY and LUT4 PPA.
#ifndef PMUX_SHARE_RESOURCES_20261101_H
#define PMUX_SHARE_RESOURCES_20261101_H
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace pmux_resource_detail {
using namespace Yosys;
using Sig = RTLIL::SigSpec;
using Bit = RTLIL::SigBit;
using Cell = RTLIL::Cell;
using Module = RTLIL::Module;

inline bool protected_object(const RTLIL::AttrObject *o)
{
    return o->get_bool_attribute(ID::keep) || o->get_bool_attribute(ID(dont_touch));
}

inline bool known_literals(const Sig &s)
{
    for (auto bit : s)
        if (!bit.wire && bit != Bit(State::S0) && bit != Bit(State::S1))
            return false;
    return true;
}

struct Context {
    Module *module;
    SigMap sigmap;
    dict<Bit, Cell *> driver;
    dict<Bit, pool<Cell *>> consumers;
    pool<Bit> multiple, input_bits, output_bits, protected_bits;
    bool complete = true;

    explicit Context(Module *m) : module(m), sigmap(m)
    {
        CellTypes types;
        types.setup(m->design);
        for (auto wire : m->wires()) {
            for (auto bit : sigmap(wire)) {
                if (wire->port_input) input_bits.insert(bit);
                if (wire->port_output) output_bits.insert(bit);
                if (protected_object(wire)) protected_bits.insert(bit);
            }
        }
        for (auto cell : m->cells()) {
            if (!types.cell_known(cell->type)) { complete = false; continue; }
            for (auto &port : cell->connections()) {
                for (auto bit : sigmap(port.second)) {
                    if (!bit.wire) continue;
                    if (types.cell_output(cell->type, port.first)) {
                        if (driver.count(bit) && driver.at(bit) != cell) multiple.insert(bit);
                        driver[bit] = cell;
                    }
                    if (types.cell_input(cell->type, port.first)) consumers[bit].insert(cell);
                }
            }
        }
    }

    bool protected_signal(const Sig &s) const
    {
        for (auto bit : s) if (protected_bits.count(bit)) return true;
        return false;
    }

    Cell *whole_driver(const Sig &s) const
    {
        if (s.empty()) return nullptr;
        Cell *found = nullptr;
        pool<Bit> seen;
        for (auto bit : s) {
            if (!bit.wire || multiple.count(bit) || input_bits.count(bit) ||
                !driver.count(bit) || seen.count(bit)) return nullptr;
            seen.insert(bit);
            auto cell = driver.at(bit);
            if (found && found != cell) return nullptr;
            found = cell;
        }
        if (!found || !found->hasPort(ID::Y) || sigmap(found->getPort(ID::Y)) != s)
            return nullptr;
        return found;
    }

    bool private_to(Cell *producer, Cell *consumer) const
    {
        if (protected_object(producer)) return false;
        for (auto bit : sigmap(producer->getPort(ID::Y))) {
            if (output_bits.count(bit) || protected_bits.count(bit) || multiple.count(bit)) return false;
            if (!consumers.count(bit) || consumers.at(bit).size() != 1 ||
                !consumers.at(bit).count(consumer)) return false;
        }
        return true;
    }
};

                                                                        
                                                                            
struct DecodeSet {
    Sig selector;
    Bit enable = Bit(State::S1);
    std::set<unsigned> codes;
};

inline bool decode_condition(Context &ctx, Bit bit, DecodeSet &out, int depth, int &budget)
{
    if (depth > 8 || --budget < 0) return false;
    bit = ctx.sigmap(bit);
    if (bit == Bit(State::S0)) return true;
    if (!bit.wire || ctx.multiple.count(bit) || !ctx.driver.count(bit)) return false;
    Cell *cell = ctx.driver.at(bit);
    if (protected_object(cell) || !cell->hasPort(ID::Y) ||
        GetSize(ctx.sigmap(cell->getPort(ID::Y))) != 1) return false;

    if (cell->type == ID($eq) || cell->type == ID($logic_not)) {
        Sig selector, constant;
        if (cell->type == ID($logic_not)) {
            selector = ctx.sigmap(cell->getPort(ID::A));
            constant = Sig(RTLIL::Const(0, GetSize(selector)));
        } else {
            if (cell->getParam(ID::A_SIGNED).as_bool() || cell->getParam(ID::B_SIGNED).as_bool()) return false;
            Sig a = ctx.sigmap(cell->getPort(ID::A)), b = ctx.sigmap(cell->getPort(ID::B));
            if (a.is_fully_const() && known_literals(a)) { constant = a; selector = b; }
            else if (b.is_fully_const() && known_literals(b)) { constant = b; selector = a; }
            else return false;
        }
        const int width = GetSize(selector);
        if (width < 1 || width > 10 || !known_literals(selector)) return false;
        for (auto sbit : selector) if (!sbit.wire) return false;
        unsigned code = 0;
        bool impossible = false;
        for (int i = 0; i < GetSize(constant); i++) if (constant[i] == Bit(State::S1)) {
            if (i >= width) impossible = true;
            else code |= unsigned(1) << i;
        }
        out.selector = selector;
        if (!impossible) out.codes.insert(code);
        return true;
    }

    if (cell->type == ID($and) || cell->type == ID($logic_and)) {
        Sig a = ctx.sigmap(cell->getPort(ID::A)), b = ctx.sigmap(cell->getPort(ID::B));
        if (GetSize(a) != 1 || GetSize(b) != 1 || !known_literals(a) || !known_literals(b)) return false;
        DecodeSet left, right;
        bool dl = decode_condition(ctx, a[0], left, depth + 1, budget);
        bool dr = decode_condition(ctx, b[0], right, depth + 1, budget);
        if (dl == dr) return false;
        out = dl ? left : right;
        if (out.codes.empty()) return true;
        if (out.enable != Bit(State::S1)) return false;
        out.enable = dl ? b[0] : a[0];
        return true;
    }

    Sig terms;
    if (cell->type == ID($reduce_or)) terms = ctx.sigmap(cell->getPort(ID::A));
    else if (cell->type == ID($or) || cell->type == ID($logic_or)) {
        Sig a = ctx.sigmap(cell->getPort(ID::A)), b = ctx.sigmap(cell->getPort(ID::B));
        if (GetSize(a) != 1 || GetSize(b) != 1) return false;
        terms = a; terms.append(b);
    } else return false;
    if (GetSize(terms) < 1 || GetSize(terms) > 64) return false;
    bool have = false;
    for (auto term : terms) {
        DecodeSet child;
        if (!decode_condition(ctx, term, child, depth + 1, budget)) return false;
        if (child.codes.empty()) continue;
        if (!have) { out.selector = child.selector; out.enable = child.enable; have = true; }
        if (out.selector != child.selector || out.enable != child.enable) return false;
        out.codes.insert(child.codes.begin(), child.codes.end());
        if (out.codes.size() > 1024) return false;
    }
    return true;
}

struct SelectionProof {
    bool full = false;
    std::vector<bool> reachable;
};

inline bool prove_selection(Context &ctx, Cell *root, const Sig &s, SelectionProof &proof)
{
    proof.reachable.assign(GetSize(s), true);
    if (root->type == ID($mux) || GetSize(s) == 1) {
        if (!known_literals(s)) return false;
        if (s[0] == Bit(State::S0)) proof.reachable[0] = false;
        proof.full = s[0] == Bit(State::S1);
        return true;
    }
    DecodeSet merged;
    bool have = false;
    int budget = 512;
    for (int i = 0; i < GetSize(s); i++) {
        DecodeSet arm;
        if (!decode_condition(ctx, s[i], arm, 0, budget)) return false;
        proof.reachable[i] = !arm.codes.empty();
        if (arm.codes.empty()) continue;
        if (!have) { merged.selector = arm.selector; merged.enable = arm.enable; have = true; }
        if (merged.selector != arm.selector || merged.enable != arm.enable) return false;
        for (auto code : arm.codes) if (!merged.codes.insert(code).second) return false;
    }
    if (!have) return false;
    proof.full = merged.enable == Bit(State::S1) &&
        merged.codes.size() == (size_t(1) << GetSize(merged.selector));
    return true;
}

struct Arm {
    int slot = -1;
    Cell *operation = nullptr;
    RTLIL::IdString kind;
    Sig left, right;
};

inline bool read_arm(Context &ctx, Cell *root, int slot, const Sig &value, int width, Arm &arm)
{
    auto op = ctx.whole_driver(value);
    if (!op || protected_object(op) || (op->type != ID($xor) && op->type != ID($add) && op->type != ID($sub)))
        return false;
    Sig a = ctx.sigmap(op->getPort(ID::A)), b = ctx.sigmap(op->getPort(ID::B));
    if (GetSize(a) != width || GetSize(b) != width ||
        op->getParam(ID::A_WIDTH).as_int() != width || op->getParam(ID::B_WIDTH).as_int() != width ||
        op->getParam(ID::Y_WIDTH).as_int() != width || !known_literals(a) || !known_literals(b)) return false;
    if (ctx.protected_signal(value) || ctx.protected_signal(a) || ctx.protected_signal(b) || !ctx.private_to(op, root))
        return false;
    arm.slot = slot; arm.operation = op; arm.kind = op->type; arm.left = a; arm.right = b;
    return true;
}

inline int classes(const std::vector<Sig> &values)
{
    return int(std::set<Sig>(values.begin(), values.end()).size());
}

inline Sig select_word(Module *m, const std::vector<Sig> &values, const Sig &s)
{
    bool same = true;
    for (auto &value : values) same = same && value == values.front();
    if (same) return values.front();
    Sig branches;
    for (size_t i = 1; i < values.size(); i++) branches.append(values[i]);
    Sig out(m->addWire(NEW_ID, GetSize(values.front())));
    auto cell = m->addPmux(NEW_ID, values.front(), branches, s, out);
    cell->set_bool_attribute(ID(pmux_resource_operand));
    return out;
}

inline bool try_share(Context &ctx, Cell *root, int &estimated_saved)
{
    if (protected_object(root) || root->get_bool_attribute(ID(pmux_resource_operand))) return false;
    const int w = root->getParam(ID::WIDTH).as_int();
    const int n = root->type == ID($mux) ? 1 : root->getParam(ID::S_WIDTH).as_int();
    if (w < 2 || w > 256 || n < 1 || n > 32) return false;
    Sig a = ctx.sigmap(root->getPort(ID::A)), b = ctx.sigmap(root->getPort(ID::B));
    Sig s = ctx.sigmap(root->getPort(ID::S)), y = ctx.sigmap(root->getPort(ID::Y));
    if (GetSize(a) != w || GetSize(b) != w*n || GetSize(s) != n || GetSize(y) != w ||
        !known_literals(s) || ctx.protected_signal(y)) return false;
    pool<Bit> seen_y;
    for (auto bit : y) {
        if (!bit.wire || ctx.multiple.count(bit) || ctx.input_bits.count(bit) || seen_y.count(bit)) return false;
        seen_y.insert(bit);
    }
    SelectionProof proof;
    if (!prove_selection(ctx, root, s, proof)) return false;

    std::vector<Arm> arms;
    if (!proof.full) {
        Arm arm;
        if (!read_arm(ctx, root, -1, a, w, arm)) return false;
        arms.push_back(arm);
    }
    for (int i = 0; i < n; i++) if (proof.reachable[i]) {
        Arm arm;
        if (!read_arm(ctx, root, i, b.extract(i*w, w), w, arm)) return false;
        arms.push_back(arm);
    }
    if (arms.size() < 2 || arms.size() > 8) return false;
    pool<Cell *> private_ops;
    bool all_xor = true, all_add = true, all_sub = true, add_sub_only = true;
    for (auto &arm : arms) {
        private_ops.insert(arm.operation);
        all_xor &= arm.kind == ID($xor);
        all_add &= arm.kind == ID($add);
        all_sub &= arm.kind == ID($sub);
        add_sub_only &= arm.kind == ID($add) || arm.kind == ID($sub);
    }
    if (private_ops.size() < 2 || (!all_xor && !add_sub_only)) return false;

                                                                           
                                                                          
    std::vector<Sig> best_left, best_right;
    int best_mux = 1000000;
    if (all_xor) {
                                                                
        for (auto common : {arms.front().left, arms.front().right}) {
            std::vector<Sig> left, right;
            bool good = true;
            for (auto &arm : arms) {
                if (arm.left == common) { left.push_back(arm.right); right.push_back(common); }
                else if (arm.right == common) { left.push_back(arm.left); right.push_back(common); }
                else { good = false; break; }
            }
            if (good) {
                int cost = classes(left) - 1;
                if (cost < best_mux) { best_mux = cost; best_left = left; best_right = right; }
            }
        }
    } else if (all_add) {
                                                                              
                                                                                
        const unsigned choices = unsigned(1) << (arms.size() - 1);
        for (unsigned mask = 0; mask < choices; mask++) {
            std::vector<Sig> left, right;
            for (size_t i = 0; i < arms.size(); i++) {
                bool swap = i > 0 && ((mask >> (i - 1)) & 1);
                left.push_back(swap ? arms[i].right : arms[i].left);
                right.push_back(swap ? arms[i].left : arms[i].right);
            }
            int cost = classes(left) + classes(right) - 2;
            if (cost < best_mux) { best_mux = cost; best_left = left; best_right = right; }
        }
    } else {
                                                                           
                                                                
        for (auto &arm : arms) { best_left.push_back(arm.left); best_right.push_back(arm.right); }
        best_mux = classes(best_left) + classes(best_right) - 2;
        if (!all_sub && best_mux != 0) return false;
    }
    if (best_left.empty()) return false;
    const int unit = all_xor ? 1 : 2;
                                                                           
    const int old_estimate = (int(private_ops.size()) * unit + int(private_ops.size()) - 1) * w;
    const int new_estimate = (unit + best_mux + ((!all_add && !all_sub && !all_xor) ? 1 : 0)) * w;
    estimated_saved = old_estimate - new_estimate;
    if (estimated_saved < (all_xor ? w : 2*w)) return false;

                                                                            
    // this is licensed only by the proof above, not by treating literal X as 0.
    std::vector<Sig> left(n+1, best_left.front()), right(n+1, best_right.front());
    for (size_t i = 0; i < arms.size(); i++) {
        left[arms[i].slot + 1] = best_left[i];
        right[arms[i].slot + 1] = best_right[i];
    }
    Sig lhs = select_word(ctx.module, left, s), rhs = select_word(ctx.module, right, s);
    if (all_xor) ctx.module->addXor(NEW_ID, lhs, rhs, y);
    else if (all_add) ctx.module->addAdd(NEW_ID, lhs, rhs, y);
    else if (all_sub) ctx.module->addSub(NEW_ID, lhs, rhs, y);
    else {
        Sig sub_terms;
        bool default_sub = false;
        for (auto &arm : arms) if (arm.kind == ID($sub)) {
            if (arm.slot < 0) default_sub = true;
            else sub_terms.append(s[arm.slot]);
        }
        if (default_sub) {
            Sig zero_select(ctx.module->addWire(NEW_ID));
            ctx.module->addLogicNot(NEW_ID, s, zero_select);
            sub_terms.append(zero_select);
        }
        Sig submode(State::S0);
        if (GetSize(sub_terms) == 1) submode = sub_terms;
        else if (GetSize(sub_terms) > 1) {
            submode = Sig(ctx.module->addWire(NEW_ID));
            ctx.module->addReduceOr(NEW_ID, sub_terms, submode);
        }
        auto alu = ctx.module->addCell(NEW_ID, ID($alu));
        alu->setParam(ID::A_WIDTH, w); alu->setParam(ID::B_WIDTH, w); alu->setParam(ID::Y_WIDTH, w);
        alu->setParam(ID::A_SIGNED, false); alu->setParam(ID::B_SIGNED, false);
        alu->setPort(ID::A, lhs); alu->setPort(ID::B, rhs);
        alu->setPort(ID::CI, submode); alu->setPort(ID::BI, submode);
        alu->setPort(ID::Y, y);
        alu->setPort(ID::X, Sig(ctx.module->addWire(NEW_ID, w)));
        alu->setPort(ID::CO, Sig(ctx.module->addWire(NEW_ID, w)));
    }
    log("PMUX_RESOURCE_SHARE module=%s cell=%s kind=%s width=%d arms=%d old_private_ops=%d operand_mux_classes=%d estimated_saved=%d full_cover=%d\n",
        log_id(ctx.module), log_id(root), all_xor ? "common_xor" : all_add ? "add" : all_sub ? "sub" : "add_sub_pair",
        w, int(arms.size()), GetSize(private_ops), best_mux, estimated_saved, int(proof.full));
    return true;
}
}                                  

inline int pmux_share_resources(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_resource_detail;
    int rewrites = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        std::vector<Cell *> roots;
        for (auto cell : module->selected_cells())
            if (cell->type == ID($pmux) || cell->type == ID($mux)) roots.push_back(cell);
        if (roots.empty()) continue;
        Context context(module);
        if (!context.complete) continue;
        std::vector<Cell *> removed;
        for (auto root : roots) {
            if (rewrites >= 128) break;
            int estimated_saved = 0;
            if (try_share(context, root, estimated_saved)) { removed.push_back(root); rewrites++; }
        }
                                                                                  
                                                                           
        for (auto root : removed) module->remove(root);
    }
    return rewrites;
}
#endif

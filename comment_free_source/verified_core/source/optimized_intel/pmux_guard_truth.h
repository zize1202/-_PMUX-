// SPDX-License-Identifier: MIT
// Conservative bounded Boolean proof for a newly flattened PMUX's guards.
// Include after pmux_resource_share.h. Attributes never substitute for proof.
#ifndef PMUX_GUARD_TRUTH_H
#define PMUX_GUARD_TRUTH_H
#include <map>
#include <set>
#include <vector>
namespace pmux_guard_truth_detail {
using namespace Yosys;
using namespace pmux_resource_detail;
struct GuardTruth {
    Context &ctx;
    std::set<Bit> visited, active, inputs;
    std::map<Bit,int> indices;
    std::map<Bit,bool> values;
    bool valid = true;
    int budget = 512, address = 0;
    explicit GuardTruth(Context &ctx) : ctx(ctx) {}
    bool supported(Cell *cell) const {
        if (!cell || protected_object(cell) || !cell->hasPort(ID::Y) ||
            GetSize(ctx.sigmap(cell->getPort(ID::Y))) != 1) return false;
        if (cell->type.in(ID($and),ID($or),ID($xor),ID($xnor),ID($not))) {
            if (GetSize(cell->getPort(ID::A)) != 1) return false;
            if (cell->hasPort(ID::B) && GetSize(cell->getPort(ID::B)) != 1) return false;
        }
        return cell->type.in(ID($eq),ID($ne),ID($logic_not),ID($reduce_or),ID($reduce_and),
            ID($reduce_bool),ID($reduce_xor),ID($reduce_xnor),ID($and),ID($or),ID($xor),
            ID($xnor),ID($not),ID($logic_and),ID($logic_or));
    }
    void gather(Bit bit, int depth = 0) {
        bit = ctx.sigmap(bit);
        if (!valid) return;
        if (active.count(bit) || depth > 24 || ctx.multiple.count(bit)) { valid = false; return; }
        if (!visited.insert(bit).second) return;
        if (--budget < 0) { valid = false; return; }
        if (!bit.wire) { if (bit != Bit(State::S0) && bit != Bit(State::S1)) valid = false; return; }
        Cell *cell = ctx.driver.count(bit) ? ctx.driver.at(bit) : nullptr;
                                                                                 
                                                                              
        if (!supported(cell) || ctx.input_bits.count(bit)) {
            inputs.insert(bit); if (inputs.size() > 8) valid = false; return;
        }
        active.insert(bit);
        for (auto pin : {ID::A,ID::B}) if (cell->hasPort(pin))
            for (auto input : ctx.sigmap(cell->getPort(pin))) gather(input,depth+1);
        active.erase(bit);
    }
    bool any(const Sig &signal) { for (auto bit : signal) if (evaluate(bit)) return true; return false; }
    bool evaluate(Bit bit) {
        if (!bit.wire) return bit == Bit(State::S1);
        if (indices.count(bit)) return (address >> indices.at(bit)) & 1;
        if (values.count(bit)) return values.at(bit);
        Cell *cell = ctx.driver.at(bit);
        Sig a = ctx.sigmap(cell->getPort(ID::A));
        Sig b = cell->hasPort(ID::B) ? ctx.sigmap(cell->getPort(ID::B)) : Sig();
        bool value = false;
        if (cell->type.in(ID($eq),ID($ne))) {
            value = true;
            bool sign = cell->getParam(ID::A_SIGNED).as_bool() && cell->getParam(ID::B_SIGNED).as_bool();
            for (int i = 0; i < std::max(GetSize(a),GetSize(b)); i++) {
                Bit av = i < GetSize(a) ? a[i] : (sign && GetSize(a) ? a[GetSize(a)-1] : Bit(State::S0));
                Bit bv = i < GetSize(b) ? b[i] : (sign && GetSize(b) ? b[GetSize(b)-1] : Bit(State::S0));
                if (evaluate(av) != evaluate(bv)) { value = false; break; }
            }
            if (cell->type == ID($ne)) value = !value;
        } else if (cell->type.in(ID($not),ID($logic_not))) value = !any(a);
        else if (cell->type == ID($reduce_and)) { value = true; for (auto input : a) value &= evaluate(input); }
        else if (cell->type.in(ID($reduce_xor),ID($reduce_xnor))) {
            for (auto input : a) value ^= evaluate(input);
            if (cell->type == ID($reduce_xnor)) value = !value;
        } else if (cell->type.in(ID($reduce_or),ID($reduce_bool))) value = any(a);
        else if (cell->type.in(ID($and),ID($logic_and))) value = any(a) && any(b);
        else if (cell->type.in(ID($xor),ID($xnor))) { value = any(a) != any(b); if (cell->type == ID($xnor)) value = !value; }
        else value = any(a) || any(b);
        values[bit] = value; return value;
    }
    bool prove(const Sig &raw_selects, SelectionProof &proof) {
        Sig selects = ctx.sigmap(raw_selects);
        if (GetSize(selects) < 1 || GetSize(selects) > 15) return false;
        for (auto bit : selects) gather(bit);
        if (!valid) return false;
        int index = 0; for (auto bit : inputs) indices[bit] = index++;
        proof.full = true; proof.reachable.assign(GetSize(selects),false);
        for (address = 0; address < (1 << index); address++) {
            values.clear(); int active_count = 0;
            for (int i = 0; i < GetSize(selects); i++) if (evaluate(selects[i])) {
                proof.reachable[i] = true; if (++active_count > 1) return false;
            }
            if (!active_count) proof.full = false;
        }
        return true;
    }
};
inline bool prove(Context &ctx, const Sig &selects, SelectionProof &proof)
{
    GuardTruth checker(ctx); return checker.prove(selects,proof);
}
}
#endif

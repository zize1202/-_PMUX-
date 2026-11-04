// SPDX-License-Identifier: MIT
// Bounded, exact 5-address-bit constant-ROM cofactor sharing, before ABC9.
// No case/module-name matching, no clock/state edits, no external mapper calls.
#ifndef PMUX_SMALL_ROM_H
#define PMUX_SMALL_ROM_H
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/mem.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>
namespace pmux_small_rom_detail {
using namespace Yosys;
using Key = std::pair<unsigned, uint32_t>;                                         

inline bool protected_object(const RTLIL::AttrObject &object)
{
    return object.get_bool_attribute(ID::keep) ||
        object.get_bool_attribute(RTLIL::escape_id("dont_touch"));
}
inline std::vector<int> variables(unsigned mask)
{
    std::vector<int> result;
    for (int i = 0; i < 5; i++) if (mask & (1u << i)) result.push_back(i);
    return result;
}
inline Key reduced(unsigned mask, uint32_t table)
{
    while (true) {
        auto vars = variables(mask);
        int n = int(vars.size()), remove = -1;
        for (int i = 0; i < n && remove < 0; i++) {
            bool independent = true;
            for (int a = 0; a < (1 << n); a++)
                if (((table >> a) & 1u) != ((table >> (a ^ (1 << i))) & 1u)) {
                    independent = false; break;
                }
            if (independent) remove = i;
        }
        if (remove < 0) return {mask, table};
        uint32_t next = 0;
        for (int a = 0; a < (1 << (n-1)); a++) {
            int old = (a & ((1 << remove)-1)) | ((a >> remove) << (remove+1));
            next |= ((table >> old) & 1u) << a;
        }
        mask &= ~(1u << vars[remove]); table = next;
    }
}
struct Ref {
                                                                       
    int kind = 0, value = 0;
    bool invert = false;
    Key key;
};
inline Ref reference(unsigned mask, uint32_t table)
{
    auto value = reduced(mask, table);
    auto vars = variables(value.first);
    Ref ref;
    if (vars.empty()) { ref.value = int(value.second); return ref; }
    if (vars.size() == 1) {
        ref.kind = 1; ref.value = vars[0]; ref.invert = value.second == 1; return ref;
    }
    uint32_t full = (1u << (1u << vars.size()))-1u;                       
    uint32_t complement = value.second ^ full;
    ref.kind = 2; ref.key = {value.first, std::min(value.second, complement)};
    ref.invert = ref.key.second != value.second;
    return ref;
}
struct Choice {
    int split = 0;
    Ref a, b;
    std::set<Key> leaves;
};
inline Choice choice(uint32_t table, int split)
{
    Choice c; c.split = split;
    auto vars = variables(31u ^ (1u << split));
    for (int val = 0; val < 2; val++) {
        uint32_t cofactor = 0;
        for (int a = 0; a < 16; a++) {
            int full = val << split;
            for (int i = 0; i < 4; i++) full |= ((a >> i) & 1) << vars[i];
            cofactor |= ((table >> full) & 1u) << a;
        }
        Ref ref = reference(31u ^ (1u << split), cofactor);
        if (ref.kind == 2) c.leaves.insert(ref.key);
        if (val == 0) c.a = ref; else c.b = ref;
    }
    return c;
}
inline int additional(const std::set<Key> &keys, const std::set<Key> &existing)
{
    int result = 0;
    for (auto key : keys) if (!existing.count(key)) result++;
    return result;
}
struct Plan {
    SigSpec address, output;
    std::vector<uint32_t> truth;
    std::vector<std::array<Choice,5>> candidates;
    std::vector<int> selected;
    std::set<Key> leaves;
    int reference_cost = 0, candidate_cost = 0;
};
inline bool prepare(Mem &mem, SigMap &sigmap, const pool<SigBit> &protected_bits,
                    const pool<SigBit> &input_bits, Plan &plan)
{
    if (protected_object(mem) || mem.size != 32 || mem.start_offset != 0 ||
        mem.width < 2 || mem.width > 16 || !mem.wr_ports.empty() || mem.rd_ports.size() != 1)
        return false;
    for (auto &init : mem.inits)
        if (init.cell && protected_object(*init.cell)) return false;
    auto &rd = mem.rd_ports[0];
    if (rd.removed || protected_object(rd) || rd.clk_enable || rd.wide_log2 != 0 ||
        GetSize(rd.addr) != 5 || GetSize(rd.data) != mem.width ||
        sigmap(rd.en) != SigSpec(State::S1) || sigmap(rd.arst) != SigSpec(State::S0) ||
        sigmap(rd.srst) != SigSpec(State::S0)) return false;
    plan.address = sigmap(rd.addr); plan.output = sigmap(rd.data);
    std::set<SigBit> distinct;
    for (auto bit : plan.address)
        if (!bit.wire || protected_bits.count(bit) || !distinct.insert(bit).second) return false;
                                                                              
                                                                         
                                                                              
    for (auto bit : plan.output)
        if (!bit.wire || protected_bits.count(bit) || input_bits.count(bit) ||
            !distinct.insert(bit).second) return false;
    Const init = mem.get_init_data();
    if (GetSize(init) != mem.width*32 || !init.is_fully_def()) return false;
    plan.truth.resize(mem.width, 0);
    for (int bit = 0; bit < mem.width; bit++) {
        for (int addr = 0; addr < 32; addr++)
            if (init[addr*mem.width+bit] == State::S1) plan.truth[bit] |= 1u << addr;
        std::array<Choice,5> choices;
        for (int split = 0; split < 5; split++) choices[split] = choice(plan.truth[bit], split);
        plan.candidates.push_back(choices);
    }
                                                                          
    using Keys = std::bitset<160>;
    std::map<Key,int> indices;
    for (auto &choices : plan.candidates) for (auto &c : choices) for (auto key : c.leaves)
        if (!indices.count(key)) indices[key] = int(indices.size());
    std::vector<std::array<Keys,5>> masks(mem.width);
    for (int b=0; b<mem.width; b++) for (int s=0; s<5; s++)
        for (auto key : plan.candidates[b][s].leaves) masks[b][s].set(indices.at(key));
    Keys reference_leaves;
    for (auto &choices : masks) reference_leaves |= choices[4];
    plan.reference_cost = mem.width + int(reference_leaves.count());
    int best_cost = 1000000;
    for (int shift = 0; shift < mem.width; shift++) for (int reverse = 0; reverse < 2; reverse++)
    for (int tie = 0; tie < 5; tie++) {
        std::vector<int> selected(mem.width, 0), order;
        Keys existing;
        for (int i = 0; i < mem.width; i++) order.push_back((i+shift)%mem.width);
        if (reverse) std::reverse(order.begin(), order.end());
        for (int bit : order) {
            std::pair<int,int> best(1000000,1000000); int which = 0;
            for (int s = 0; s < 5; s++) {
                auto rank = std::make_pair(int((masks[bit][s] & ~existing).count()), (s-tie+5)%5);
                if (rank < best) { best = rank; which = s; }
            }
            selected[bit] = which;
            existing |= masks[bit][which];
        }
        for (int pass = 0; pass < 3; pass++) for (int bit = 0; bit < mem.width; bit++) {
            Keys other;
            for (int b = 0; b < mem.width; b++) if (b != bit)
                other |= masks[b][selected[b]];
            std::pair<int,int> best(1000000,1000000); int which = 0;
            for (int s = 0; s < 5; s++) {
                auto rank = std::make_pair(int((masks[bit][s] & ~other).count()),s);
                if (rank < best) { best = rank; which = s; }
            }
            selected[bit] = which;
        }
        existing.reset();
        for (int bit = 0; bit < mem.width; bit++) existing |= masks[bit][selected[bit]];
        int cost = mem.width + int(existing.count());
        if (cost < best_cost || (cost == best_cost && selected < plan.selected)) {
            best_cost = cost; plan.selected = selected;
        }
    }
    for (int bit=0; bit<mem.width; bit++) { auto &keys = plan.candidates[bit][plan.selected[bit]].leaves;
        plan.leaves.insert(keys.begin(),keys.end()); }
    plan.candidate_cost = best_cost;
    return plan.candidate_cost < plan.reference_cost;
}

inline int evaluate(const Ref &ref, int address)
{
    if (ref.kind == 0) return ref.value;
    if (ref.kind == 1) return ((address >> ref.value)&1) ^ ref.invert;
    auto vars = variables(ref.key.first); int a = 0;
    for (int i = 0; i < int(vars.size()); i++) a |= ((address>>vars[i])&1)<<i;
    return int((ref.key.second>>a)&1u) ^ ref.invert;
}
inline bool verify(const Plan &plan)
{
    for (int bit = 0; bit < int(plan.truth.size()); bit++) for (int a = 0; a < 32; a++) {
        auto &c = plan.candidates[bit][plan.selected[bit]];
        int actual = evaluate(((a>>c.split)&1) ? c.b : c.a, a);
        if (actual != int((plan.truth[bit]>>a)&1u)) return false;
    }
    return true;
}
inline void emit(Mem &mem, const Plan &plan)
{
    Module *module = mem.module;
    std::map<Key,SigBit> nodes;
    std::map<SigBit,SigBit> inverted;
    for (auto key : plan.leaves) {
        SigSpec input;
        for (int bit : variables(key.first)) input.append(plan.address[bit]);
        SigBit output(module->addWire(NEW_ID));
        module->addLut(NEW_ID,input,output,Const(int(key.second),1<<GetSize(input)));
        nodes[key] = output;
    }
    auto value = [&](const Ref &ref) -> SigBit {
        if (ref.kind == 0) return SigBit(ref.value ? State::S1 : State::S0);
        SigBit bit = ref.kind == 1 ? plan.address[ref.value] : nodes.at(ref.key);
        if (!ref.invert) return bit;
        if (!inverted.count(bit)) {
            SigBit out(module->addWire(NEW_ID));
            module->addNot(NEW_ID,bit,out); inverted[bit] = out;
        }
        return inverted.at(bit);
    };
    for (int bit = 0; bit < int(plan.truth.size()); bit++) {
        const Choice &c = plan.candidates[bit][plan.selected[bit]];
        SigBit a = value(c.a), b = value(c.b);
        if (a == b) module->connect(plan.output[bit],a);
        else module->addMux(NEW_ID,a,b,plan.address[c.split],plan.output[bit]);
    }
                                                                           
                                                                          
    mem.remove();
}
}                                   

#include "pmux_rom_residual.h"

inline int pmux_share_small_roms(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_small_rom_detail;
    int changed = 0, examined = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(*module)) continue;
        SigMap sigmap(module);
        pool<SigBit> protected_bits, input_bits;
        for (auto wire : module->wires()) {
            const bool protect = protected_object(*wire);
            if (!protect && !wire->port_input) continue;
            for (auto bit : sigmap(wire)) {
                if (protect) protected_bits.insert(bit);
                if (wire->port_input) input_bits.insert(bit);
            }
        }
        for (auto mem : Mem::get_selected_memories(module)) {
            if (changed >= 8) return changed;
            if (mem.size == 32 && mem.width >= 2 && mem.width <= 16 && ++examined > 8) return changed;
            Plan plan;
            if (!prepare(mem,sigmap,protected_bits,input_bits,plan) || !verify(plan)) continue;
            pmux_rom_residual_detail::ResidualPlan residual;
            if (pmux_rom_residual_detail::prepare_residual(plan,residual)) {
                log("PMUX_SMALL_ROM module=%s address_bits=5 outputs=%d representation=residual groups=%d old_aig_cost=%d new_aig_cost=%d\n",
                    log_id(module),mem.width,int(residual.groups.size()),residual.old_cost,residual.new_cost);
                pmux_rom_residual_detail::emit_residual(mem,plan,residual);
            } else {
                log("PMUX_SMALL_ROM module=%s address_bits=5 outputs=%d cofactor_luts=%d reference_cost=%d candidate_cost=%d\n",
                    log_id(module),mem.width,int(plan.leaves.size()),plan.reference_cost,plan.candidate_cost);
                emit(mem,plan);
            }
            changed++;
        }
    }
    return changed;
}
#endif

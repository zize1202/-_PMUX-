// SPDX-License-Identifier: MIT
// Preserve the declared FAQ zero-initial-state interpretation before a PMUX
// rewrite exposes constant FF data. This is NOT arbitrary-initial-state RTL
// equivalence. Applicable only to the frozen synth_intel mapping:
//   dfflegalize -cell $_DFFE_PN0P_ 01; techmap intel/common/ff_map.v
// Pure positive-edge $dff/$dffe cells need no Q inversion for that legalizer.
// Explicit init 0/1 is preserved; downstream passes must preserve it as well.
// The final baseline-to-optimized sequential proof remains mandatory.
#ifndef PMUX_PRESERVE_ZERO_STATE_20261101_H
#define PMUX_PRESERVE_ZERO_STATE_20261101_H

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/ffinit.h"
#include <algorithm>
#include <vector>

namespace pmux_zero_state_detail {
using namespace Yosys;
using Bit = RTLIL::SigBit;
using Sig = RTLIL::SigSpec;
using Cell = RTLIL::Cell;
using Module = RTLIL::Module;

inline bool protected_object(const RTLIL::AttrObject *object)
{
    return object->get_bool_attribute(ID::keep) ||
           object->get_bool_attribute(ID(dont_touch)) ||
           object->get_bool_attribute(ID(preserve));
}

struct Endpoint {
    Cell *cell;
    RTLIL::IdString port;
    int offset;
};

inline bool exact_parameter(Cell *cell, RTLIL::IdString name, int value)
{
    auto it = cell->parameters.find(name);
    return it != cell->parameters.end() && it->second.is_fully_def() &&
           it->second.as_int() == value;
}

inline bool supported_ff(Module *module, Cell *cell, SigMap &sigmap)
{
                                                                               
    const bool enable = cell->type == ID($dffe);
    if ((!enable && cell->type != ID($dff)) || protected_object(cell) ||
        !module->selected(cell))
        return false;
    if (GetSize(cell->connections()) != (enable ? 4 : 3) ||
        GetSize(cell->parameters) != (enable ? 3 : 2) ||
        !cell->hasPort(ID::D) || !cell->hasPort(ID::Q) ||
        !cell->hasPort(ID::CLK) || (enable && !cell->hasPort(ID::EN)))
        return false;
    const int width = GetSize(cell->getPort(ID::D));
    if (width <= 0 || GetSize(cell->getPort(ID::Q)) != width ||
        !exact_parameter(cell, ID::WIDTH, width) ||
        !exact_parameter(cell, ID::CLK_POLARITY, 1))
        return false;
    const Sig clock = sigmap(cell->getPort(ID::CLK));
    if (GetSize(clock) != 1 || !clock[0].wire)
        return false;
    if (enable) {
        if (!exact_parameter(cell, ID::EN_POLARITY, 0) &&
            !exact_parameter(cell, ID::EN_POLARITY, 1))
            return false;
        const Sig en = sigmap(cell->getPort(ID::EN));
        if (GetSize(en) != 1 || (!en[0].wire && en[0] != Bit(State::S0) && en[0] != Bit(State::S1)))
            return false;
    }
    return true;
}
}                                    

                                                                                
                                                                             
                                                                                  
inline bool pmux_preserve_zero_state_for(Yosys::RTLIL::Module *module,
                                         Yosys::RTLIL::Cell *pmux)
{
    using namespace Yosys;
    using namespace pmux_zero_state_detail;
    if (!module || !pmux || !module->design || module->cell(pmux->name) != pmux ||
        pmux->type != ID($pmux) || !pmux->hasPort(ID::Y) ||
        module->get_blackbox_attribute() || protected_object(module) ||
        protected_object(pmux) || !module->selected(pmux) ||
        module->has_processes() || module->has_memories())
        return false;

    SigMap sigmap(module);
    CellTypes types(module->design);
    pool<Bit> input_bits, output_bits, protected_bits, explicit_unknown;
    dict<Bit, State> checked_init;
    dict<Bit, std::vector<Endpoint>> drivers, users;

                                                                                
                                                                             
    for (auto wire : module->wires()) {
        const Sig bits = sigmap(wire);
        for (auto bit : bits) {
            if (wire->port_input) input_bits.insert(bit);
            if (wire->port_output) output_bits.insert(bit);
            if (protected_object(wire)) protected_bits.insert(bit);
        }
        auto attr = wire->attributes.find(ID::init);
        if (attr == wire->attributes.end()) continue;
        const int n = std::min(GetSize(bits), GetSize(attr->second));
        for (int i = 0; i < n; ++i) {
            const Bit bit = bits[i];
            const State value = attr->second[i];
            if (value != State::S0 && value != State::S1) {
                explicit_unknown.insert(bit);
                if (bit.wire) continue;                                 
            }
            auto previous = checked_init.find(bit);
            if (previous != checked_init.end() && previous->second != value)
                return false;
            checked_init[bit] = value;
        }
    }

                                                                               
                                                                                
    for (auto cell : module->cells()) {
        if (!types.cell_known(cell->type)) return false;
        for (const auto &port : cell->connections()) {
            const bool is_in = types.cell_input(cell->type, port.first);
            const bool is_out = types.cell_output(cell->type, port.first);
            if (!is_in && !is_out) return false;
            const Sig bits = sigmap(port.second);
            for (int i = 0; i < GetSize(bits); ++i) {
                if (!bits[i].wire) continue;
                const Endpoint endpoint{cell, port.first, i};
                if (is_in) users[bits[i]].push_back(endpoint);
                if (is_out) drivers[bits[i]].push_back(endpoint);
            }
        }
    }

    const Sig outputs = sigmap(pmux->getPort(ID::Y));
    if (outputs.empty()) return false;
    pool<Bit> seen_outputs, seen_q;
    pool<Cell *> checked_ffs;
    std::vector<Bit> add_zero;
    int preserved = 0;
    for (int i = 0; i < GetSize(outputs); ++i) {
        const Bit output = outputs[i];
        if (!output.wire || seen_outputs.count(output) || input_bits.count(output) ||
            output_bits.count(output) || protected_bits.count(output))
            return false;
        seen_outputs.insert(output);
        const auto driver = drivers.find(output);
        if (driver == drivers.end() || driver->second.size() != 1 ||
            driver->second[0].cell != pmux || driver->second[0].port != ID::Y ||
            driver->second[0].offset != i)
            return false;
        const auto consumers = users.find(output);
        if (consumers == users.end() || consumers->second.empty()) return false;
        for (const Endpoint &use : consumers->second) {
            Cell *ff = use.cell;
            if (use.port != ID::D) return false;
            if (!checked_ffs.count(ff)) {
                if (!supported_ff(module, ff, sigmap)) return false;
                checked_ffs.insert(ff);
            }
            const Bit original_q = ff->getPort(ID::Q)[use.offset];
            const Bit q = sigmap(original_q);
                                                                            
            if (!original_q.wire || !q.wire || input_bits.count(q) ||
                protected_bits.count(q) || explicit_unknown.count(q))
                return false;
            const auto qdriver = drivers.find(q);
            if (qdriver == drivers.end() || qdriver->second.size() != 1 ||
                qdriver->second[0].cell != ff || qdriver->second[0].port != ID::Q ||
                qdriver->second[0].offset != use.offset)
                return false;
            if (seen_q.count(q)) continue;
            seen_q.insert(q);
            const auto init = checked_init.find(q);
            if (init != checked_init.end()) {
                if (init->second != State::S0 && init->second != State::S1) return false;
                ++preserved;
                continue;
            }
                                                                           
                                                                           
            auto own_attr = original_q.wire->attributes.find(ID::init);
            if (own_attr != original_q.wire->attributes.end() &&
                original_q.offset >= GetSize(own_attr->second))
                return false;
            add_zero.push_back(original_q);
        }
    }

                                                                         
    FfInitVals initvals(&sigmap, module);
    for (auto bit : add_zero)
        if (initvals(bit) != State::Sx) return false;
                                                                                
                                                                            
                                                                         
                                                                                 
                                                                                
    std::vector<Bit> carriers;
    for (auto bit : add_zero) {
        auto carrier = module->addWire(NEW_ID, 1);
        module->connect(Sig(carrier), Sig(bit));
        carriers.emplace_back(carrier, 0);
    }
    SigMap committed_sigmap(module);
    FfInitVals committed_init(&committed_sigmap, module);
    for (auto carrier : carriers)
        committed_init.set_init(carrier, State::S0);
    log("PMUX_ZERO_INIT_PRESERVE module=%s pmux=%s state_bits=%d added_zero_bits=%d preserved_explicit_bits=%d model=faq_zero_initial_state\n",
        log_id(module), log_id(pmux), int(seen_q.size()), int(add_zero.size()), preserved);
    return true;
}

#endif

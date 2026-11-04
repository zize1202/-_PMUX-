// SPDX-License-Identifier: MIT
// Replace selector bits in a PMUX B arm only when its proved equality codes
// force the bit. No A/S, state, clock, reset, enable, X/Z, or tree changes.
// Include after pmux_resource_share.h.
#ifndef PMUX_BRANCH_CONSTANTS_H
#define PMUX_BRANCH_CONSTANTS_H
#include <map>
#include <set>
#include <vector>
inline int pmux_propagate_branch_constants(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_resource_detail;
    int changed_cells = 0, changed_bits = 0, inspected = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        Context ctx(module);
        if (!ctx.complete) continue;
        for (auto cell : module->selected_cells()) {
            if (cell->type != ID($pmux) || protected_object(cell)) continue;
            if (++inspected > 256 || changed_bits >= 4096) break;
            Sig s = ctx.sigmap(cell->getPort(ID::S));
            Sig b = cell->getPort(ID::B), y = ctx.sigmap(cell->getPort(ID::Y));
            int width = GetSize(cell->getPort(ID::A));
            if (width < 1 || width > 1024 || GetSize(s) < 1 || GetSize(s) > 64 ||
                GetSize(b) != width*GetSize(s) || ctx.protected_signal(y)) continue;
            SelectionProof proof;
            if (!prove_selection(ctx,cell,s,proof)) continue;
            int budget = 512; bool valid = true;
            std::vector<DecodeSet> arms;
            for (auto select : s) {
                DecodeSet arm;
                if (!decode_condition(ctx,select,arm,0,budget)) { valid = false; break; }
                // Reject repeated/constant selector bits rather than relying
                // on a partially inconsistent code representation.
                std::set<Bit> unique;
                for (auto bit : arm.selector)
                    if (!bit.wire || !unique.insert(bit).second) valid = false;
                arms.push_back(arm);
            }
            if (!valid) continue;
            int replacements = 0;
            for (int arm_index = 0; arm_index < GetSize(s); arm_index++) {
                if (changed_bits + replacements >= 4096) break;
                const auto &arm = arms[arm_index];
                if (arm.codes.empty() || !proof.reachable[arm_index]) continue;
                std::map<Bit,int> positions;
                for (int i = 0; i < GetSize(arm.selector); i++) positions[arm.selector[i]] = i;
                for (int bit = 0; bit < width; bit++) {
                    if (changed_bits + replacements >= 4096) break;
                    int index = arm_index*width+bit;
                    Bit data = ctx.sigmap(b[index]);
                    if (!data.wire || ctx.protected_bits.count(data) || !positions.count(data)) continue;
                    int position = positions.at(data);
                    unsigned first = (*arm.codes.begin() >> position)&1u;
                    bool uniform = true;
                    for (auto code : arm.codes) if (((code >> position)&1u) != first) { uniform = false; break; }
                    if (!uniform) continue;
                    b[index] = first ? State::S1 : State::S0;
                    replacements++;
                }
            }
            if (!replacements) continue;
            cell->setPort(ID::B,b);
            changed_cells++; changed_bits += replacements;
            log("PMUX_BRANCH_CONSTANTS module=%s cell=%s replaced_B_bits=%d\n",
                log_id(module),log_id(cell),replacements);
        }
        if (inspected > 256 || changed_bits >= 4096) break;
    }
    log("PMUX_BRANCH_CONSTANTS changed_cells=%d replaced_B_bits=%d\n",changed_cells,changed_bits);
    return changed_cells;
}
#endif

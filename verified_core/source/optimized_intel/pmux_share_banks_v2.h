// SPDX-License-Identifier: MIT
// Alternative bank construction: fuse exceptions into rows, then share two
// small binary banks and a final swap. Keep pmux_share_banks.h as unchanged v1.
#ifndef PMUX_SHARE_BANKS_V2_20261101_H
#define PMUX_SHARE_BANKS_V2_20261101_H
#include "pmux_share_banks.h"

namespace pmux_bank_v2_detail {
using namespace Yosys;
using pmux_resource_detail::Sig;
using pmux_resource_detail::Bit;
using pmux_resource_detail::Cell;
using pmux_resource_detail::Module;
using pmux_resource_detail::Context;
using pmux_resource_detail::protected_object;
using pmux_bank_detail::Table;
using pmux_bank_detail::insert_bit;

struct Plan {
    int bit = -1, patch_bits = 0, patched_rows = 0;
    int old_estimate = 0, new_estimate = 0, saving = 0;
};

inline bool make_plan(const Table &first, const Table &second, Plan &best)
{
    if (first.width != second.width || first.selector != second.selector || first.rows.size() != second.rows.size()) return false;
    pool<Bit> outputs;
    for (auto bit : first.output) outputs.insert(bit);
    for (auto bit : second.output) if (outputs.count(bit)) return false;
    const int w = first.width, n = int(first.rows.size());
    // At most eight words per shared bank: a binary bank has <=7 mux/LUT
    // positions per output bit. Generated cells carry pmux_bank_local so a
    // caller may exclude this planned structure from priority-chain rewriting.
    if (n > 16) return false;
    for (int bit = 0; bit < GetSize(first.selector); bit++) {
        Plan plan; plan.bit = bit;
        for (int group = 0; group < n/2; group++) {
            unsigned c0 = insert_bit(unsigned(group), bit, 0), c1 = insert_bit(unsigned(group), bit, 1);
            for (int bank = 0; bank < 2; bank++) {
                const Sig &lo = bank ? second.rows[c0] : first.rows[c0];
                const Sig &hi = bank ? first.rows[c1] : second.rows[c1];
                int changed = 0;
                for (int j = 0; j < w; j++) if (lo[j] != hi[j]) changed++;
                if (changed) { plan.patched_rows++; plan.patch_bits += changed; }
            }
        }
        if (plan.patch_bits > 2*w || plan.patched_rows > 4) continue;
        plan.old_estimate = 2*w*pmux_bank_detail::bank_lut4_estimate(n);
        // Every two-input data mux has <=3 support inputs; count all such
        // positions, including row exceptions and the final two-output swap.
        // No original address-decoder removal is credited.
        plan.new_estimate = 2*w*(n/2-1) + 2*w + plan.patch_bits;
        plan.saving = plan.old_estimate - plan.new_estimate;
        if (plan.saving < 2*w || 10*plan.saving < plan.old_estimate) continue;
        if (best.bit < 0 || plan.saving > best.saving) best = plan;
    }
    return best.bit >= 0;
}

struct Builder {
    Module *module;
    std::map<std::pair<Sig, std::vector<Sig>>, Sig> banks;
    explicit Builder(Module *m) : module(m) {}

    Sig mux(const Sig &a, const Sig &b, Bit select)
    {
        Sig result = a, lo, hi;
        std::vector<int> positions;
        for (int i = 0; i < GetSize(a); i++) if (a[i] != b[i]) {
            positions.push_back(i); lo.append(a[i]); hi.append(b[i]);
        }
        if (positions.empty()) return result;
        Sig out(module->addWire(NEW_ID, int(positions.size())));
        auto cell = module->addMux(NEW_ID, lo, hi, select, out);
        cell->set_bool_attribute(ID(pmux_bank_local));
        for (size_t j = 0; j < positions.size(); j++) result.replace(positions[j], Sig(out[int(j)]));
        return result;
    }

    Sig binary_bank(const Sig &selector, const std::vector<Sig> &rows)
    {
        auto key = std::make_pair(selector, rows);
        auto found = banks.find(key);
        if (found != banks.end()) return found->second;
        std::vector<Sig> layer = rows;
        for (int level = 0; layer.size() > 1; level++) {
            std::vector<Sig> next;
            for (size_t i = 0; i < layer.size(); i += 2) next.push_back(mux(layer[i], layer[i+1], selector[level]));
            layer.swap(next);
        }
        banks.emplace(key, layer.front());
        return layer.front();
    }

    void emit(const Table &first, const Table &second, const Plan &plan)
    {
        const Bit phase = first.selector[plan.bit];
        Sig reduced;
        for (int i = 0; i < GetSize(first.selector); i++) if (i != plan.bit) reduced.append(first.selector[i]);
        std::vector<Sig> rows_a, rows_b;
        for (unsigned group = 0; group < first.rows.size()/2; group++) {
            unsigned c0 = insert_bit(group, plan.bit, 0), c1 = insert_bit(group, plan.bit, 1);
            // A(p=0)=F0, A(p=1)=G1; B(p=0)=G0, B(p=1)=F1.
            // Equal rows produce no cell. Exceptions are therefore local to a
            // bank row rather than conditional muxes after the full outputs.
            rows_a.push_back(mux(first.rows[c0], second.rows[c1], phase));
            rows_b.push_back(mux(second.rows[c0], first.rows[c1], phase));
        }
        Sig a = binary_bank(reduced, rows_a), b = binary_bank(reduced, rows_b);
        module->connect(first.output, mux(a, b, phase));
        module->connect(second.output, mux(b, a, phase));
        log("PMUX_BANK_SHARE_V2 module=%s first=%s second=%s address_bits=%d split_bit=%d width=%d patched_rows=%d patch_bits=%d old_estimate=%d new_estimate=%d estimated_saved=%d\n",
            log_id(module), log_id(first.cell), log_id(second.cell), GetSize(first.selector), plan.bit,
            first.width, plan.patched_rows, plan.patch_bits, plan.old_estimate, plan.new_estimate, plan.saving);
    }
};
} // namespace pmux_bank_v2_detail

inline int pmux_share_banks_v2(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using pmux_resource_detail::Context;
    using pmux_resource_detail::Cell;
    using pmux_bank_detail::Table;
    using pmux_bank_v2_detail::Plan;
    int pairs = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || pmux_resource_detail::protected_object(module)) continue;
        std::vector<Cell *> roots;
        for (auto cell : module->selected_cells()) if (cell->type == ID($pmux)) roots.push_back(cell);
        if (roots.size() < 2) continue;
        Context context(module);
        if (!context.complete) continue;
        std::vector<Table> tables;
        for (auto cell : roots) {
            if (tables.size() >= 64) break;
            Table table;
            if (pmux_bank_detail::read_table(context, cell, table)) tables.push_back(table);
        }
        pmux_bank_v2_detail::Builder builder(module);
        pool<Cell *> used;
        int comparisons = 0;
        for (size_t i = 0; i < tables.size(); i++) {
            if (used.count(tables[i].cell)) continue;
            int best_j = -1; Plan best;
            for (size_t j = i+1; j < tables.size(); j++) {
                if (used.count(tables[j].cell) || ++comparisons > 256) continue;
                Plan trial;
                if (pmux_bank_v2_detail::make_plan(tables[i], tables[j], trial) &&
                    (best_j < 0 || trial.saving > best.saving)) { best = trial; best_j = int(j); }
            }
            if (best_j < 0) continue;
            builder.emit(tables[i], tables[size_t(best_j)], best);
            used.insert(tables[i].cell); used.insert(tables[size_t(best_j)].cell); pairs++;
        }
        for (auto cell : used) module->remove(cell);
    }
    return pairs;
}
#endif

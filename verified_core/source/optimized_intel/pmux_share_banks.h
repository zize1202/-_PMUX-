// SPDX-License-Identifier: MIT
// Share two high-fanin decoded PMUX outputs whose data cofactors are swapped.
// Optional sparse differences are restored with exact equality-controlled muxes.
// Banks are explicit OR-of-masked-data networks, never binary data-mux trees.
#ifndef PMUX_SHARE_BANKS_20261101_H
#define PMUX_SHARE_BANKS_20261101_H
#include "pmux_resource_share.h"

namespace pmux_bank_detail {
using namespace Yosys;
using pmux_resource_detail::Sig;
using pmux_resource_detail::Bit;
using pmux_resource_detail::Cell;
using pmux_resource_detail::Module;
using pmux_resource_detail::Context;
using pmux_resource_detail::DecodeSet;
using pmux_resource_detail::protected_object;
using pmux_resource_detail::known_literals;

inline bool boundary_data(Context &ctx, const Sig &value)
{
    static const std::set<RTLIL::IdString> state_types = {
        ID($dff), ID($dffe), ID($adff), ID($adffe), ID($sdff), ID($sdffe),
        ID($sdffce), ID($dffsr), ID($dffsre), ID($aldff), ID($aldffe),
        ID($dlatch), ID($adlatch), ID($dlatchsr), ID($ff)
    };
    for (auto bit : value) {
        if (!bit.wire || ctx.multiple.count(bit) || ctx.protected_bits.count(bit)) return false;
        if (ctx.input_bits.count(bit)) {
            if (ctx.driver.count(bit)) return false;
            continue;
        }
        if (!ctx.driver.count(bit) || !state_types.count(ctx.driver.at(bit)->type)) return false;
    }
    return true;
}

struct Table {
    Cell *cell = nullptr;
    Sig selector, output;
    int width = 0;
    std::vector<Sig> rows;
    std::map<unsigned, Bit> singleton_controls;
};

inline bool read_table(Context &ctx, Cell *cell, Table &table)
{
    if (cell->type != ID($pmux) || protected_object(cell)) return false;
    int w = cell->getParam(ID::WIDTH).as_int(), n = cell->getParam(ID::S_WIDTH).as_int();
    if (w < 4 || w > 256 || n < 7 || n > 32) return false;
    Sig a = ctx.sigmap(cell->getPort(ID::A)), b = ctx.sigmap(cell->getPort(ID::B));
    Sig s = ctx.sigmap(cell->getPort(ID::S)), y = ctx.sigmap(cell->getPort(ID::Y));
    if (GetSize(a) != w || GetSize(b) != w*n || GetSize(s) != n || GetSize(y) != w ||
        !known_literals(s) || ctx.protected_signal(y)) return false;
    pool<Bit> ybits;
    for (auto bit : y) {
        if (!bit.wire || ctx.multiple.count(bit) || ctx.input_bits.count(bit) || ybits.count(bit)) return false;
        ybits.insert(bit);
    }
    Sig selector;
    std::map<unsigned, Sig> rows;
    int budget = 512;
    for (int i = 0; i < n; i++) {
        DecodeSet arm;
        if (!pmux_resource_detail::decode_condition(ctx, s[i], arm, 0, budget)) return false;
        if (arm.codes.empty()) continue;
        if (arm.enable != Bit(State::S1)) return false;
        if (selector.empty()) selector = arm.selector;
        if (selector != arm.selector || GetSize(selector) < 3 || GetSize(selector) > 5) return false;
        Sig value = b.extract(i*w, w);
        // Only primary inputs or current state leaves: no root dependency can
        // become a new combinational feedback loop when two outputs are shared.
        if (!known_literals(value) || !boundary_data(ctx, value)) return false;
        for (auto code : arm.codes) if (!rows.emplace(code, value).second) return false;
        if (arm.codes.size() == 1) table.singleton_controls[*arm.codes.begin()] = s[i];
    }
    const unsigned count = unsigned(1) << GetSize(selector);
    if (selector.empty() || rows.size() != count) return false;
    table.cell = cell; table.selector = selector; table.output = y; table.width = w;
    for (unsigned code = 0; code < count; code++) {
        auto found = rows.find(code);
        if (found == rows.end()) return false;
        table.rows.push_back(found->second);
    }
    // Cost admission assumes high data diversity. Reject columns that standard
    // reduction can already turn into a small constant/repeated-data selector.
    for (int j = 0; j < w; j++) {
        std::set<Bit> values;
        for (auto &row : table.rows) values.insert(row[j]);
        if (values.size() != count) return false;
    }
    return true;
}

inline unsigned insert_bit(unsigned group, int position, int value)
{
    const unsigned lowmask = (unsigned(1) << position) - 1;
    return (group & lowmask) | ((group >> position) << (position + 1)) | (unsigned(value) << position);
}

struct Patch {
    int output = 0;
    unsigned code = 0;
    Sig expected;
    std::vector<int> bits;
};
struct Plan {
    int bit = -1, exact_phase = 1;
    int old_estimate = 0, new_estimate = 0, saving = 0, patch_bits = 0;
    std::vector<Sig> bank_a, bank_b;
    std::vector<Patch> patches;
};

inline int bank_lut4_estimate(int leaves)
{
    if (leaves <= 1) return 0;
    const int pair_terms = (leaves + 1) / 2;
    return pair_terms + (pair_terms - 1 + 2) / 3;
}

inline bool make_plan(const Table &first, const Table &second, Plan &best)
{
    if (first.width != second.width || first.selector != second.selector || first.rows.size() != second.rows.size()) return false;
    pool<Bit> outputs;
    for (auto bit : first.output) outputs.insert(bit);
    for (auto bit : second.output) if (outputs.count(bit)) return false;
    const int w = first.width, n = int(first.rows.size());
    for (int bit = 0; bit < GetSize(first.selector); bit++) {
        // Phase 1 is tried first; ties have a deterministic, structural policy.
        for (int phase : {1, 0}) {
            Plan plan; plan.bit = bit; plan.exact_phase = phase;
            for (int group = 0; group < n/2; group++) {
                unsigned code0 = insert_bit(unsigned(group), bit, 0), code1 = insert_bit(unsigned(group), bit, 1);
                Sig a = phase ? second.rows[code1] : first.rows[code0];
                Sig b = phase ? first.rows[code1] : second.rows[code0];
                plan.bank_a.push_back(a); plan.bank_b.push_back(b);
                for (int output = 0; output < 2; output++) {
                    unsigned patch_code = phase ? code0 : code1;
                    Sig actual = phase ? (output ? b : a) : (output ? a : b);
                    const Sig &expected = output ? second.rows[patch_code] : first.rows[patch_code];
                    Patch patch; patch.output = output; patch.code = patch_code; patch.expected = expected;
                    for (int j = 0; j < w; j++) if (actual[j] != expected[j]) patch.bits.push_back(j);
                    if (!patch.bits.empty()) {
                        plan.patch_bits += int(patch.bits.size());
                        plan.patches.push_back(patch);
                    }
                }
            }
            // Exact exchange or at most two word-sized sparse exceptions.
            if (plan.patch_bits > 2*w || plan.patches.size() > 4) continue;
            plan.old_estimate = 2*w*bank_lut4_estimate(n);
            // Charge every new grouped decoder plus both final output muxes,
            // and every actually patched output bit. No savings are claimed
            // for original decoders or state logic that may remain live.
            plan.new_estimate = 2*w*bank_lut4_estimate(n/2) + 2*w + plan.patch_bits + n/2;
            plan.saving = plan.old_estimate - plan.new_estimate;
            if (plan.saving < 2*w || 5*plan.saving < plan.old_estimate) continue;
            if (best.bit < 0 || plan.saving > best.saving) best = plan;
        }
    }
    return best.bit >= 0;
}

struct Builder {
    Module *module;
    std::map<std::pair<Sig, unsigned>, Bit> controls;
    std::map<std::pair<Sig, std::vector<Sig>>, Sig> banks;
    explicit Builder(Module *m) : module(m) {}

    void seed_controls(const Table &table)
    {
        for (auto &entry : table.singleton_controls) controls.emplace(std::make_pair(table.selector, entry.first), entry.second);
    }

    Bit equality(const Sig &selector, unsigned code)
    {
        auto key = std::make_pair(selector, code);
        auto found = controls.find(key);
        if (found != controls.end()) return found->second;
        Bit result(module->addWire(NEW_ID));
        if (code == 0) module->addLogicNot(NEW_ID, selector, result);
        else module->addEq(NEW_ID, selector, Sig(RTLIL::Const(code, GetSize(selector))), result);
        controls.emplace(key, result);
        return result;
    }

    Sig make_bank(const Sig &selector, const std::vector<Sig> &rows)
    {
        auto key = std::make_pair(selector, rows);
        auto found = banks.find(key);
        if (found != banks.end()) return found->second;
        int w = GetSize(rows.front());
        std::vector<Sig> layer;
        for (unsigned i = 0; i < rows.size(); i++) {
            Bit cond = equality(selector, i);
            Sig mask;
            for (int j = 0; j < w; j++) mask.append(cond);
            Sig term(module->addWire(NEW_ID, w));
            module->addAnd(NEW_ID, rows[i], mask, term);
            layer.push_back(term);
        }
        while (layer.size() > 1) {
            std::vector<Sig> next;
            for (size_t i = 0; i < layer.size(); i += 2) {
                if (i + 1 == layer.size()) next.push_back(layer[i]);
                else {
                    Sig value(module->addWire(NEW_ID, w));
                    module->addOr(NEW_ID, layer[i], layer[i+1], value);
                    next.push_back(value);
                }
            }
            layer.swap(next);
        }
        banks.emplace(key, layer.front());
        return layer.front();
    }

    void emit(const Table &first, const Table &second, const Plan &plan)
    {
        seed_controls(first); seed_controls(second);
        Sig reduced_selector;
        for (int i = 0; i < GetSize(first.selector); i++) if (i != plan.bit) reduced_selector.append(first.selector[i]);
        Sig a = make_bank(reduced_selector, plan.bank_a), b = make_bank(reduced_selector, plan.bank_b);
        Sig outputs[2] = {Sig(module->addWire(NEW_ID, first.width)), Sig(module->addWire(NEW_ID, first.width))};
        module->addMux(NEW_ID, a, b, first.selector[plan.bit], outputs[0]);
        module->addMux(NEW_ID, b, a, first.selector[plan.bit], outputs[1]);
        for (auto &patch : plan.patches) {
            Sig old_bits, new_bits;
            for (int index : patch.bits) { old_bits.append(outputs[patch.output][index]); new_bits.append(patch.expected[index]); }
            Sig patched(module->addWire(NEW_ID, int(patch.bits.size())));
            module->addMux(NEW_ID, old_bits, new_bits, equality(first.selector, patch.code), patched);
            for (size_t j = 0; j < patch.bits.size(); j++) outputs[patch.output].replace(patch.bits[j], Sig(patched[int(j)]));
        }
        module->connect(first.output, outputs[0]);
        module->connect(second.output, outputs[1]);
        log("PMUX_BANK_SHARE module=%s first=%s second=%s address_bits=%d split_bit=%d width=%d exact_phase=%d patches=%d patch_bits=%d old_estimate=%d new_estimate=%d estimated_saved=%d\n",
            log_id(module), log_id(first.cell), log_id(second.cell), GetSize(first.selector), plan.bit,
            first.width, plan.exact_phase, int(plan.patches.size()), plan.patch_bits,
            plan.old_estimate, plan.new_estimate, plan.saving);
    }
};
} // namespace pmux_bank_detail

inline int pmux_share_banks(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_bank_detail;
    int pairs = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        std::vector<Cell *> roots;
        for (auto cell : module->selected_cells()) if (cell->type == ID($pmux)) roots.push_back(cell);
        if (roots.size() < 2) continue;
        Context context(module);
        if (!context.complete) continue;
        std::vector<Table> tables;
        for (auto cell : roots) {
            if (tables.size() >= 64) break;
            Table table;
            if (read_table(context, cell, table)) tables.push_back(table);
        }
        Builder builder(module);
        pool<Cell *> used;
        int comparisons = 0;
        for (size_t i = 0; i < tables.size(); i++) {
            if (used.count(tables[i].cell)) continue;
            int best_j = -1; Plan best;
            for (size_t j = i + 1; j < tables.size(); j++) {
                if (used.count(tables[j].cell) || ++comparisons > 256) continue;
                Plan trial;
                if (make_plan(tables[i], tables[j], trial) && (best_j < 0 || trial.saving > best.saving)) { best = trial; best_j = int(j); }
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

// SPDX-License-Identifier: MIT
// Declare a source-level initial-state contract, not an unconstrained
// sequential-equivalence transformation.  Must run immediately after proc,
// before sequential optimization can remove an uninitialized constant-D FF.
#ifndef PMUX_INIT_ZERO_20261101_H
#define PMUX_INIT_ZERO_20261101_H

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"
#include "kernel/mem.h"
#include <cstdint>
#include <climits>

struct PmuxSourceInitBudget {
    int64_t memory_bits = 0, existing_init_bits = 0, added_bits = 0;
    int memories = 0, roms = 0, added_segments = 0;
};

inline void pmux_declare_ram_zero_initial_state(Yosys::RTLIL::Module *module,
                                               Yosys::SigMap &sigmap,
                                               PmuxSourceInitBudget &budget)
{
    using namespace Yosys;
    constexpr int64_t per_ram_limit = int64_t(16) * 1024 * 1024;
    constexpr int64_t design_limit = int64_t(64) * 1024 * 1024;
    constexpr int segment_limit = 65536;
    for (auto &mem : Mem::get_selected_memories(module)) {
                                                                            
                                                                            
        for (const auto &port : mem.rd_ports)
            if (!port.removed && (port.clk_enable || port.wide_log2 != 0))
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: pre-merged synchronous/wide read port; run immediately after proc.\n",
                              log_id(module), log_id(mem.memid));

        bool writable = false, have_clock = false, polarity = false;
        SigSpec clock;
        for (const auto &port : mem.wr_ports) {
            if (port.removed) continue;
            SigSpec en = sigmap(port.en);
            if (en.is_fully_const() && en.is_fully_def() && !en.as_bool())
                continue;
            if (en.is_fully_const() && !en.is_fully_def())
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: unknown constant write enable.\n",
                              log_id(module), log_id(mem.memid));
            writable = true;
            SigSpec clk = sigmap(port.clk);
            if (!port.clk_enable || port.wide_log2 != 0 || GetSize(clk) != 1 || !clk[0].wire)
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: active asynchronous/wide write or non-signal clock.\n",
                              log_id(module), log_id(mem.memid));
            if (!have_clock) {
                have_clock = true;
                clock = clk;
                polarity = port.clk_polarity;
            } else if (clock != clk || polarity != port.clk_polarity) {
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: write ports have different clock domains.\n",
                              log_id(module), log_id(mem.memid));
            }
        }
        if (!writable) {
            ++budget.roms;
                                                                              
                                                                             
            log("OPT_PMUX_INIT_ZERO_ROM: %s.%s unchanged (no active runtime writes).\n",
                log_id(module), log_id(mem.memid));
            continue;
        }

        const int64_t bits = int64_t(mem.width) * int64_t(mem.size);
        const int64_t last_address = int64_t(mem.start_offset) + int64_t(mem.size) - 1;
        if (mem.width <= 0 || mem.size <= 0 || bits > per_ram_limit ||
            last_address < INT_MIN || last_address > INT_MAX ||
            budget.memory_bits + bits > design_limit)
            log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: RAM size/address budget exceeded (16M bits/RAM, 64M bits/design).\n",
                          log_id(module), log_id(mem.memid));
        int64_t init_bits = 0;
        for (const auto &init : mem.inits) {
            if (init.removed) continue;
            if (!init.addr.is_fully_def() || GetSize(init.addr) > 32 ||
                GetSize(init.en) != mem.width || GetSize(init.data) % mem.width != 0)
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: malformed initialization address/width.\n",
                              log_id(module), log_id(mem.memid));
                                                                          
                                                                           
            const int64_t words = int64_t(init.addr.as_int()) - int64_t(mem.start_offset);
            const int64_t offset = words * int64_t(mem.width);
            if (words < INT_MIN || words > INT_MAX || offset < INT_MIN ||
                offset > INT_MAX || offset + GetSize(init.data) > INT_MAX)
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: initialization offset overflows native memory indexing.\n",
                              log_id(module), log_id(mem.memid));
            init_bits += GetSize(init.data);
            if (budget.existing_init_bits + init_bits > design_limit)
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: existing initialization scan exceeds 64M bits/design.\n",
                              log_id(module), log_id(mem.memid));
        }
        budget.memory_bits += bits;
        budget.existing_init_bits += init_bits;
        ++budget.memories;
        Const final_init = mem.get_init_data();
        std::vector<MemInit> additions;
        Const run_mask;
        int run_start = 0;
        int64_t unknown_bits = 0;
        auto finish_run = [&](int end) {
            if (end == run_start || !run_mask.as_bool()) return;
            if (budget.added_segments + GetSize(additions) >= segment_limit)
                log_cmd_error("OPT_PMUX_INIT_ZERO_UNSUPPORTED %s.%s: more than 65536 additional initialization segments/design.\n",
                              log_id(module), log_id(mem.memid));
            MemInit init;
            init.addr = Const(mem.start_offset + run_start, 32);
            init.data = Const(State::S0, mem.width * (end - run_start));
            init.en = run_mask;
            additions.push_back(std::move(init));
        };
        for (int word = 0; word < mem.size; ++word) {
            Const mask(State::S0, mem.width);
            for (int bit = 0; bit < mem.width; ++bit) {
                State value = final_init[word * mem.width + bit];
                if (value != State::S0 && value != State::S1) {
                    mask.set(bit, State::S1);
                    ++unknown_bits;
                }
            }
            if (word == 0) {
                run_mask = mask;
            } else if (mask != run_mask) {
                finish_run(word);
                run_start = word;
                run_mask = mask;
            }
        }
        finish_run(mem.size);
        const int segments = GetSize(additions);
        if (segments) {
                                                                           
                                                                               
                                                                                
            for (auto &init : additions)
                mem.inits.push_back(std::move(init));
            mem.emit();
        }
        budget.added_bits += unknown_bits;
        budget.added_segments += segments;
        log("OPT_PMUX_INIT_ZERO_RAM: %s.%s width=%d size=%d offset=%d added_zero_bits=%lld preserved_explicit_01_bits=%lld segments=%d\n",
            log_id(module), log_id(mem.memid), mem.width, mem.size, mem.start_offset,
            static_cast<long long>(unknown_bits), static_cast<long long>(bits - unknown_bits), segments);
    }
}

inline void pmux_declare_zero_initial_state(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    int added = 0, preserved = 0, cells = 0, skipped_latches = 0;
    PmuxSourceInitBudget memory_budget;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute())
            continue;
        if (module->has_processes())
            log_cmd_error("opt_pmux -init-zero requires proc before declaring source FF initialization.\n");
        bool has_ff = false, has_memory = module->has_memories();
        for (auto cell : module->cells()) {
            if (cell->is_builtin_ff()) has_ff = true;
            if (cell->type.in(ID($mem), ID($mem_v2))) has_memory = true;
            if (has_ff && has_memory) break;
        }
                                                                           
                                                                              
                                                                             
                                                                 
        if (!has_ff && !has_memory) continue;
        SigMap sigmap(module);
        FfInitVals initvals(&sigmap, module);
        pool<SigBit> input_bits, seen;
        for (auto wire : module->wires())
            if (wire->port_input)
                for (auto bit : sigmap(wire))
                    input_bits.insert(bit);
        for (auto cell : module->selected_cells()) {
            if (!cell->is_builtin_ff())
                continue;
            FfData ff(&initvals, cell);
            if (!ff.has_clk && !ff.has_gclk) {
                ++skipped_latches;
                continue;
            }
            ++cells;
            for (auto q : ff.sig_q) {
                SigBit bit = sigmap(q);
                if (!q.wire || !bit.wire || input_bits.count(bit))
                    log_cmd_error("opt_pmux -init-zero cannot attach a source FF initial value to a constant or external input: %s.%s Q=%s.\n",
                                  log_id(module), log_id(cell), log_signal(q));
                if (!seen.insert(bit).second)
                    continue;
                State value = initvals(q);
                if (value == State::S0 || value == State::S1) {
                    ++preserved;
                    continue;
                }
                                                                               
                                                                              
                initvals.set_init(q, State::S0);
                ++added;
            }
        }
                                                                              
        if (has_memory)
            pmux_declare_ram_zero_initial_state(module, sigmap, memory_budget);
    }
    log("OPT_PMUX_INIT_ZERO: source_ff_cells=%d added_zero_bits=%d preserved_explicit_01_bits=%d skipped_unclocked_storage=%d contract=logical_ff_zero_unless_explicit_01\n",
        cells, added, preserved, skipped_latches);
    log("OPT_PMUX_INIT_ZERO: additional source initial-state contract; not arbitrary-initial-state equivalence.\n");
    log("OPT_PMUX_INIT_ZERO_MEMORY: writable_rams=%d roms_unchanged=%d added_zero_bits=%lld additional_segments=%d\n",
        memory_budget.memories, memory_budget.roms,
        static_cast<long long>(memory_budget.added_bits), memory_budget.added_segments);
}

#endif

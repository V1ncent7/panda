/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
 PANDAENDCOMMENT */

/*
 * PANDA taint analysis plugin
 * Ryan Whelan, Tim Leek, Sam Coe, Nathan VanBenschoten
 */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <iostream>

#include "panda/plugin.h"
#include "panda/tcg-llvm.h"

#include <llvm/PassManager.h>
#include <llvm/PassRegistry.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "shad_dir_32.h"
#include "shad_dir_64.h"
#include "llvm_taint_lib.h"
#include "fast_shad.h"
#include "taint_ops.h"
#include "taint2.h"
#include "label_set.h"
#include "taint_api.h"
#include "taint2_hypercalls.h"

#include "callstack_instr/callstack_instr.h"
#include "callstack_instr/callstack_instr_ext.h"

extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int after_block_translate(CPUState *cpu, TranslationBlock *tb);
bool before_block_exec_invalidate_opt(CPUState *cpu, TranslationBlock *tb);
int after_block_exec(CPUState *cpu, TranslationBlock *tb);

int phys_mem_write_callback(CPUState *cpu, target_ulong pc, target_ulong addr, target_ulong size, void *buf);
int phys_mem_read_callback(CPUState *cpu, target_ulong pc, target_ulong addr, target_ulong size, void *buf);

void taint_state_changed(FastShad *, uint64_t, uint64_t);
PPP_PROT_REG_CB(on_taint_change);
PPP_CB_BOILERPLATE(on_taint_change);

bool track_taint_state = false;

int asid_changed_callback(CPUState *env, target_ulong oldval, target_ulong newval);
}

ShadowState *shadow = nullptr; // Global shadow memory

// Pointer passed in init_plugin()
void *taint2_plugin = nullptr;

// Our pass manager to derive taint ops
llvm::FunctionPassManager *FPM = nullptr;

// Taint function pass.
llvm::PandaTaintFunctionPass *PTFP = nullptr;

// For now, taint becomes enabled when a label operation first occurs, and
// becomes disabled when a query operation subsequently occurs
bool taintEnabled = false;

// Lets us know right when taint was disabled
bool taintJustDisabled = false;

// Taint memlog
static taint2_memlog taint_memlog;

// Configuration
bool tainted_pointer = true;
bool optimize_llvm = true;
extern bool inline_taint;
bool debug_taint = false;

// ============== Hypercall stuff ===============
#ifdef TARGET_I386
// shorcuts for accesing the values of x86 registers
#define EAX ((CPUArchState*)cpu->env_ptr)->regs[R_EAX]
#define EBX ((CPUArchState*)cpu->env_ptr)->regs[R_EBX]
#define ECX ((CPUArchState*)cpu->env_ptr)->regs[R_ECX]
#define EDI ((CPUArchState*)cpu->env_ptr)->regs[R_EDI]
#endif

// max length of strnlen or taint query
#define QUERY_HYPERCALL_MAX_LEN 32

/**
 * @brief Constructs a pandalog message for src-level info.
 */
Panda__SrcInfo *pandalog_src_info_create(PandaHypercallStruct phs) {
    Panda__SrcInfo *si = (Panda__SrcInfo *)malloc(sizeof(Panda__SrcInfo));
    *si = PANDA__SRC_INFO__INIT;
    si->filename = phs.src_filename;
    si->astnodename = phs.src_ast_node_name;
    si->linenum = phs.src_linenum;
    si->has_insertionpoint = 0;
    if (phs.insertion_point) {
        si->has_insertionpoint = 1;
        si->insertionpoint = phs.insertion_point;
    }
    si->has_ast_loc_id = 1;
    si->ast_loc_id = phs.src_filename;
    return si;
}

/**
 * @brief Hypercall-initiated taint query of some src-level extent.
 */
void taint_query_hypercall(PandaHypercallStruct phs) {
    CPUState *cpu = first_cpu;
    if  (pandalog && taintEnabled && (taint2_num_labels_applied() > 0)) {
        // okay, taint is on and some labels have actually been applied
        // is there *any* taint on this extent
        uint32_t num_tainted = 0;
        bool is_strnlen = ((int) phs.len == -1);
        uint32_t offset=0;
        while (true) {
            uint32_t va = phs.buf + offset;
            uint32_t pa =  panda_virt_to_phys(cpu, va);
            if (is_strnlen) {
                uint8_t c;
                panda_virtual_memory_rw(cpu, pa, &c, 1, false);
                // null terminator
                if (c==0) break;
            }
            if ((int) pa != -1) {
                Addr a = make_maddr(pa);
                if (taint2_query(a)) {
                    num_tainted ++;
                }
            }
            offset ++;
            // end of query by length or max string length
            if (!is_strnlen && offset == phs.len) break;
            if (is_strnlen && (offset == QUERY_HYPERCALL_MAX_LEN)) break;
        }
        uint32_t len = offset;
        if (num_tainted) {
            // ok at least one byte in the extent is tainted
            // 1. write the pandalog entry that tells us something was tainted on this extent
            Panda__TaintQueryHypercall *tqh = (Panda__TaintQueryHypercall *) malloc (sizeof (Panda__TaintQueryHypercall));
            *tqh = PANDA__TAINT_QUERY_HYPERCALL__INIT;
            tqh->buf = phs.buf;
            tqh->len = len;
            tqh->num_tainted = num_tainted;
            // obtain the actual data out of memory
            // NOTE: first X bytes only!
            uint32_t data[QUERY_HYPERCALL_MAX_LEN];
            uint32_t n = len;
            // grab at most X bytes from memory to pandalog
            // this is just a snippet.  we dont want to write 1M buffer
            if (QUERY_HYPERCALL_MAX_LEN < len) n = QUERY_HYPERCALL_MAX_LEN;
            for (uint32_t i=0; i<n; i++) {
                data[i] = 0;
                uint8_t c;
                panda_virtual_memory_rw(cpu, phs.buf+i, &c, 1, false);
                data[i] = c;
            }
            tqh->n_data = n;
            tqh->data = data;
            // 2. write out src-level info
            Panda__SrcInfo *si = pandalog_src_info_create(phs);
            tqh->src_info = si;
            // 3. write out callstack info
            Panda__CallStack *cs = pandalog_callstack_create();
            tqh->call_stack = cs;
            std::vector<Panda__TaintQuery *> tq;
            for (uint32_t offset=0; offset<len; offset++) {
                uint32_t va = phs.buf + offset;
                uint32_t pa =  panda_virt_to_phys(cpu, va);
                if ((int) pa != -1) {
                    Addr a = make_maddr(pa);
                    if (taint2_query(a)) {
                        tq.push_back(taint2_query_pandalog(a, offset));
                    }
                }
            }
            tqh->n_taint_query = tq.size();
            tqh->taint_query = (Panda__TaintQuery **) malloc(sizeof(Panda__TaintQuery *) * tqh->n_taint_query);
            for (uint32_t i=0; i<tqh->n_taint_query; i++) {
                tqh->taint_query[i] = tq[i];
            }
            Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
            ple.taint_query_hypercall = tqh;
            pandalog_write_entry(&ple);
            free(tqh->src_info);
            pandalog_callstack_free(tqh->call_stack);
            for (uint32_t i=0; i<tqh->n_taint_query; i++) {
                pandalog_taint_query_free(tqh->taint_query[i]);
            }
            free(tqh);
        }
    }
}

void lava_attack_point(PandaHypercallStruct phs) {
    if (pandalog) {
        Panda__AttackPoint *ap = (Panda__AttackPoint *)malloc(sizeof (Panda__AttackPoint));
        *ap = PANDA__ATTACK_POINT__INIT;
        ap->info = phs.info;
        Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
        ple.attack_point = ap;
        ple.attack_point->src_info = pandalog_src_info_create(phs);
        ple.attack_point->call_stack = pandalog_callstack_create();
        pandalog_write_entry(&ple);
        free(ple.attack_point->src_info);
        pandalog_callstack_free(ple.attack_point->call_stack);
        free(ap);
    }
}

int guest_hypercall_callback(CPUState *cpu) {
#if defined(TARGET_I386)
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    if (taintEnabled) {
        if (EAX == 7 || EAX == 8) {
            target_ulong buf_start = EBX;
            target_ulong buf_len = ECX;
            long label = EDI;
            if (R_EAX == 7) {
                // Standard buffer label
                printf("taint2: single taint label\n");
                taint2_add_taint_ram_single_label(cpu, (uint64_t)buf_start,
                                                  (int)buf_len, label);
            }
            else if (R_EAX == 8) {
                // Positional buffer label
                printf("taint2: positional taint label\n");
                taint2_add_taint_ram_pos(cpu, (uint64_t)buf_start, (int)buf_len, label);
            }
        }
        else {
            // LAVA Hypercall
            target_ulong addr = panda_virt_to_phys(cpu, env->regs[R_EAX]);
            if ((int)addr == -1) {
                // if EAX is not a valid ptr, then it is unlikely that this is a
                // PandaHypercall which requires EAX to point to a block of memory
                // defined by PandaHypercallStruct
                printf ("cpuid with invalid ptr in EAX: vaddr=0x%x paddr=0x%x. Probably not a Panda Hypercall\n",
                        (uint32_t) env->regs[R_EAX], (uint32_t) addr);
            }
            else if (pandalog) {
                PandaHypercallStruct phs;
                panda_virtual_memory_rw(cpu, env->regs[R_EAX], (uint8_t *) &phs, sizeof(phs), false);
                if (phs.magic == 0xabcd) {
                    if  (phs.action == 11) {
                        // it's a lava query
                        taint_query_hypercall(phs);
                    }
                    else if (phs.action == 12) {
                        // it's an attack point sighting
                        lava_attack_point(phs);
                    }
                    else if (phs.action == 13) {
                        // it's a pri taint query point
                        // do nothing and let pri_taint with hypercall
                        // option handle it
                    }
                    else if (phs.action == 14) {
                        // reserved for taint-exploitability
                    }
                    else {
                        printf("Unknown hypercall action %d\n", phs.action);
                    }
                }
                else {
                    printf ("Invalid magic value in PHS struct: %x != 0xabcd.\n", phs.magic);
                }
            }
        }
    }
    return 1;
#elif defined(TARGET_ARM)
    // R0 is command (label or query)
    // R1 is buf_start
    // R2 is length
    // R3 is offset (not currently implemented)
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    if (env->regs[0] == 7 || env->regs[0] == 8) { //Taint label
        if (!taintEnabled) {
            printf("Taint plugin: Label operation detected @ %lu\n", rr_get_guest_instr_count());
            printf("Enabling taint processing\n");
            taint2_enable_taint();
        }
        // FIXME: do labeling here.
    }
    else if (env->regs[0] == 9) { //Query taint on label
        if (taintEnabled) {
            printf("Taint plugin: Query operation detected @ %lu\n", rr_get_guest_instr_count());
        }
    }
    return 1;
#else
    // other architectures
    return 0;
#endif
}
/*
 * These memory callbacks are only for whole-system mode.  User-mode memory
 * accesses are captured by IR instrumentation.
 */
int phys_mem_write_callback(CPUState *cpu, target_ulong pc, target_ulong addr, target_ulong size, void *buf) {
    taint_memlog_push(&taint_memlog, addr);
    return 0;
}

int phys_mem_read_callback(CPUState *cpu, target_ulong pc, target_ulong addr, target_ulong size) {
    taint_memlog_push(&taint_memlog, addr);
    return 0;
}

void taint2_enable_tainted_pointer(void) {
    tainted_pointer = true;
}


void taint2_enable_taint(void) {
    if(taintEnabled) {return;}
    std::cerr << PANDA_MSG << __FUNCTION__ << std::endl;
    taintEnabled = true;
    panda_cb pcb;

    pcb.before_block_exec_invalidate_opt = before_block_exec_invalidate_opt;
    panda_register_callback(taint2_plugin, PANDA_CB_BEFORE_BLOCK_EXEC_INVALIDATE_OPT, pcb);
    pcb.phys_mem_before_read = phys_mem_read_callback;
    panda_register_callback(taint2_plugin, PANDA_CB_PHYS_MEM_BEFORE_READ, pcb);
    pcb.phys_mem_before_write = phys_mem_write_callback;
    panda_register_callback(taint2_plugin, PANDA_CB_PHYS_MEM_BEFORE_WRITE, pcb);
    pcb.asid_changed = asid_changed_callback;
    panda_register_callback(taint2_plugin, PANDA_CB_ASID_CHANGED, pcb);

    panda_enable_precise_pc(); //before_block_exec requires precise_pc for panda_current_asid

    if (!execute_llvm){
        panda_enable_llvm();
    }
    panda_enable_llvm_helpers();

    if (shadow) delete shadow;
    shadow = new ShadowState();

    // Initialize memlog.
    memset(&taint_memlog, 0, sizeof(taint_memlog));

    llvm::Module *mod = tcg_llvm_ctx->getModule();
    FPM = tcg_llvm_ctx->getFunctionPassManager();

    std::cerr << PANDA_MSG "LLVM optimizations " << PANDA_FLAG_STATUS(optimize_llvm) << std::endl;
    if (optimize_llvm) {
        llvm::PassManagerBuilder Builder;
        Builder.OptLevel = 2;
        Builder.SizeLevel = 0;
        Builder.populateFunctionPassManager(*FPM);
    }

    // Add the taint analysis pass to our taint pass manager
    PTFP = new llvm::PandaTaintFunctionPass(shadow, &taint_memlog);
    FPM->add(PTFP);

    FPM->doInitialization();

    // Populate module with helper function taint ops
    for (auto i = mod->begin(); i != mod->end(); i++){
        if (!i->isDeclaration()) PTFP->runOnFunction(*i);
    }

    std::cerr << PANDA_MSG "Done processing helper functions for taint." << std::endl;

    std::string err;
    if(verifyModule(*mod, llvm::AbortProcessAction, &err)){
        std::cerr << PANDA_MSG << err << std::endl;
        exit(1);
    }

#ifdef TAINT2_DEBUG
    tcg_llvm_write_module(tcg_llvm_ctx, "/tmp/llvm-mod.bc");
#endif

    std::cerr << "Done verifying module. Running..." << std::endl;
}

// Execute taint ops
int after_block_exec(CPUState *cpu, TranslationBlock *tb) {
    if (taintJustDisabled){
        taintJustDisabled = false;
        execute_llvm = 0;
        generate_llvm = 0;
        panda_do_flush_tb();
        panda_disable_memcb();
    }
    return 0;
}

__attribute__((unused)) static void print_labels(uint32_t el, void *stuff) {
    printf("%d ", el);
}

__attribute__((unused)) static void record_bit(uint32_t el, void *array) {
    *(uint64_t *)array |= 1 << el;
}

#if 0
// move this to a generic utilities header/source file?
#define PANDA_MAX_STRING_READ 256
void panda_virtual_string_read(CPUState *cpu, target_ulong vaddr, char *str) {
    for (uint32_t i=0; i<PANDA_MAX_STRING_READ; i++) {
        uint8_t c = 0;
        if (-1 == panda_virtual_memory_rw(cpu, vaddr + i, &c, 1, false)) {
            printf("Can't access memory at " TARGET_FMT_lx "\n", vaddr + i);
            str[i] = 0;
            break;
        }
        str[i] = c;
        if (c==0) break;
    }
    str[PANDA_MAX_STRING_READ-1] = 0;
}
#endif

/**
 * @brief Wrapper for running the registered `on_taint_change` PPP callbacks.
 * Called by the shadow memory implementation whenever changes occur to it.
 */
void taint_state_changed(FastShad *fast_shad, uint64_t shad_addr, uint64_t size) {
    Addr addr;
    if (fast_shad == &shadow->llv) {
        addr = make_laddr(shad_addr / MAXREGSIZE, shad_addr % MAXREGSIZE);
    } else if (fast_shad == &shadow->ram) {
        addr = make_maddr(shad_addr);
    } else if (fast_shad == &shadow->grv) {
        addr = make_greg(shad_addr / sizeof(target_ulong), shad_addr % sizeof(target_ulong));
    } else if (fast_shad == &shadow->gsv) {
        addr.typ = GSPEC;
        addr.val.gs = shad_addr;
        addr.off = 0;
        addr.flag = (AddrFlag)0;
    } else if (fast_shad == &shadow->ret) {
        addr.typ = RET;
        addr.val.ret = 0;
        addr.off = shad_addr;
        addr.flag = (AddrFlag)0;
    } else return;

    PPP_RUN_CB(on_taint_change, addr, size);
}

bool before_block_exec_invalidate_opt(CPUState *cpu, TranslationBlock *tb) {
    if (taintEnabled) {
        return tb->llvm_tc_ptr ? false : true /* invalidate! */;
    }
    return false;
}


/**
 * @brief Basic initialization for `taint2` plugin.
 *
 * @note Taint propagation won't happen before you also call `taint2_enable_taint()`.
 */
bool init_plugin(void *self) {
    taint2_plugin = self;

    // set required panda options
    panda_enable_memcb();
    panda_disable_tb_chaining();

    // hook taint2 callbacks
#ifdef TAINT2_HYPERCALLS
    panda_cb pcb;
    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);
#endif
#if 0
    // also registered by taint2_enable_taint() - registering twice triggers assertion error
    // keep this commented until we figure out which one we should eliminate
    pcb.before_block_exec_invalidate_opt = before_block_exec_invalidate_opt;
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC_INVALIDATE_OPT, pcb);
#endif

    // parse arguments
    panda_arg_list *args = panda_get_args("taint2");
    tainted_pointer = !panda_parse_bool_opt(args, "no_tp", "track taint through pointer dereference");
    std::cerr << PANDA_MSG "propagation via pointer dereference " << PANDA_FLAG_STATUS(tainted_pointer) << std::endl;
    inline_taint = panda_parse_bool_opt(args, "inline", "inline taint operations");
    std::cerr << PANDA_MSG "taint operations inlining " << PANDA_FLAG_STATUS(inline_taint) << std::endl;
    optimize_llvm = panda_parse_bool_opt(args, "opt", "run LLVM optimization on taint");
    std::cerr << PANDA_MSG "llvm optimizations " << PANDA_FLAG_STATUS(optimize_llvm) << std::endl;
    debug_taint = panda_parse_bool_opt(args, "debug", "enable taint debugging");
    std::cerr << PANDA_MSG "taint debugging " << PANDA_FLAG_STATUS(debug_taint) << std::endl;

    // load dependencies
    panda_require("callstack_instr");
    assert(init_callstack_instr_api());

    return true;
}

void uninit_plugin(void *self) {
    if (shadow) {
        delete shadow;
        shadow = nullptr;
    }

    panda_disable_llvm();
    panda_disable_memcb();
    panda_enable_tb_chaining();
}

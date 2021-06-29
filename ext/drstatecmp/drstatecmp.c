/* **********************************************************
 * Copyright (c) 2021 Google, Inc.   All rights reserved.
 * **********************************************************/
/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "dr_defines.h"
#include "dr_api.h"
#include "drmgr.h"
#include "drstatecmp.h"
#include <string.h>
#include <stdint.h>

#ifdef DEBUG
#    define ASSERT(x, msg) DR_ASSERT_MSG(x, msg)
#else
#    define ASSERT(x, msg)
#endif

typedef struct {
    dr_mcontext_t saved_state_for_restore; /* Last saved machine state for restoration. */
    dr_mcontext_t saved_state_for_cmp;     /* Last saved machine state for comparison. */
} drstatecmp_saved_states;

static int tls_idx = -1; /* For thread local storage info. */

/* Label types. */
typedef enum {
    DRSTATECMP_LABEL_TERM,    /* Denotes the terminator of the original bb. */
    DRSTATECMP_LABEL_ORIG_BB, /* Denotes the beginning of the original bb. */
    DRSTATECMP_LABEL_COPY_BB, /* Denotes the beginning of the bb copy. */
    DRSTATECMP_LABEL_COUNT,
} drstatecmp_label_t;

/* Reserve space for the label values. */
static ptr_uint_t label_base;
static void
drstatecmp_label_init(void)
{
    label_base = drmgr_reserve_note_range(DRSTATECMP_LABEL_COUNT);
    ASSERT(label_base != DRMGR_NOTE_NONE, "failed to reserve note space");
}

/* Get label values. */
static inline ptr_int_t
get_label_val(drstatecmp_label_t label_type)
{
    return (ptr_int_t)(label_base + label_type);
}

/* Compare instr label against a label_type. Returns true if they match. */
static inline bool
match_label_val(instr_t *instr, drstatecmp_label_t label_type)
{
    return instr_get_note(instr) == (void *)get_label_val(label_type);
}

/* Create and insert bb labels. */
static instr_t *
drstatecmp_insert_label(void *drcontext, instrlist_t *ilist, instr_t *where,
                        drstatecmp_label_t label_type, bool preinsert)
{
    instr_t *label = INSTR_CREATE_label(drcontext);
    instr_set_meta(label);
    instr_set_note(label, (void *)get_label_val(label_type));
    if (preinsert)
        instrlist_meta_preinsert(ilist, where, label);
    else
        instrlist_meta_postinsert(ilist, where, label);
    return label;
}

typedef struct {
    instr_t *orig_bb_start;
    instr_t *copy_bb_start;
    instr_t *term;
} drstatecmp_dup_labels_t;

/* Returns whether or not instr may have side effects. */
static bool
drstatecmp_may_have_side_effects_instr(instr_t *instr)
{
    /* Instructions with side effects include instructions that write to memory,
     * interrupts, and syscalls.
     */
    return instr_writes_memory(instr) || instr_is_interrupt(instr) ||
        instr_is_syscall(instr);
}

static void
drstatecmp_duplicate_bb(void *drcontext, instrlist_t *bb, drstatecmp_dup_labels_t *labels)
{
    /* Duplication process.
     * Consider the following example bb:
     *   instr1
     *   meta_instr
     *   instr2
     *   term_instr
     *
     * In this stage, we just duplicate the bb (except for its terminating
     * instruction and meta instructions) and add special labels to the original and
     * duplicated blocks.  add saving/restoring of machine
     * state and the state comparison. Note that there might be no term_instr (no control
     * transfer instruction) and the bb just falls-through. Even with no term_instr the
     * jmp and the TERM label are inserted in the same way, as shown in this example.
     *
     * The example bb is transformed, in this stage, as follows:
     * ORIG_BB:
     *   instr1
     *   meta_instr
     *   instr2
     *
     * COPY_BB:
     *   instr1
     *   instr2
     *
     * TERM:
     *   term_instr
     *
     */

    /* Create a clone of bb. */
    instrlist_t *copy_bb = instrlist_clone(drcontext, bb);

    /* Remove all instrumentation code in the bb copy. */
    for (instr_t *instr = instrlist_first(copy_bb); instr != NULL;
         instr = instr_get_next(instr)) {
        if (!instr_is_app(instr)) {
            instrlist_remove(copy_bb, instr);
            instr_destroy(drcontext, instr);
        }
    }

    /* Create and insert the labels. */
    labels->orig_bb_start = drstatecmp_insert_label(drcontext, bb, instrlist_first(bb),
                                                    DRSTATECMP_LABEL_ORIG_BB,
                                                    /*preinsert=*/true);
    labels->copy_bb_start =
        drstatecmp_insert_label(drcontext, copy_bb, instrlist_first(copy_bb),
                                DRSTATECMP_LABEL_COPY_BB, /*preinsert=*/true);
    /* Insert the TERM label before the terminating instruction or after the
     * last instruction if the bb falls through.
     */
    instr_t *term_inst_copy_bb = instrlist_last_app(copy_bb);
    bool preinsert = true;
    if (!instr_is_cti(term_inst_copy_bb) && !instr_is_return(term_inst_copy_bb))
        preinsert = false;
    labels->term = drstatecmp_insert_label(drcontext, copy_bb, term_inst_copy_bb,
                                           DRSTATECMP_LABEL_TERM, preinsert);

    /* Delete the terminating instruction of the original bb (if any) to let the original
     * bb fall through to its copy for re-execution.
     */
    instr_t *term_inst = instrlist_last_app(bb);
    if (instr_is_cti(term_inst) || instr_is_return(term_inst)) {
        instrlist_remove(bb, term_inst);
        instr_destroy(drcontext, term_inst);
    }

    /* Append the instructions of the bb copy to the original bb. */
    instrlist_append(bb, labels->copy_bb_start);
    /* Empty and destroy the bb copy (but not its instructions) since it is not needed
     * anymore.
     */
    instrlist_init(copy_bb);
    instrlist_destroy(drcontext, copy_bb);
}

static void
drstatecmp_save_state_call(int for_cmp)
{
    void *drcontext = dr_get_current_drcontext();
    drstatecmp_saved_states *pt =
        (drstatecmp_saved_states *)drmgr_get_tls_field(drcontext, tls_idx);

    dr_mcontext_t *mcontext = NULL;
    if (for_cmp)
        mcontext = &pt->saved_state_for_cmp;
    else
        mcontext = &pt->saved_state_for_restore;
    mcontext->size = sizeof(*mcontext);
    mcontext->flags = DR_MC_ALL;
    dr_get_mcontext(drcontext, mcontext);
}

static void
drstatecmp_save_state(void *drcontext, instrlist_t *bb, instr_t *instr, bool for_cmp)
{
    dr_insert_clean_call(drcontext, bb, instr, (void *)drstatecmp_save_state_call,
                         false /*fpstate */, 1, OPND_CREATE_INT32((int)for_cmp));
}

static void
drstatecmp_restore_state_call(void)
{
    void *drcontext = dr_get_current_drcontext();
    drstatecmp_saved_states *pt =
        (drstatecmp_saved_states *)drmgr_get_tls_field(drcontext, tls_idx);

    dr_mcontext_t *mcontext = &pt->saved_state_for_restore;
    mcontext->size = sizeof(*mcontext);
    mcontext->flags = DR_MC_ALL;
    dr_set_mcontext(drcontext, mcontext);
}

static void
drstatecmp_restore_state(void *drcontext, instrlist_t *bb, instr_t *instr)
{
    dr_insert_clean_call(drcontext, bb, instr, (void *)drstatecmp_restore_state_call,
                         false /*fpstate */, 0);
}

static void
drstatecmp_check_gpr_value(const char *name, reg_t reg_value, reg_t reg_expected)
{
    DR_ASSERT_MSG(reg_value == reg_expected, name);
}

#ifdef AARCHXX
static void
drstatecmp_check_xflags_value(const char *name, uint reg_value, uint reg_expected)
{
    DR_ASSERT_MSG(reg_value == reg_expected, name);
}
#endif

static void
drstatecmp_check_simd_value
#ifdef X86
    (dr_zmm_t *value, dr_zmm_t *expected)
{
    DR_ASSERT_MSG(!memcmp(value, expected, sizeof(dr_zmm_t)), "SIMD mismatch");
}
#elif defined(AARCHXX)
    (dr_simd_t *value, dr_simd_t *expected)
{
    DR_ASSERT_MSG(!memcmp(value, expected, sizeof(dr_simd_t)), "SIMD mismatch");
}
#endif

#ifdef X86
static void
drstatecmp_check_opmask_value(dr_opmask_t opmask_value, dr_opmask_t opmask_expected)
{
    DR_ASSERT_MSG(opmask_value == opmask_expected, "opmask mismatch");
}
#endif

static void
drstatecmp_compare_state_call(void)
{
    void *drcontext = dr_get_current_drcontext();
    drstatecmp_saved_states *pt =
        (drstatecmp_saved_states *)drmgr_get_tls_field(drcontext, tls_idx);

    dr_mcontext_t *mc_instrumented = &pt->saved_state_for_cmp;
    dr_mcontext_t mc_expected;
    mc_expected.size = sizeof(mc_expected);
    mc_expected.flags = DR_MC_ALL;
    dr_get_mcontext(drcontext, &mc_expected);

#ifdef X86
    drstatecmp_check_gpr_value("xdi", mc_instrumented->xdi, mc_expected.xdi);
    drstatecmp_check_gpr_value("xsi", mc_instrumented->xsi, mc_expected.xsi);
    drstatecmp_check_gpr_value("xbp", mc_instrumented->xbp, mc_expected.xbp);

    drstatecmp_check_gpr_value("xax", mc_instrumented->xax, mc_expected.xax);
    drstatecmp_check_gpr_value("xbx", mc_instrumented->xbx, mc_expected.xbx);
    drstatecmp_check_gpr_value("xcx", mc_instrumented->xcx, mc_expected.xcx);
    drstatecmp_check_gpr_value("xdx", mc_instrumented->xdx, mc_expected.xdx);

#    ifdef X64
    drstatecmp_check_gpr_value("r8", mc_instrumented->r8, mc_expected.r8);
    drstatecmp_check_gpr_value("r9", mc_instrumented->r9, mc_expected.r9);
    drstatecmp_check_gpr_value("r10", mc_instrumented->r10, mc_expected.r10);
    drstatecmp_check_gpr_value("r11", mc_instrumented->r11, mc_expected.r11);
    drstatecmp_check_gpr_value("r12", mc_instrumented->r12, mc_expected.r12);
    drstatecmp_check_gpr_value("r13", mc_instrumented->r13, mc_expected.r13);
    drstatecmp_check_gpr_value("r14", mc_instrumented->r14, mc_expected.r14);
    drstatecmp_check_gpr_value("r15", mc_instrumented->r15, mc_expected.r15);
#    endif

    drstatecmp_check_gpr_value("xflags", mc_instrumented->xflags, mc_expected.xflags);
    for (int i = 0; i < MCXT_NUM_OPMASK_SLOTS; i++) {
        drstatecmp_check_opmask_value(mc_instrumented->opmask[i], mc_expected.opmask[i]);
    }

#elif defined(AARCHXX)
    drstatecmp_check_gpr_value("r0", mc_instrumented->r0, mc_expected.r0);
    drstatecmp_check_gpr_value("r1", mc_instrumented->r1, mc_expected.r1);
    drstatecmp_check_gpr_value("r2", mc_instrumented->r2, mc_expected.r2);
    drstatecmp_check_gpr_value("r3", mc_instrumented->r3, mc_expected.r3);
    drstatecmp_check_gpr_value("r4", mc_instrumented->r4, mc_expected.r4);
    drstatecmp_check_gpr_value("r5", mc_instrumented->r5, mc_expected.r5);
    drstatecmp_check_gpr_value("r6", mc_instrumented->r6, mc_expected.r6);
    drstatecmp_check_gpr_value("r7", mc_instrumented->r7, mc_expected.r7);
    drstatecmp_check_gpr_value("r8", mc_instrumented->r8, mc_expected.r8);
    drstatecmp_check_gpr_value("r9", mc_instrumented->r9, mc_expected.r9);
    drstatecmp_check_gpr_value("r10", mc_instrumented->r10, mc_expected.r10);
    drstatecmp_check_gpr_value("r11", mc_instrumented->r11, mc_expected.r11);
    drstatecmp_check_gpr_value("r12", mc_instrumented->r12, mc_expected.r12);

#    ifdef X64
    drstatecmp_check_gpr_value("r13", mc_instrumented->r13, mc_expected.r13);
    drstatecmp_check_gpr_value("r14", mc_instrumented->r14, mc_expected.r14);
    drstatecmp_check_gpr_value("r15", mc_instrumented->r15, mc_expected.r15);
    drstatecmp_check_gpr_value("r16", mc_instrumented->r16, mc_expected.r16);
    drstatecmp_check_gpr_value("r17", mc_instrumented->r17, mc_expected.r17);
    drstatecmp_check_gpr_value("r18", mc_instrumented->r18, mc_expected.r18);
    drstatecmp_check_gpr_value("r19", mc_instrumented->r19, mc_expected.r19);
    drstatecmp_check_gpr_value("r20", mc_instrumented->r20, mc_expected.r20);
    drstatecmp_check_gpr_value("r21", mc_instrumented->r21, mc_expected.r21);
    drstatecmp_check_gpr_value("r22", mc_instrumented->r22, mc_expected.r22);
    drstatecmp_check_gpr_value("r23", mc_instrumented->r23, mc_expected.r23);
    drstatecmp_check_gpr_value("r24", mc_instrumented->r24, mc_expected.r24);
    drstatecmp_check_gpr_value("r25", mc_instrumented->r25, mc_expected.r25);
    drstatecmp_check_gpr_value("r26", mc_instrumented->r26, mc_expected.r26);
    drstatecmp_check_gpr_value("r27", mc_instrumented->r27, mc_expected.r27);
    drstatecmp_check_gpr_value("r28", mc_instrumented->r28, mc_expected.r28);
    drstatecmp_check_gpr_value("r29", mc_instrumented->r29, mc_expected.r29);
#    endif

    drstatecmp_check_gpr_value("lr", mc_instrumented->lr, mc_expected.lr);
    drstatecmp_check_xflags_value("xflags", mc_instrumented->xflags, mc_expected.xflags);

#else
#    error NYI
#endif

    drstatecmp_check_gpr_value("xsp", mc_instrumented->xsp, mc_expected.xsp);
    for (int i = 0; i < MCXT_NUM_SIMD_SLOTS; i++) {
        drstatecmp_check_simd_value(&mc_instrumented->simd[i], &mc_expected.simd[i]);
    }
}

static void
drstatecmp_compare_state(void *drcontext, instrlist_t *bb, instr_t *instr)
{
    dr_insert_clean_call(drcontext, bb, instr, (void *)drstatecmp_compare_state_call,
                         false /*fpstate */, 0);
}

static void
drstatecmp_check_reexecution(void *drcontext, instrlist_t *bb,
                             drstatecmp_dup_labels_t *labels)
{
    /* Save state at the beginning of the original bb in order to restore it at the
     * end of it (to enable re-execution of the bb). */
    drstatecmp_save_state(drcontext, bb, labels->orig_bb_start, /*for_cmp=*/false);

    /* Save the state at the end of the original bb (or alternatively before the start of
     * the copy bb) for later comparison and restore the machine state to the state before
     * executing the original bb (allows re-execution).
     */
    drstatecmp_save_state(drcontext, bb, labels->copy_bb_start, /*for_cmp=*/true);
    drstatecmp_restore_state(drcontext, bb, labels->copy_bb_start);

    /* Compare the state at the end of the copy bb (uninstrumented) with the saved state
     * at the end of the original (instrumented) bb to detect clobbering by the
     * instrumentation.
     */
    drstatecmp_compare_state(drcontext, bb, labels->term);
}

/* Duplicate the side-effect basic block for re-execution and add saving/restoring of
 * machine state and state comparison to check for instrumentation-induced clobbering of
 * machine state.
 */
static void
drstatecmp_post_process_side_effect_free_bb(void *drcontext, instrlist_t *bb)
{
  drstatecmp_dup_labels_t labels;
  drstatecmp_duplicate_bb(drcontext, bb, &labels);
  drstatecmp_check_reexecution(drcontext, bb, &labels);
}

static void
drstatecmp_post_process_bb_with_side_effects(void)
{
    /* TODO i#4678: Add checks for bbs with side effects.  */
}

static dr_emit_flags_t
drstatecmp_post_instru_phase(void *drcontext, void *tag, instrlist_t *bb,
                               bool for_trace, bool translating)
{
    /* Determine whether the basic block is free of side effects. */
    bool side_effect_free = true;
    for (instr_t *inst = instrlist_first_app(bb); inst != NULL;
         inst = instr_get_next_app(inst)) {
        if (drstatecmp_may_have_side_effects_instr(inst)) {
            side_effect_free = false;
            break;
        }
    }

    if (side_effect_free) {
        drstatecmp_post_process_side_effect_free_bb(drcontext, bb);
    } else {
        /* Basic blocks with side-effects not handled yet. */
        drstatecmp_post_process_bb_with_side_effects();
    }

    return DR_EMIT_DEFAULT;
}


/****************************************************************************
 *  THREAD INIT AND EXIT
 */

static void
drstatecmp_thread_init(void *drcontext)
{
    drstatecmp_saved_states *pt = (drstatecmp_saved_states *)dr_thread_alloc(
        drcontext, sizeof(drstatecmp_saved_states));
    drmgr_set_tls_field(drcontext, tls_idx, (void *)pt);
    memset(pt, 0, sizeof(*pt));
}

static void
drstatecmp_thread_exit(void *drcontext)
{
    drstatecmp_saved_states *pt =
        (drstatecmp_saved_states *)drmgr_get_tls_field(drcontext, tls_idx);
    ASSERT(pt != NULL, "thread-local storage should not be NULL");
    dr_thread_free(drcontext, pt, sizeof(drstatecmp_saved_states));
}

/***************************************************************************
 * INIT AND EXIT
 */

static int drstatecmp_init_count;

drstatecmp_status_t
drstatecmp_init(void)
{
    int count = dr_atomic_add32_return_sum(&drstatecmp_init_count, 1);
    if (count != 1)
        return DRSTATECMP_ERROR_ALREADY_INITIALIZED;

    drmgr_priority_t priority = { sizeof(drmgr_priority_t),
                                  DRMGR_PRIORITY_NAME_DRSTATECMP, NULL, NULL,
                                  DRMGR_PRIORITY_DRSTATECMP };

    drmgr_init();

    tls_idx = drmgr_register_tls_field();
    if (tls_idx == -1)
        return DRSTATECMP_ERROR;

    drstatecmp_label_init();

    if (!drmgr_register_thread_init_event(drstatecmp_thread_init) ||
        !drmgr_register_thread_exit_event(drstatecmp_thread_exit) ||
        !drmgr_register_bb_post_instru_event(drstatecmp_post_instru_phase, &priority))
        return DRSTATECMP_ERROR;

    return DRSTATECMP_SUCCESS;
}

drstatecmp_status_t
drstatecmp_exit(void)
{
    int count = dr_atomic_add32_return_sum(&drstatecmp_init_count, -1);
    if (count != 0)
        return DRSTATECMP_ERROR_NOT_INITIALIZED;

    if (!drmgr_unregister_thread_init_event(drstatecmp_thread_init) ||
        !drmgr_unregister_thread_exit_event(drstatecmp_thread_exit) ||
        !drmgr_unregister_tls_field(tls_idx) ||
        !drmgr_unregister_bb_post_instru_event(drstatecmp_post_instru_phase))
        return DRSTATECMP_ERROR;

    drmgr_exit();

    return DRSTATECMP_SUCCESS;
}

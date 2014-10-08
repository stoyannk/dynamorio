/* **********************************************************
 * Copyright (c) 2011-2014 Google, Inc.  All rights reserved.
 * Copyright (c) 2001-2009 VMware, Inc.  All rights reserved.
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
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2001 Hewlett-Packard Company */

/* disassemble.c -- printing of x86 instructions
 *
 * Note that when printing out instructions:
 * Uses DR syntax of "srcs -> dsts" including implicit operands, unless
 * -syntax_att (AT&T syntax) or -syntax_intel (Intel syntax) is specified.
 * See the info pages for "as" for details on the differences.
 */

/*
 * XXX disassembly discrepancies:
 * 1) I print "%st(0),%st(1)", gdb prints "%st,%st(1)"
 * 2) I print movzx, gdb prints movzw (with an 'l' suffix tacked on)
 * 3) gdb says bound and leave are to be printed "Intel order", not AT&T ?!?
 *    From gdb: "The enter and bound instructions are printed with operands
 *    in the same order as the intel book; everything else is printed in
 *    reverse order."
 */

#include "../globals.h"
#include "arch.h"
#include "instr.h"
#include "decode.h"
#include "decode_fast.h"
/* FIXME i#1551: refactor this file and avoid this x86-specific include in base arch/ */
#include "x86/decode_private.h"
#include "disassemble.h"
#include "../module_shared.h"
#include <string.h>

/* these are only needed for symbolic address lookup: */
#include "../fragment.h" /* for fragment_pclookup */
#include "../link.h" /* for linkstub lookup */

#include "../fcache.h" /* for in_fcache */

#if defined(INTERNAL) || defined(DEBUG) || defined(CLIENT_INTERFACE)

#ifdef DEBUG
/* case 10450: give messages to clients */
/* we can't undef ASSERT b/c of DYNAMO_OPTION */
# undef ASSERT_TRUNCATE
# undef ASSERT_BITFIELD_TRUNCATE
# undef ASSERT_NOT_REACHED
# define ASSERT_TRUNCATE DO_NOT_USE_ASSERT_USE_CLIENT_ASSERT_INSTEAD
# define ASSERT_BITFIELD_TRUNCATE DO_NOT_USE_ASSERT_USE_CLIENT_ASSERT_INSTEAD
# define ASSERT_NOT_REACHED DO_NOT_USE_ASSERT_USE_CLIENT_ASSERT_INSTEAD
#endif

/****************************************************************************
 * Printing of instructions
 */

void
disassemble_options_init(void)
{
    dr_disasm_flags_t flags = DYNAMO_OPTION(disasm_mask);
    if (DYNAMO_OPTION(syntax_intel)) {
        flags |= DR_DISASM_INTEL;
        flags &= ~DR_DISASM_ATT; /* mutually exclusive */
    }
    if (DYNAMO_OPTION(syntax_att)) {
        flags |= DR_DISASM_ATT;
        flags &= ~DR_DISASM_INTEL; /* mutually exclusive */
    }
    /* This option is separate as it's not strictly a disasm style */
    dynamo_options.decode_strict = TEST(DR_DISASM_STRICT_INVALID, flags);
    if (DYNAMO_OPTION(decode_strict))
        flags |= DR_DISASM_STRICT_INVALID; /* for completeness */
    dynamo_options.disasm_mask = flags;
}

DR_API
void
disassemble_set_syntax(dr_disasm_flags_t flags)
{
#ifndef STANDALONE_DECODER
    options_make_writable();
#endif
    dynamo_options.disasm_mask = flags;
    /* This option is separate as it's not strictly a disasm style */
    dynamo_options.decode_strict = TEST(DR_DISASM_STRICT_INVALID, flags);
#ifndef STANDALONE_DECODER
    options_restore_readonly();
#endif
}

#define BYTES_PER_LINE 7

static void
internal_instr_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                           dcontext_t *dcontext, instr_t *instr);

static inline const char *
immed_prefix(void)
{
    return (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ? "" : "$");
}

static inline const char *
postop_suffix(void)
{
    return (TESTANY(DR_DISASM_INTEL|DR_DISASM_ATT, DYNAMO_OPTION(disasm_mask)) ?
            "" : " ");
}

static void
reg_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                reg_id_t reg, const char *prefix, const char *suffix)
{
    print_to_buffer(buf, bufsz, sofar,
                    TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ? "%s%s%s" : "%s%%%s%s",
                    prefix, reg_names[reg], suffix);
}

static const char *
opnd_size_suffix_dr(opnd_t opnd)
{
    int sz = opnd_size_in_bytes(opnd_get_size(opnd));
    switch (sz) {
    case 1: return "1byte";
    case 2: return "2byte";
    case 4: return "4byte";
    case 6: return "6byte";
    case 8: return "8byte";
    case 10: return "10byte";
    case 12: return "12byte";
    case 14: return "14byte";
    case 15: return "15byte";
    case 16: return "16byte";
    case 28: return "28byte";
    case 32: return "32byte";
    case 40: return "40byte";
    case 94: return "94byte";
    case 108: return "108byte";
    case 512: return "512byte";
    }
    return "";
}

static const char *
opnd_size_suffix_intel(opnd_t opnd)
{
    int sz = opnd_size_in_bytes(opnd_get_size(opnd));
    switch (sz) {
    case 1: return "byte";
    case 2: return "word";
    case 4: return "dword";
    case 6: return "fword";
    case 8: return "qword";
    case 10: return "tbyte";
    case 12: return "";
    case 16: return "oword";
    case 32: return "yword";
    }
    return "";
}

static void
opnd_mem_disassemble_prefix(char *buf, size_t bufsz, size_t *sofar INOUT,
                            dcontext_t *dcontext, opnd_t opnd)
{
    if (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
        const char *size_str = opnd_size_suffix_intel(opnd);
        if (size_str[0] != '\0')
            print_to_buffer(buf, bufsz, sofar, "%s ptr [", size_str);
        else /* assume size implied by opcode */
            print_to_buffer(buf, bufsz, sofar, "[");
    }

}

static void
opnd_base_disp_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                           dcontext_t *dcontext, opnd_t opnd)
{
    reg_id_t seg = opnd_get_segment(opnd);
    reg_id_t base = opnd_get_base(opnd);
    int disp = opnd_get_disp(opnd);
    reg_id_t index = opnd_get_index(opnd);
    int scale = opnd_get_scale(opnd);

    opnd_mem_disassemble_prefix(buf, bufsz, sofar, dcontext, opnd);

    if (seg != REG_NULL)
        reg_disassemble(buf, bufsz, sofar, seg, "", ":");

    if (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
        if (base != REG_NULL)
            reg_disassemble(buf, bufsz, sofar, base, "", "");
        if (index != REG_NULL) {
            reg_disassemble(buf, bufsz, sofar, index, base == REG_NULL ? "" : "+", "");
            if (scale > 1)
                print_to_buffer(buf, bufsz, sofar, "*%d", scale);
        }
    }

    if (disp != 0 || (base == REG_NULL && index == REG_NULL) ||
        opnd_is_disp_encode_zero(opnd)) {
        if (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
            /* windbg negates if top byte is 0xff
             * for x64 udis86 negates if at all negative
             */
            if (IF_X64_ELSE(disp < 0, (disp & 0xff000000) == 0xff000000)) {
                disp = -disp;
                print_to_buffer(buf, bufsz, sofar, "-");
            } else if (base != REG_NULL || index != REG_NULL)
                print_to_buffer(buf, bufsz, sofar, "+");
        }
        if (disp >= INT8_MIN && disp <= INT8_MAX &&
            !opnd_is_disp_force_full(opnd))
            print_to_buffer(buf, bufsz, sofar, "0x%02x", disp);
        else if (opnd_is_disp_short_addr(opnd))
            print_to_buffer(buf, bufsz, sofar, "0x%04x", disp);
        else /* there are no 64-bit displacements */
            print_to_buffer(buf, bufsz, sofar, "0x%08x", disp);
    }

    if (!TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
        if (base != REG_NULL || index != REG_NULL) {
            print_to_buffer(buf, bufsz, sofar, "(");
            if (base != REG_NULL)
                reg_disassemble(buf, bufsz, sofar, base, "", "");
            if (index != REG_NULL) {
                reg_disassemble(buf, bufsz, sofar, index, ",", "");
                if (scale != 0)
                    print_to_buffer(buf, bufsz, sofar, ",%d", scale);
            }
            print_to_buffer(buf, bufsz, sofar, ")");
        }
    }

    if (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)))
        print_to_buffer(buf, bufsz, sofar, "]");
}

static bool
print_known_pc_target(char *buf, size_t bufsz, size_t *sofar INOUT,
                      dcontext_t *dcontext, byte *target)
{
    bool printed = false;
#ifndef STANDALONE_DECODER
    /* symbolic addresses */
    if (ENTER_DR_HOOK != NULL && target == (app_pc) ENTER_DR_HOOK) {
        print_to_buffer(buf, bufsz, sofar,
                        "$"PFX" <enter_dynamorio_hook> ", target);
        printed = true;
    } else if (EXIT_DR_HOOK != NULL && target == (app_pc) EXIT_DR_HOOK) {
        print_to_buffer(buf, bufsz, sofar,
                        "$"PFX" <exit_dynamorio_hook> ", target);
        printed = true;
    } else if (dcontext != NULL && dynamo_initialized && !standalone_library) {
        const char *gencode_routine = NULL;
        const char *ibl_brtype;
        const char *ibl_name =
            get_ibl_routine_name(dcontext, target, &ibl_brtype);
        if (ibl_name == NULL && in_coarse_stub_prefixes(target) &&
            *target == JMP_OPCODE) {
            ibl_name = get_ibl_routine_name(dcontext,
                                            PC_RELATIVE_TARGET(target+1),
                                            &ibl_brtype);
        }
# ifdef WINDOWS
        /* must test first, as get_ibl_routine_name will think "bb_ibl_indjmp" */
        if (dcontext != GLOBAL_DCONTEXT) {
            if (target == shared_syscall_routine(dcontext))
                gencode_routine = "shared_syscall";
            else if (target == unlinked_shared_syscall_routine(dcontext))
                gencode_routine = "unlinked_shared_syscall";
        } else {
            if (target == shared_syscall_routine_ex(dcontext _IF_X64(GENCODE_X64)))
                gencode_routine = "shared_syscall";
            else if (target == unlinked_shared_syscall_routine_ex
                     (dcontext _IF_X64(GENCODE_X64)))
                gencode_routine = "unlinked_shared_syscall";
#  ifdef X64
            else if (target == shared_syscall_routine_ex
                     (dcontext _IF_X64(GENCODE_X86)))
                gencode_routine = "x86_shared_syscall";
            else if (target == unlinked_shared_syscall_routine_ex
                     (dcontext _IF_X64(GENCODE_X86)))
                gencode_routine = "x86_unlinked_shared_syscall";
#  endif
        }
# endif
        if (ibl_name) {
            /* can't use gencode_routine since need two strings here */
            print_to_buffer(buf, bufsz, sofar, "$"PFX" <%s_%s>",
                            target, ibl_name, ibl_brtype);
            printed = true;
        } else if (SHARED_FRAGMENTS_ENABLED() && target ==
                   fcache_return_shared_routine(IF_X64(GENCODE_X64)))
            gencode_routine = "fcache_return";
# ifdef X64
        else if (SHARED_FRAGMENTS_ENABLED() && target ==
                 fcache_return_shared_routine(IF_X64(GENCODE_X86)))
            gencode_routine = "x86_fcache_return";
# endif
        else if (dcontext != GLOBAL_DCONTEXT &&
                 target == fcache_return_routine(dcontext))
            gencode_routine = "fcache_return";
        else if (DYNAMO_OPTION(coarse_units)) {
            if (target == fcache_return_coarse_prefix(target, NULL) ||
                target == fcache_return_coarse_routine(IF_X64(GENCODE_X64)))
                gencode_routine = "fcache_return_coarse";
            else if (target == trace_head_return_coarse_prefix(target, NULL) ||
                     target == trace_head_return_coarse_routine
                     (IF_X64(GENCODE_X64)))
                gencode_routine = "trace_head_return_coarse";
# ifdef X64
            else if (target == fcache_return_coarse_prefix(target, NULL) ||
                     target == fcache_return_coarse_routine(IF_X64(GENCODE_X86)))
                gencode_routine = "x86_fcache_return_coarse";
            else if (target == trace_head_return_coarse_prefix(target, NULL) ||
                     target == trace_head_return_coarse_routine
                     (IF_X64(GENCODE_X86)))
                gencode_routine = "x86_trace_head_return_coarse";
# endif
        }
#ifdef PROFILE_RDTSC
        else if ((void *)target == profile_fragment_enter)
            gencode_routine = "profile_fragment_enter";
#endif
#ifdef TRACE_HEAD_CACHE_INCR
        else if ((void *)target == trace_head_incr_routine(dcontext))
            gencode_routine = "trace_head_incr";
#endif

        if (gencode_routine != NULL) {
            print_to_buffer(buf, bufsz, sofar, "$"PFX" <%s> ",
                            target, gencode_routine);
            printed = true;
        } else if (!printed && fragment_initialized(dcontext)) {
            /* see if target is in a fragment */
            bool alloc = false;
            fragment_t *fragment;
#ifdef DEBUG
            fragment_t wrapper;
            /* Unfortunately our fast lookup by fcache unit has lock
             * ordering issues which we get around by using the htable
             * method, though that won't find invisible fragments
             * (FIXME: for those could perhaps pass in a pointer).
             * For !DEADLOCK_AVOIDANCE, OWN_MUTEX's conservative imprecision
             * is fine.
             */
            if ((SHARED_FRAGMENTS_ENABLED() &&
                 self_owns_recursive_lock(&change_linking_lock))
                /* HACK to avoid recursion if the pclookup invokes
                 * decode_fragment() (for coarse target) and it then invokes
                 * disassembly
                 */
                IF_DEBUG(|| (dcontext != GLOBAL_DCONTEXT &&
                             dcontext->in_opnd_disassemble))) {
                fragment = fragment_pclookup_by_htable(dcontext, (void *)target,
                                                       &wrapper);
            } else {
                bool prev_flag = false;
                if (dcontext != GLOBAL_DCONTEXT) {
                    prev_flag = dcontext->in_opnd_disassemble;
                    dcontext->in_opnd_disassemble = true;
                }
#endif /* shouldn't be any logging so no disasm in the middle of sensitive ops */
                fragment = fragment_pclookup_with_linkstubs(dcontext, target,
                                                            &alloc);
#ifdef DEBUG
                if (dcontext != GLOBAL_DCONTEXT)
                    dcontext->in_opnd_disassemble = prev_flag;
            }
#endif
            if (fragment != NULL) {
                if (FCACHE_ENTRY_PC(fragment) == (cache_pc)target ||
                    FCACHE_PREFIX_ENTRY_PC(fragment) == (cache_pc)target ||
                    FCACHE_IBT_ENTRY_PC(fragment) == (cache_pc)target) {
#ifdef DEBUG
                    print_to_buffer(buf, bufsz, sofar, "$"PFX" <fragment %d> ",
                                    target, fragment->id);
#else
                    print_to_buffer(buf, bufsz, sofar, "$"PFX" <fragment "PFX"> ",
                                    target, fragment->tag);
#endif
                    printed = true;
                } else if (!TEST(FRAG_FAKE, fragment->flags)) {
                    /* check exit stubs */
                    linkstub_t *ls;
                    int ls_num = 0;
                    CLIENT_ASSERT(!TEST(FRAG_FAKE, fragment->flags),
                                  "opnd_disassemble: invalid target");
                    for (ls=FRAGMENT_EXIT_STUBS(fragment); ls; ls=LINKSTUB_NEXT_EXIT(ls)) {
                        if (target == EXIT_STUB_PC(dcontext, fragment, ls)) {
                            print_to_buffer(buf, bufsz, sofar,
                                            "$"PFX" <exit stub %d> ",
                                            target, ls_num);
                            printed = true;
                            break;
                        }
                        ls_num++;
                    }
                }
                if (alloc)
                    fragment_free(dcontext, fragment);
            } else if (coarse_is_entrance_stub(target)) {
                print_to_buffer(buf, bufsz, sofar,
                                "$"PFX" <entrance stub for "PFX"> ",
                                target, entrance_stub_target_tag(target, NULL));
                printed = true;
            }
        }
    } else if (dynamo_initialized && !SHARED_FRAGMENTS_ENABLED() &&
               !standalone_library) {
        print_to_buffer(buf, bufsz, sofar, "NULL DCONTEXT! ");
    }
#endif /* !STANDALONE_DECODER */
    return printed;
}

static void
internal_opnd_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                          dcontext_t *dcontext, opnd_t opnd,
                          bool use_size_sfx)
{
    switch (opnd.kind) {
    case NULL_kind:
        return;
    case IMMED_INTEGER_kind:
        {
            opnd_size_t sz = opnd_get_size(opnd);
            /* PR 327775: when we don't know other operands we truncate.
             * We rely on instr_disassemble to temporarily change operand
             * size to sign-extend to match the size of adjacent operands.
             */
            if (sz == OPSZ_1 || sz == OPSZ_0) {
                print_to_buffer(buf, bufsz, sofar, "%s0x%02x", immed_prefix(),
                                (uint)((byte)opnd_get_immed_int(opnd)));
            } else if (sz == OPSZ_2) {
                print_to_buffer(buf, bufsz, sofar, "%s0x%04x", immed_prefix(),
                                (uint)((unsigned short)opnd_get_immed_int(opnd)));
            } else if (sz == OPSZ_4) {
                print_to_buffer(buf, bufsz, sofar, "%s0x%08x", immed_prefix(),
                                (uint)opnd_get_immed_int(opnd));
            } else {
                print_to_buffer(buf, bufsz, sofar, "%s0x"ZHEX64_FORMAT_STRING,
                                immed_prefix(), opnd_get_immed_int(opnd));
            }
        }
        break;
    case IMMED_FLOAT_kind:
        {
            /* need to save floating state around float printing */
            PRESERVE_FLOATING_POINT_STATE({
                uint top; uint bottom;
                const char *sign;
                double_print(opnd_get_immed_float(opnd), 6,
                             &top, &bottom, &sign);
                print_to_buffer(buf, bufsz, sofar, "%s%s%u.%.6u",
                                immed_prefix(), sign, top, bottom);
            });
            break;
        }
    case PC_kind:
        {
            app_pc target = opnd_get_pc(opnd);
            if (!print_known_pc_target(buf, bufsz, sofar, dcontext, target)) {
                print_to_buffer(buf, bufsz, sofar, "%s"PFX, immed_prefix(), target);
            }
            break;
        }
    case FAR_PC_kind:
        /* constant is selector and not a SEG_ constant */
        print_to_buffer(buf, bufsz, sofar, "0x%04x:"PFX,
                        (ushort)opnd_get_segment_selector(opnd), opnd_get_pc(opnd));
        break;
    case INSTR_kind:
        print_to_buffer(buf, bufsz, sofar, "@"PFX, opnd_get_instr(opnd));
        break;
    case FAR_INSTR_kind:
        /* constant is selector and not a SEG_ constant */
        print_to_buffer(buf, bufsz, sofar, "0x%04x:@"PFX,
                        (ushort)opnd_get_segment_selector(opnd), opnd_get_instr(opnd));
        break;
    case MEM_INSTR_kind:
        print_to_buffer(buf, bufsz, sofar, IF_X64("<re> ")"@"PFX"+%d",
                        opnd_get_instr(opnd), opnd_get_mem_instr_disp(opnd));
        break;
    case REG_kind:
        reg_disassemble(buf, bufsz, sofar, opnd_get_reg(opnd), "", "");
        break;
    case BASE_DISP_kind:
        opnd_base_disp_disassemble(buf, bufsz, sofar, dcontext, opnd);
        break;
#ifdef X64
    case REL_ADDR_kind:
        print_to_buffer(buf, bufsz, sofar, "<rel> ");
        /* fall-through */
    case ABS_ADDR_kind:
        opnd_mem_disassemble_prefix(buf, bufsz, sofar, dcontext, opnd);
        if (opnd_get_segment(opnd) != REG_NULL)
            reg_disassemble(buf, bufsz, sofar, opnd_get_segment(opnd), "", ":");
        print_to_buffer(buf, bufsz, sofar, PFX"%s", opnd_get_addr(opnd),
                        TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ? "]" : "");
        break;
#endif
    default:
        print_to_buffer(buf, bufsz, sofar, "UNKNOWN OPERAND TYPE %d", opnd.kind);
        CLIENT_ASSERT(false, "opnd_disassemble: invalid opnd type");
    }

    if (use_size_sfx) {
        switch (opnd.kind) {
            case NULL_kind:
            case IMMED_INTEGER_kind:
            case IMMED_FLOAT_kind:
            case PC_kind:
            case FAR_PC_kind:
                break;
            case REG_kind:
                if (!opnd_is_reg_partial(opnd))
                    break;
                /* fall-through */
            default: {
                const char *size_str = opnd_size_suffix_dr(opnd);
                if (size_str[0] != '\0')
                    print_to_buffer(buf, bufsz, sofar, "[%s]", size_str);
            }
        }
    }

    print_to_buffer(buf, bufsz, sofar, "%s", postop_suffix());
}

void
opnd_disassemble(dcontext_t *dcontext, opnd_t opnd, file_t outfile)
{
    char buf[MAX_OPND_DIS_SZ];
    size_t sofar = 0;
    internal_opnd_disassemble(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar, dcontext, opnd,
                              false/*don't know*/);
    /* not propagating bool return vals of print_to_buffer but should be plenty big */
    CLIENT_ASSERT(sofar < BUFFER_SIZE_ELEMENTS(buf) - 1, "internal buffer too small");
    os_write(outfile, buf, sofar);
}

size_t
opnd_disassemble_to_buffer(dcontext_t *dcontext, opnd_t opnd,
                           char *buf, size_t bufsz)

{
    size_t sofar = 0;
    internal_opnd_disassemble(buf, bufsz, &sofar, dcontext, opnd, false/*don't know*/);
    return sofar;
}

static int
print_bytes_to_buffer(char *buf, size_t bufsz, size_t *sofar INOUT,
                      byte *pc, byte *next_pc, bool valid)
{
    int sz = (int) (next_pc - pc);
    int i, extra_sz;
    if (sz > BYTES_PER_LINE) {
        extra_sz = sz - BYTES_PER_LINE;
        sz = BYTES_PER_LINE;
    } else
        extra_sz = 0;
    for (i = 0; i < sz; i++)
        print_to_buffer(buf, bufsz, sofar, " %02x", *(pc + i));
    if (!valid) {
        print_to_buffer(buf, bufsz, sofar, "...?? ");
        sz += 2;
    }
    for (i = sz; i < BYTES_PER_LINE; i++)
        print_to_buffer(buf, bufsz, sofar, "   ");
    print_to_buffer(buf, bufsz, sofar, " ");
    return extra_sz;
}

static void
print_extra_bytes_to_buffer(char *buf, size_t bufsz, size_t *sofar INOUT,
                            byte *pc, byte *next_pc, int extra_sz,
                            const char *extra_bytes_prefix)
{
    int i;
    if (extra_sz > 0) {
        print_to_buffer(buf, bufsz, sofar, "%s", extra_bytes_prefix);
        for (i = 0; i < extra_sz; i++)
            print_to_buffer(buf, bufsz, sofar, " %02x", *(pc + BYTES_PER_LINE + i));
        print_to_buffer(buf, bufsz, sofar, "\n");
    }
}

static int
print_bytes_to_file(file_t outfile, byte *pc, byte *next_pc, bool valid)
{
    char buf[MAX_PC_DIS_SZ];
    size_t sofar = 0;
    int extra_sz = print_bytes_to_buffer(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar,
                                         pc, next_pc, valid);
    CLIENT_ASSERT(sofar < BUFFER_SIZE_ELEMENTS(buf) - 1, "internal buffer too small");
    os_write(outfile, buf, sofar);
    return extra_sz;
}

static void
print_extra_bytes_to_file(file_t outfile, byte *pc, byte *next_pc, int extra_sz,
                          const char *extra_bytes_prefix)
{
    char buf[MAX_PC_DIS_SZ];
    size_t sofar = 0;
    print_extra_bytes_to_buffer(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar,
                                pc, next_pc, extra_sz, extra_bytes_prefix);
    CLIENT_ASSERT(sofar < BUFFER_SIZE_ELEMENTS(buf) - 1, "internal buffer too small");
    os_write(outfile, buf, sofar);
}

/* Disassembles the instruction at pc and prints the result to buf.
 * Returns a pointer to the pc of the next instruction.
 * Returns NULL if the instruction at pc is invalid.
 */
static byte *
internal_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                     dcontext_t *dcontext, byte *pc, byte *orig_pc,
                     bool with_pc, bool with_bytes, const char *extra_bytes_prefix)
{
    int extra_sz = 0;
    byte * next_pc;
    instr_t instr;
    bool valid = true;

    instr_init(dcontext, &instr);
    if (orig_pc != pc)
        next_pc = decode_from_copy(dcontext, pc, orig_pc, &instr);
    else
        next_pc = decode(dcontext, pc, &instr);
    if (next_pc == NULL) {
        valid = false;
        /* HACK: if decode_fast thinks it knows size use that */
        next_pc = decode_next_pc(dcontext, pc);
    }
    if (next_pc == NULL) {
        valid = false;
        /* last resort: arbitrarily pick 4 bytes */
        next_pc = pc + 4;
    }

    if (with_pc)
        print_to_buffer(buf, bufsz, sofar, "  "PFX" ", orig_pc);

    if (with_bytes) {
        extra_sz = print_bytes_to_buffer(buf, bufsz, sofar, pc, next_pc,
                                         instr_valid(&instr));
    }

    internal_instr_disassemble(buf, bufsz, sofar, dcontext, &instr);

    /* XXX: should we give caller control over whether \n or \r\n? */
    print_to_buffer(buf, bufsz, sofar, "\n");

    if (with_bytes && extra_sz > 0) {
        if (with_pc)
            print_to_buffer(buf, bufsz, sofar, IF_X64_ELSE("%21s","%13s")," ");
        print_extra_bytes_to_buffer(buf, bufsz, sofar, pc, next_pc, extra_sz,
                                    extra_bytes_prefix);
    }

    instr_free(dcontext, &instr);

    return (valid ? next_pc : NULL);
}

/* Disassembles the instruction at pc and prints the result to outfile.
 * Returns a pointer to the pc of the next instruction.
 * Returns NULL if the instruction at pc is invalid.
 */
static byte *
internal_disassemble_to_file(dcontext_t *dcontext, byte *pc, byte *orig_pc,
                             file_t outfile, bool with_pc, bool with_bytes,
                             const char *extra_bytes_prefix)
{
    char buf[MAX_PC_DIS_SZ];
    size_t sofar = 0;
    byte *next = internal_disassemble(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar,
                                      dcontext, pc, orig_pc, with_pc, with_bytes,
                                      extra_bytes_prefix);
    /* not propagating bool return vals of print_to_buffer but should be plenty big */
    CLIENT_ASSERT(sofar < BUFFER_SIZE_ELEMENTS(buf) - 1, "internal buffer too small");
    os_write(outfile, buf, sofar);
    return next;
}

/****************************************************************************
 * Exported routines
 */

/* Disassembles the instruction at pc and prints the result to outfile
 * Returns a pointer to the pc of the next instruction.
 * Returns NULL if the instruction at pc is invalid.
 */
byte *
disassemble(dcontext_t *dcontext, byte *pc, file_t outfile)
{
    return internal_disassemble_to_file(dcontext, pc, pc, outfile, true, false, "");
}

/* Disassembles a single instruction and prints its pc and
 * bytes then the disassembly.
 * Returns the pc of the next instruction.
 * If the instruction at pc is invalid, guesses a size!
 * This is b/c we internally use that feature, but I don't want to export it:
 * so this unexported routine maintains it, and we don't have to change all
 * our call sites to check for NULL.
 */
byte *
disassemble_with_bytes(dcontext_t *dcontext, byte *pc, file_t outfile)
{
    byte *next_pc = internal_disassemble_to_file(dcontext, pc, pc, outfile,
                                                 true, true, "");
    if (next_pc == NULL) {
        next_pc = decode_next_pc(dcontext, pc);
        if (next_pc == NULL)
            next_pc = pc + 4; /* guess size */
    }
    return next_pc;
}

/* Disassembles a single instruction, optionally printing its pc (if show_pc)
 * and its raw bytes (show_bytes) beforehand.
 * Returns the pc of the next instruction.
 * FIXME: vs disassemble_with_bytes -- didn't want to update all callers
 * so leaving, though should probably remove.
 * Returns NULL if the instruction at pc is invalid.
 */
byte *
disassemble_with_info(dcontext_t *dcontext, byte *pc, file_t outfile,
                      bool show_pc, bool show_bytes)
{
    return internal_disassemble_to_file(dcontext, pc, pc, outfile,
                                        show_pc, show_bytes, "");
}

/*
 * Decodes the instruction at address \p copy_pc as though
 * it were located at address \p orig_pc, and then prints the
 * instruction to file \p outfile.
 * Prior to the instruction the address \p orig_pc is printed if \p show_pc and the raw
 * bytes are printed if \p show_bytes.
 * Returns the address of the subsequent instruction after the copy at
 * \p copy_pc, or NULL if the instruction at \p copy_pc is invalid.
 */
byte *
disassemble_from_copy(dcontext_t *dcontext, byte *copy_pc, byte *orig_pc,
                      file_t outfile, bool show_pc, bool show_bytes)
{
    return internal_disassemble_to_file(dcontext, copy_pc, orig_pc, outfile,
                                        show_pc, show_bytes, "");
}

byte *
disassemble_to_buffer(dcontext_t *dcontext, byte *pc, byte *orig_pc,
                      bool show_pc, bool show_bytes, char *buf, size_t bufsz,
                      int *printed OUT)
{
    size_t sofar = 0;
    byte *next = internal_disassemble(buf, bufsz, &sofar, dcontext,
                                      pc, orig_pc, show_pc, show_bytes, "");
    if (printed != NULL)
        *printed = (int) sofar;
    return next;
}

static bool
instr_implicit_reg(instr_t *instr)
{
    /* instrs that have multiple encodings whose reg opnds are always implicit */
    switch (instr_get_opcode(instr)) {
    case OP_ins: case OP_rep_ins:
    case OP_outs: case OP_rep_outs:
    case OP_movs: case OP_rep_movs:
    case OP_stos: case OP_rep_stos:
    case OP_lods: case OP_rep_lods:
    case OP_cmps: case OP_rep_cmps: case OP_repne_cmps:
    case OP_scas: case OP_rep_scas: case OP_repne_scas:
        return true;
    }
    return false;
}

static bool
opnd_disassemble_noimplicit(char *buf, size_t bufsz, size_t *sofar INOUT,
                            dcontext_t *dcontext, instr_t *instr,
                            byte optype, opnd_t opnd, bool prev, bool multiple_encodings)
{
    switch (optype) {
    case TYPE_REG:
    case TYPE_VAR_REG:
    case TYPE_VARZ_REG:
    case TYPE_VAR_XREG:
    case TYPE_REG_EX:
    case TYPE_VAR_REG_EX:
    case TYPE_VAR_XREG_EX:
    case TYPE_VAR_REGX_EX:
        /* we do want to print implicit operands for opcode-decides-register
         * instrs like inc-reg and pop-reg, but not for say lahf, aaa, or cdq.
         */
        if (!multiple_encodings || instr_implicit_reg(instr) ||
            /* if has implicit st0 then don't print it */
            (opnd_get_reg(opnd) == REG_ST0 && instr_memory_reference_size(instr) > 0))
            return false;
        /* else fall through */
    case TYPE_A:
    case TYPE_B:
    case TYPE_C:
    case TYPE_D:
    case TYPE_E:
    case TYPE_INDIR_E:
    case TYPE_G:
    case TYPE_H:
    case TYPE_I:
    case TYPE_J:
    case TYPE_L:
    case TYPE_M:
    case TYPE_O:
    case TYPE_P:
    case TYPE_Q:
    case TYPE_R:
    case TYPE_S:
    case TYPE_V:
    case TYPE_W:
    case TYPE_P_MODRM:
    case TYPE_V_MODRM:
    case TYPE_FLOATMEM:
    case TYPE_VSIB:
    case TYPE_1:
        if (prev)
            print_to_buffer(buf, bufsz, sofar, ", ");
        internal_opnd_disassemble(buf, bufsz, sofar, dcontext, opnd, false);
        return true;
    case TYPE_X:
    case TYPE_XLAT:
    case TYPE_MASKMOVQ:
        if (opnd_get_segment(opnd) != SEG_DS) {
            /* FIXME: really we should put before opcode */
            if (prev)
                print_to_buffer(buf, bufsz, sofar, ", ");
            reg_disassemble(buf, bufsz, sofar, opnd_get_segment(opnd), "",
                            postop_suffix());
            return true;
        }
    case TYPE_Y:
    case TYPE_FLOATCONST:
    case TYPE_VAR_ADDR_XREG:
    case TYPE_INDIR_REG:
    case TYPE_INDIR_VAR_XREG:
    case TYPE_INDIR_VAR_REG:
    case TYPE_INDIR_VAR_XIREG:
    case TYPE_INDIR_VAR_XREG_OFFS_1:
    case TYPE_INDIR_VAR_XREG_OFFS_8:
    case TYPE_INDIR_VAR_XREG_OFFS_N:
    case TYPE_INDIR_VAR_XIREG_OFFS_1:
    case TYPE_INDIR_VAR_REG_OFFS_2:
    case TYPE_INDIR_VAR_XREG_SIZEx8:
    case TYPE_INDIR_VAR_REG_SIZEx2:
    case TYPE_INDIR_VAR_REG_SIZEx3x5:
        /* implicit operand */
        return false;
    default:
        CLIENT_ASSERT(false, "missing decode type"); /* catch any missing types */
    }
    return false;
}

static void
instr_disassemble_opnds_noimplicit(char *buf, size_t bufsz, size_t *sofar INOUT,
                                   dcontext_t *dcontext, instr_t *instr)
{
    /* We need to find the non-implicit operands */
    const instr_info_t *info;
    int i, num;
    byte optype;
    /* avoid duplicate on ALU: only happens w/ 2dst, 3srcs */
    byte optype_already[3] = {TYPE_NONE, TYPE_NONE, TYPE_NONE};
    opnd_t opnd;
    bool prev = false, multiple_encodings = false;

    info = instr_get_instr_info(instr);
    if (info != NULL && get_next_instr_info(info) != NULL &&
        !TESTALL(HAS_EXTRA_OPERANDS | EXTRAS_IN_CODE_FIELD, info->flags))
        multiple_encodings = true;

    info = get_encoding_info(instr);
    if (info == NULL) {
        print_to_buffer(buf, bufsz, sofar, "<INVALID>");
        return;
    }
    num = TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ?
        instr_num_dsts(instr) : instr_num_srcs(instr);
    for (i=0; i<num; i++) {
        bool printing;
        opnd = TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ?
            instr_get_dst(instr, i) : instr_get_src(instr, i);
        optype = instr_info_opnd_type(info, !TEST(DR_DISASM_INTEL,
                                                  DYNAMO_OPTION(disasm_mask)), i);
        printing = opnd_disassemble_noimplicit(buf, bufsz, sofar, dcontext,
                                               instr, optype, opnd, prev,
                                               multiple_encodings);
        /* w/o the "printing" check we suppress "push esp" => "push" */
        if (printing && i < 3)
            optype_already[i] = optype;
        prev = printing || prev;
    }
    num = TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ?
        instr_num_srcs(instr) : instr_num_dsts(instr);
    for (i=0; i<num; i++) {
        opnd = TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask)) ?
            instr_get_src(instr, i) : instr_get_dst(instr, i);
        optype = instr_info_opnd_type(info, TEST(DR_DISASM_INTEL,
                                                 DYNAMO_OPTION(disasm_mask)), i);
        /* PR 312458: still not matching Intel-style tools like windbg or udis86:
         * we need to suppress certain implicit operands, such as:
         * - div dx, ax
         * - imul ax
         * - idiv edx, eax
         * - in al
         */

        /* Don't re-do src==dst of ALU ops */
        if ((optype != optype_already[0] && optype != optype_already[1] &&
             optype != optype_already[2]) ||
            /* Don't suppress 2nd of st* if FP ALU */
            (i == 0 && opnd_is_reg(opnd) && reg_is_fp(opnd_get_reg(opnd)))) {
            prev = opnd_disassemble_noimplicit(buf, bufsz, sofar, dcontext,
                                               instr, optype, opnd, prev,
                                               multiple_encodings)
                || prev;
        }
    }
}

static const char *
instr_opcode_name(instr_t *instr, const instr_info_t *info)
{
    if (TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
        switch (instr_get_opcode(instr)) {
        /* remove "l" prefix */
        case OP_call_far: return "call";
        case OP_call_far_ind: return "call";
        case OP_jmp_far: return "jmp";
        case OP_jmp_far_ind: return "jmp";
        case OP_ret_far: return "retf";
        }
    }
#ifdef X64
    if (!instr_get_x86_mode(instr)) {
        if (instr_get_opcode(instr) == OP_jecxz &&
            reg_is_pointer_sized(opnd_get_reg(instr_get_src(instr, 1))))
            return "jrcxz";
        else if (instr_get_opcode(instr) == OP_pextrd &&
                 opnd_get_size(instr_get_dst(instr, 0)) == OPSZ_PTR)
            return "pextrq";
        else if (instr_get_opcode(instr) == OP_vpextrd &&
                 opnd_get_size(instr_get_dst(instr, 0)) == OPSZ_PTR)
            return "vpextrq";
        else if (instr_get_opcode(instr) == OP_pinsrd &&
                 opnd_get_size(instr_get_src(instr, 0)) == OPSZ_PTR)
            return "pinsrq";
        else if (instr_get_opcode(instr) == OP_vpinsrd &&
                 opnd_get_size(instr_get_src(instr, 0)) == OPSZ_PTR)
            return "vpinsrq";
    }
#endif
    return info->name;
}

static const char *
instr_opcode_name_suffix(instr_t *instr)
{
    if (TESTANY(DR_DISASM_INTEL|DR_DISASM_ATT, DYNAMO_OPTION(disasm_mask))) {
        /* add "b" or "d" suffix */
        switch (instr_get_opcode(instr)) {
        case OP_pushf: case OP_popf:
        case OP_xlat:
        case OP_ins: case OP_rep_ins:
        case OP_outs: case OP_rep_outs:
        case OP_movs: case OP_rep_movs:
        case OP_stos: case OP_rep_stos:
        case OP_lods: case OP_rep_lods:
        case OP_cmps: case OP_rep_cmps: case OP_repne_cmps:
        case OP_scas: case OP_rep_scas: case OP_repne_scas: {
                uint sz = instr_memory_reference_size(instr);
                if (sz == 1)
                    return "b";
                else if (sz == 2)
                    return "w";
                else if (sz == 4)
                    return "d";
                else if (sz == 8)
                    return "q";
        }
        case OP_pusha:
        case OP_popa: {
                uint sz = instr_memory_reference_size(instr);
                if (sz == 16)
                    return "w";
                else if (sz == 32)
                    return "d";
        }
        case OP_iret: {
                uint sz = instr_memory_reference_size(instr);
                if (sz == 6)
                    return "w";
                else if (sz == 12)
                    return "d";
                else if (sz == 40)
                    return "q";
        }
        }
    }
    if (TEST(DR_DISASM_ATT, DYNAMO_OPTION(disasm_mask)) && instr_operands_valid(instr)) {
        /* XXX: requiring both src and dst.  Ideally we'd wait until we
         * see if there is a register or in some cases an immed operand
         * and then go back and add the suffix.  This will do for now.
         */
        if (instr_num_srcs(instr) > 0 && !opnd_is_reg(instr_get_src(instr, 0)) &&
            instr_num_dsts(instr) > 0 && !opnd_is_reg(instr_get_dst(instr, 0))) {
            uint sz = instr_memory_reference_size(instr);
            if (sz == 1)
                return "b";
            else if (sz == 2)
                return "w";
            else if (sz == 4)
                return "l";
            else if (sz == 8)
                return "q";
        }
    }
    return "";
}

static bool
instr_needs_opnd_size_sfx(instr_t *instr)
{
#ifdef DISASM_SUFFIX_ONLY_ON_MISMATCH /* disabled: see below */
    opnd_t src, dst;
    if (TEST(DR_DISASM_NO_OPND_SIZE, DYNAMO_OPTION(disasm_mask)))
        return false;
    /* We really only care about the primary src and primary dst */
    if (instr_num_srcs(instr) == 0 ||
        instr_num_dsts(instr) == 0)
        return false;
    src = instr_get_src(instr, 0);
    /* Avoid opcodes that have a 1-byte immed but all other operands
     * the same size from triggering suffixes
     */
    if (opnd_is_immed(src) && instr_num_srcs(instr) > 1)
        return false;
    dst = instr_get_dst(instr, 0);
    return (opnd_get_size(src) != opnd_get_size(dst) ||
            /* We haven't sign-extended yet -- if we did maybe we wouldn't
             * need this.  Good to show size on mov of immed into memory.
             */
            opnd_is_immed_int(src) ||
            opnd_is_reg_partial(src) ||
            opnd_is_reg_partial(dst));
#else
    /* Originally I tried only showing the sizes when they mismatch or
     * can't be inferred (code above), but that gets a little tricky,
     * and IMHO it's nice to see the size of all memory operands.  We
     * never print for immeds or non-partial regs, so we can just set
     * to true for all instructions.
     */
    if (TEST(DR_DISASM_NO_OPND_SIZE, DYNAMO_OPTION(disasm_mask)))
        return false;
    return true;
#endif
}

static void
sign_extend_immed(instr_t *instr, int srcnum, opnd_t *src)
{
    opnd_size_t opsz = OPSZ_NA;
    bool resize = true;
    if (opnd_is_immed_int(*src)) {
        /* PR 327775: force operand to sign-extend if all other operands
         * are of a larger and identical-to-each-other size (since we
         * don't want to extend immeds used in stores) and are not
         * multimedia registers (since immeds there are always indices).
         */
        int j;
        for (j=0; j<instr_num_srcs(instr); j++) {
            if (j != srcnum) {
                if (opnd_is_reg(instr_get_src(instr, j)) &&
                    !reg_is_gpr(opnd_get_reg(instr_get_src(instr, j)))) {
                    resize = false;
                    break;
                }
                if (opsz == OPSZ_NA)
                    opsz = opnd_get_size(instr_get_src(instr, j));
                else if (opsz != opnd_get_size(instr_get_src(instr, j))) {
                    resize = false;
                    break;
                }
            }
        }
        for (j=0; j<instr_num_dsts(instr); j++) {
            if (opnd_is_reg(instr_get_dst(instr, j)) &&
                !reg_is_gpr(opnd_get_reg(instr_get_dst(instr, j)))) {
                resize = false;
                break;
            }
            if (opsz == OPSZ_NA)
                opsz = opnd_get_size(instr_get_dst(instr, j));
            else if (opsz != opnd_get_size(instr_get_dst(instr, j))) {
                resize = false;
                break;
            }
        }
        if (resize && opsz != OPSZ_NA && !instr_is_interrupt(instr))
            opnd_set_size(src, opsz);
    }
}

/*
 * Prints the instruction instr to file outfile.
 * Does not print addr16 or data16 prefixes for other than just-decoded instrs,
 * and does not check that the instruction has a valid encoding.
 * Prints each operand with leading zeros indicating the size.
 */
static void
internal_instr_disassemble(char *buf, size_t bufsz, size_t *sofar INOUT,
                           dcontext_t *dcontext, instr_t *instr)
{
    int i, sz;
    const instr_info_t * info;
    const char * name;
    int name_width = 6;
    bool use_size_sfx = false;

    if (!instr_valid(instr)) {
        print_to_buffer(buf, bufsz, sofar, "<INVALID>");
        return;
    } else if (instr_is_label(instr)) {
        print_to_buffer(buf, bufsz, sofar, "<label>");
        return;
    } else if (instr_opcode_valid(instr)) {
        info = instr_get_instr_info(instr);
        name = instr_opcode_name(instr, info);
    } else
        name = "<RAW>";

    if (TEST(PREFIX_XACQUIRE, instr->prefixes))
        print_to_buffer(buf, bufsz, sofar, "xacquire ");
    if (TEST(PREFIX_XRELEASE, instr->prefixes))
        print_to_buffer(buf, bufsz, sofar, "xrelease ");
    if (TEST(PREFIX_LOCK, instr->prefixes))
        print_to_buffer(buf, bufsz, sofar, "lock ");
    /* Note that we do not try to figure out data16 or addr16 prefixes
     * if they are not already set from a recent decode;
     * we don't want to enforce a valid encoding at this point.
     *
     * To walk the operands and find addr16, we'd need to look for
     * opnd_is_disp_short_addr() as well as push/pop of REG_SP, jecxz/loop* of
     * REG_CX, or string ops, maskmov*, or xlat of REG_DI or REG_SI.
     * For data16, we'd look for 16-bit reg or OPSZ_2 immed or base_disp.
     */
    if (!TEST(DR_DISASM_INTEL, DYNAMO_OPTION(disasm_mask))) {
        if (TEST(PREFIX_DATA, instr->prefixes))
            print_to_buffer(buf, bufsz, sofar, "data16 ");
        if (TEST(PREFIX_ADDR, instr->prefixes)) {
            print_to_buffer(buf, bufsz, sofar,
                            X64_MODE_DC(dcontext) ? "addr32 " : "addr16 ");
        }
#if 0 /* disabling for PR 256226 */
        if (TEST(PREFIX_REX_W, instr->prefixes))
            print_to_buffer(buf, bufsz, sofar, "rex.w ");
#endif
    }

    print_to_buffer(buf, bufsz, sofar, "%s%s", name, instr_opcode_name_suffix(instr));

    if (TEST(PREFIX_JCC_TAKEN, instr->prefixes)) {
        print_to_buffer(buf, bufsz, sofar, ",pt ");
        name_width -= 3;
    } else if (TEST(PREFIX_JCC_NOT_TAKEN, instr->prefixes)) {
        print_to_buffer(buf, bufsz, sofar, ",pn ");
        name_width -= 3;
    } else
        print_to_buffer(buf, bufsz, sofar, " ");

    IF_X64(CLIENT_ASSERT(CHECK_TRUNCATE_TYPE_int(strlen(name)),
                         "instr_disassemble: internal truncation error"));
    sz = (int) strlen(name) + (int) strlen(instr_opcode_name_suffix(instr));
    for (i=sz; i<name_width; i++)
        print_to_buffer(buf, bufsz, sofar, " ");

    /* operands */
    if (!instr_operands_valid(instr)) {
        /* we could decode the raw bits, but caller should if they want that */
        byte *raw = instr_get_raw_bits(instr);
        uint len = instr_length(dcontext, instr);
        byte *b;
        print_to_buffer(buf, bufsz, sofar, "<raw "PFX"-"PFX" ==", raw, raw + len);
        for (b = raw; b < raw + len && b < raw + 9; b++)
            print_to_buffer(buf, bufsz, sofar, " %02x", *b);
        if (len > 9)
            print_to_buffer(buf, bufsz, sofar, " ...");
        print_to_buffer(buf, bufsz, sofar, ">");
        return;
    }

    if (TESTANY(DR_DISASM_INTEL|DR_DISASM_ATT, DYNAMO_OPTION(disasm_mask))) {
        instr_disassemble_opnds_noimplicit(buf, bufsz, sofar, dcontext, instr);
        return;
    }

    use_size_sfx = instr_needs_opnd_size_sfx(instr);

    for (i=0; i<instr_num_srcs(instr); i++) {
        opnd_t src = instr_get_src(instr, i);
        sign_extend_immed(instr, i, &src);
        internal_opnd_disassemble(buf, bufsz, sofar, dcontext, src, use_size_sfx);
    }
    if (instr_num_dsts(instr) > 0) {
        print_to_buffer(buf, bufsz, sofar, "-> ");
        for (i=0; i<instr_num_dsts(instr); i++) {
            internal_opnd_disassemble(buf, bufsz, sofar, dcontext,
                                      instr_get_dst(instr, i), use_size_sfx);
        }
    }
}

/*
 * Prints the instruction instr to file outfile.
 * Does not print addr16 or data16 prefixes for other than just-decoded instrs,
 * and does not check that the instruction has a valid encoding.
 * Prints each operand with leading zeros indicating the size.
 */
void
instr_disassemble(dcontext_t *dcontext, instr_t *instr, file_t outfile)
{
    char buf[MAX_INSTR_DIS_SZ];
    size_t sofar = 0;
    internal_instr_disassemble(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar, dcontext, instr);
    /* not propagating bool return vals of print_to_buffer but should be plenty big */
    CLIENT_ASSERT(sofar < BUFFER_SIZE_ELEMENTS(buf) - 1, "internal buffer too small");
    os_write(outfile, buf, sofar);
}

/*
 * Prints the instruction \p instr to the buffer \p buf.
 * Always null-terminates, and will not print more than \p bufsz characters,
 * which includes the final null character.
 * Returns the number of characters printed, not including the final null.
 *
 * Does not print address-size or data-size prefixes for other than
 * just-decoded instrs, and does not check that the instruction has a
 * valid encoding.  Prints each operand with leading zeros indicating
 * the size.
 * The default is to use AT&T-style syntax, unless the \ref op_syntax_intel
 * "-syntax_intel" runtime option is specified.
 */
size_t
instr_disassemble_to_buffer(dcontext_t *dcontext, instr_t *instr,
                            char *buf, size_t bufsz)
{
    size_t sofar = 0;
    internal_instr_disassemble(buf, bufsz, &sofar, dcontext, instr);
    return sofar;
}

#ifndef STANDALONE_DECODER
static inline const char*
exit_stub_type_desc(dcontext_t *dcontext, fragment_t *f, linkstub_t *l)
{
    if (LINKSTUB_DIRECT(l->flags)) {
        if (EXIT_IS_CALL(l->flags))
            return "call";
        if (EXIT_IS_JMP(l->flags))
            return "jmp/jcc";
        return "fall-through/speculated/IAT";
        /* FIXME: mark these appropriately */
    } else {
        CLIENT_ASSERT(LINKSTUB_INDIRECT(l->flags), "invalid exit stub");
        if (TEST(LINK_RETURN, l->flags))
            return "ret";
        if (EXIT_IS_CALL(l->flags))
            return "indcall";
        if (TEST(LINK_JMP, l->flags)) /* JMP or IND_JMP_PLT */
            return "indjmp";
#ifdef WINDOWS
        if (is_shared_syscall_routine(dcontext, EXIT_TARGET_TAG(dcontext, f, l)))
            return "shared_syscall";
#endif
    }
    CLIENT_ASSERT(false, "unknown exit stub type");
    return "<unknown>";
}

/* Disassemble and pretty-print the generated code for fragment f.
 * Header and body control whether header info and code itself are printed
 */
static void
common_disassemble_fragment(dcontext_t *dcontext,
                            fragment_t *f_in, file_t outfile, bool header, bool body)
{
    cache_pc entry_pc, prefix_pc;
    cache_pc pc;
    cache_pc body_end_pc;
    cache_pc end_pc;
    linkstub_t *l;
    int exit_num = 0;
#ifdef PROFILE_RDTSC
    cache_pc profile_end = 0;
#endif
    bool alloc;
    fragment_t *f = f_in;

    if (header) {
#ifdef DEBUG
        print_file(outfile, "Fragment %d, tag "PFX", flags 0x%x, %s%s%s%ssize %d%s%s:\n",
                   f->id,
#else
        print_file(outfile, "Fragment tag "PFX", flags 0x%x, %s%s%s%ssize %d%s%s:\n",
#endif
                   f->tag, f->flags,
                   IF_X64_ELSE(FRAG_IS_32(f->flags) ? "32-bit, " : "", ""),
                   TEST(FRAG_COARSE_GRAIN, f->flags) ? "coarse, " : "",
                   (TEST(FRAG_SHARED, f->flags) ? "shared, " :
                    (SHARED_FRAGMENTS_ENABLED() ?
                     (TEST(FRAG_TEMP_PRIVATE, f->flags) ? "private temp, " : "private, ") : "")),
                   (TEST( FRAG_IS_TRACE, f->flags)) ? "trace, " :
                   (TEST(FRAG_IS_TRACE_HEAD, f->flags)) ? "tracehead, " : "",
                   f->size,
                   (TEST(FRAG_CANNOT_BE_TRACE, f->flags)) ?", cannot be trace":"",
                   (TEST(FRAG_MUST_END_TRACE, f->flags)) ?", must end trace":"",
                   (TEST(FRAG_CANNOT_DELETE, f->flags)) ?", cannot delete":"");

        DOLOG(2, LOG_SYMBOLS, { /* FIXME: this affects non-logging uses... dump_traces, etc. */
            char symbolbuf[MAXIMUM_SYMBOL_LENGTH];
            print_symbolic_address(f->tag, symbolbuf, sizeof(symbolbuf), false);
            print_file(outfile, "\t%s\n", symbolbuf);
        });
    }

    if (!body)
        return;

    if (body && TEST(FRAG_FAKE, f->flags)) {
        alloc = true;
        f = fragment_recreate_with_linkstubs(dcontext, f_in);
    } else {
        alloc = false;
    }
    end_pc = f->start_pc + f->size;
    body_end_pc = fragment_body_end_pc(dcontext, f);
    entry_pc = FCACHE_ENTRY_PC(f);
    prefix_pc = FCACHE_PREFIX_ENTRY_PC(f);
    pc = FCACHE_IBT_ENTRY_PC(f);
    if (pc != entry_pc) {
        if (pc != prefix_pc) {
            /* indirect branch target prefix exists */
            print_file(outfile, "  -------- indirect branch target entry: --------\n");
        }
        while (pc < entry_pc) {
            if (pc == prefix_pc) {
                print_file(outfile, "  -------- prefix entry: --------\n");
            }
            pc = (cache_pc) disassemble_with_bytes(dcontext, (byte *)pc, outfile);
        }
        print_file(outfile, "  -------- normal entry: --------\n");
    }

    CLIENT_ASSERT(pc == entry_pc, "disassemble_fragment: invalid prefix");

#ifdef PROFILE_RDTSC
    if (dynamo_options.profile_times && (f->flags & FRAG_IS_TRACE) != 0) {
        int sz = profile_call_size();
        profile_end = pc + sz;
        if (stats->loglevel < 3) {
            /* don't print profile stuff to save space */
            print_file(outfile, "  "PFX"..."PFX" = profile code\n",
                       pc, (pc+sz-1));
            pc += sz;
        } else {
            /* print profile stuff, but delineate it: */
            print_file(outfile, "  -------- profile call: --------\n");
        }
    }
#endif

    while (pc < body_end_pc) {
        pc = (cache_pc) disassemble_with_bytes(dcontext, (byte *)pc, outfile);
#ifdef PROFILE_RDTSC
        if (dynamo_options.profile_times &&
            (f->flags & FRAG_IS_TRACE) != 0 &&
            pc == profile_end) {
            print_file(outfile, "  -------- end profile call -----\n");
        }
#endif
    }

    for (l = FRAGMENT_EXIT_STUBS(f); l; l = LINKSTUB_NEXT_EXIT(l)) {
        cache_pc next_stop_pc;
        linkstub_t *nxt;
        /* store fragment pc since we don't want to walk forward in fragment */
        cache_pc frag_pc = pc;
        print_file(outfile, "  -------- exit stub %d: -------- <target: "PFX"> type: %s\n",
                   exit_num, EXIT_TARGET_TAG(dcontext, f, l),
                   exit_stub_type_desc(dcontext, f, l));
        if (!EXIT_HAS_LOCAL_STUB(l->flags, f->flags)) {
            if (EXIT_STUB_PC(dcontext, f, l) != NULL) {
                pc = EXIT_STUB_PC(dcontext, f, l);
                next_stop_pc = pc + linkstub_size(dcontext, f, l);
            } else if (TEST(FRAG_COARSE_GRAIN, f->flags)) {
                cache_pc cti_pc = EXIT_CTI_PC(f, l);
                if (cti_pc == end_pc) {
                    /* must be elided final jmp */
                    print_file(outfile, "  <no final jmp since elided>\n");
                    print_file(outfile, "  <no stub since linked and frozen>\n");
                    CLIENT_ASSERT(pc == end_pc, "disassemble_fragment: invalid end");
                    next_stop_pc = end_pc;
                } else {
                    pc = entrance_stub_from_cti(cti_pc);
                    if (coarse_is_entrance_stub(pc)) {
                        next_stop_pc = pc + linkstub_size(dcontext, f, l);
                    } else {
                        CLIENT_ASSERT(in_fcache(pc),
                                      "disassemble_fragment: invalid exit stub");
                        print_file(outfile, "  <no stub since linked and frozen>\n");
                        next_stop_pc = pc;
                    }
                }
            } else {
                if (TEST(LINK_SEPARATE_STUB, l->flags))
                    print_file(outfile, "  <no stub created since linked>\n");
                else if (!EXIT_HAS_STUB(l->flags, f->flags))
                    print_file(outfile, "  <no stub needed: -no_indirect_stubs>\n");
                else
                    CLIENT_ASSERT(false, "disassemble_fragment: invalid exit stub");
                next_stop_pc = pc;
            }
        } else {
            for (nxt = LINKSTUB_NEXT_EXIT(l); nxt != NULL; nxt = LINKSTUB_NEXT_EXIT(nxt)) {
                if (EXIT_HAS_LOCAL_STUB(nxt->flags, f->flags))
                    break;
            }
            if (nxt != NULL)
                next_stop_pc = EXIT_STUB_PC(dcontext, f, nxt);
            else
                next_stop_pc =  pc + linkstub_size(dcontext, f, l);
            CLIENT_ASSERT(next_stop_pc != NULL, "disassemble_fragment: invalid stubs");
        }
        while (pc < next_stop_pc) {
            pc = (cache_pc) disassemble_with_bytes(dcontext, (byte *)pc, outfile);
        }
        /* point pc back at tail of fragment code if it was off in separate stub land */
        if (TEST(LINK_SEPARATE_STUB, l->flags))
            pc = frag_pc;
        exit_num++;
    }

    if (TEST(FRAG_SELFMOD_SANDBOXED, f->flags)) {
        DOSTATS({ /* skip stored sz */ end_pc -= sizeof(uint); });
        print_file(outfile, "  -------- original code (from "PFX"-"PFX") -------- \n",
                   f->tag, (f->tag + (end_pc - pc)));
        while (pc < end_pc) {
            pc = (cache_pc) disassemble_with_bytes(dcontext, (byte *)pc, outfile);
        }
    }

    if (alloc)
        fragment_free(dcontext, f);
}

#ifdef DEBUG
void
disassemble_fragment(dcontext_t *dcontext, fragment_t *f, bool just_header)
{
    if ((stats->logmask & LOG_EMIT) != 0) {
        common_disassemble_fragment(dcontext, f, THREAD,
                                    true, !just_header);
        if (!just_header)
            LOG(THREAD, LOG_EMIT, 1, "\n");
    }
}
#endif /* DEBUG */

void
disassemble_fragment_header(dcontext_t *dcontext, fragment_t *f, file_t outfile)
{
    common_disassemble_fragment(dcontext, f, outfile, true, false);
}

void
disassemble_fragment_body(dcontext_t *dcontext, fragment_t *f, file_t outfile)
{
    common_disassemble_fragment(dcontext, f, outfile, false, true);
}

void
disassemble_app_bb(dcontext_t *dcontext, app_pc tag, file_t outfile)
{
    instrlist_t *ilist = build_app_bb_ilist(dcontext, tag, outfile);
    instrlist_clear_and_destroy(dcontext, ilist);
}

#endif /* !STANDALONE_DECODER */
/***************************************************************************/

/* Two entry points to the disassembly routines: */

void
instrlist_disassemble(dcontext_t *dcontext,
                      app_pc tag, instrlist_t *ilist, file_t outfile)
{
    int len, sz;
    instr_t *instr;
    byte *addr;
    byte *next_addr;
    byte bytes[64];     /* scratch array for encoding instrs */
    int level;
    int offs = 0;
    /* we want to print out the decode level each instr is at, so we have to
     * do a little work
     */

    print_file(outfile, "TAG  "PFX"\n", tag);

    for (instr = instrlist_first(ilist); instr; instr = instr_get_next(instr)) {
        DOLOG(5, LOG_ALL, {
            if (instr_raw_bits_valid(instr)) {
                print_file(outfile, " <raw "PFX"-"PFX">::\n",
                           instr_get_raw_bits(instr),
                           instr_get_raw_bits(instr) + instr_length(dcontext, instr));
            }
            if (instr_get_translation(instr) != NULL) {
                print_file(outfile, " <translation "PFX">::\n",
                           instr_get_translation(instr));
            }
        });
        if (instr_needs_encoding(instr)) {
            byte *nxt_pc;
            level = 4;
            /* encode instr and then output as BINARY */
            nxt_pc = instr_encode_ignore_reachability(dcontext, instr, bytes);
            ASSERT(nxt_pc != NULL);
            len = (int) (nxt_pc - bytes);
            addr = bytes;
            CLIENT_ASSERT(len < 64, "instrlist_disassemble: too-long instr");
        } else {
            addr = instr_get_raw_bits(instr);
            len = instr_length(dcontext, instr);
            if (instr_operands_valid(instr))
                level = 3;
            else if (instr_opcode_valid(instr))
                level = 2;
            else if (decode_sizeof(dcontext, addr, NULL _IF_X64(NULL)) == len)
                level = 1;
            else
                level = 0;
        }

        /* Print out individual instructions.  Remember that multiple
         * instructions may be packed into a single instr.
         */
        if (level > 3) {
            /* for L4 we want to see instr targets and don't care
             * as much about raw bytes
             */
            int extra_sz;
            print_file(outfile, " +%-4d %c%d @"PFX" ",
                       offs, instr_is_app(instr) ? 'L' : 'm', level, instr);
            extra_sz = print_bytes_to_file(outfile, addr, addr+len, instr_valid(instr));
            instr_disassemble(dcontext, instr, outfile);
            print_file(outfile, "\n");
            if (extra_sz > 0) {
                print_file(outfile, IF_X64_ELSE("%30s","%22s"), " " );
                print_extra_bytes_to_file(outfile, addr, addr+len, extra_sz, "");
            }
            offs += len;
            len = 0; /* skip loop */
        }
        while (len) {
            print_file(outfile, " +%-4d %c%d "IF_X64_ELSE("%20s","%12s"),
                       offs, instr_is_app(instr) ? 'L' : 'm', level, " ");
            next_addr = internal_disassemble_to_file
                (dcontext, addr, addr, outfile, false, true,
                 IF_X64_ELSE("                               ","                       "));
            if (next_addr == NULL)
                break;
            sz = (int) (next_addr - addr);
            CLIENT_ASSERT(sz <= len, "instrlist_disassemble: invalid length");
            len -= sz;
            addr += sz;
            offs += sz;
        }
        DOLOG(5, LOG_ALL, {
            print_file(outfile, "---- multi-instr boundary ----\n");
        });

#ifdef CUSTOM_EXIT_STUBS
        /* custom exit stub? */
        if (instr_is_exit_cti(instr) && instr_is_app(instr)) {
            instrlist_t * custom = instr_exit_stub_code(instr);
            if (custom != NULL) {
                print_file(outfile, "\t=> custom exit stub code:\n");
                instrlist_disassemble(dcontext, instr_get_branch_target_pc(instr),
                                      custom, outfile);
            }
        }
#endif

    }

    print_file(outfile, "END "PFX"\n\n", tag);
}

#endif /* INTERNAL || CLIENT_INTERFACE */


/***************************************************************************/
#ifndef STANDALONE_DECODER

static void
callstack_dump_module_info(char *buf, size_t bufsz, size_t *sofar,
                           app_pc pc, uint flags)
{
    if (TEST(CALLSTACK_MODULE_INFO, flags)) {
        module_area_t *ma;
        os_get_module_info_lock();
        ma = module_pc_lookup(pc);
        if (ma != NULL) {
            print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                            "mod=\""PFX"\" offs=\""PFX"\" " : " <%s+"PIFX">",
                            TEST(CALLSTACK_MODULE_PATH, flags) ? ma->full_path :
                            GET_MODULE_NAME(&ma->names), pc - ma->start);
        }
        os_get_module_info_unlock();
    }
}

static void
internal_dump_callstack_to_buffer(char *buf, size_t bufsz, size_t *sofar,
                                  app_pc cur_pc, app_pc ebp, uint flags)
{
    ptr_uint_t *pc = (ptr_uint_t *) ebp;
    int num = 0;
    LOG_DECLARE(char symbolbuf[MAXIMUM_SYMBOL_LENGTH];)
    const char *symbol_name = "";

    if (TEST(CALLSTACK_ADD_HEADER, flags)) {
        print_to_buffer(buf, bufsz, sofar,
                        TEST(CALLSTACK_USE_XML, flags) ?
                        "\t<call-stack>\n" : "Call stack:\n");
    }

    if (cur_pc != NULL) {
        DOLOG(1, LOG_SYMBOLS, {
            print_symbolic_address(cur_pc, symbolbuf, sizeof(symbolbuf), false);
            symbol_name = symbolbuf;
        });
        print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                        "\t<current_pc=\""PFX"\" name=\"%s\" " :
                        "\t"PFX" %s ",
                        cur_pc, symbol_name);
        callstack_dump_module_info(buf, bufsz, sofar, cur_pc, flags);
        print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                        "/>\n" : "\n");
    }

    while (pc != NULL &&
           is_readable_without_exception_query_os((byte *)pc, 8)) {
        DOLOG(1, LOG_SYMBOLS, {
            print_symbolic_address((app_pc)*(pc+1), symbolbuf, sizeof(symbolbuf), false);
            symbol_name = symbolbuf;
        });

        print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                        "\t\t" : "\t");
        if (TEST(CALLSTACK_FRAME_PTR, flags)) {
            print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                            "<frame ptr=\""PFX"\" parent=\""PFX"\" " :
                            "frame ptr "PFX" => parent "PFX", ",
                            pc, *pc);
        }
        print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                        "ret=\""PFX"\" name=\"%s\" " : PFX" %s ",
                        *(pc+1), symbol_name);
        callstack_dump_module_info(buf, bufsz, sofar, (app_pc) *(pc+1), flags);
        print_to_buffer(buf, bufsz, sofar, TEST(CALLSTACK_USE_XML, flags) ?
                        "/>\n" : "\n");

        num++;
        /* yes I've seen weird recursive cases before */
        if (pc == (ptr_uint_t *) *pc || num > 100)
            break;
        pc = (ptr_uint_t *) *pc;
    }

    if (TESTALL(CALLSTACK_USE_XML | CALLSTACK_ADD_HEADER, flags))
        print_to_buffer(buf, bufsz, sofar, "\t</call-stack>\n");
}

static void
internal_dump_callstack(app_pc cur_pc, app_pc ebp, file_t outfile, bool dump_xml,
                        bool header)
{
    char buf[MAX_LOG_LENGTH];
    size_t sofar = 0;
    internal_dump_callstack_to_buffer(buf, BUFFER_SIZE_ELEMENTS(buf), &sofar,
                                      cur_pc, ebp,
                                      CALLSTACK_ADD_HEADER |
                                      CALLSTACK_FRAME_PTR |
                                      (dump_xml ? CALLSTACK_USE_XML : 0));
    print_file(outfile, "%s", buf);
}

void
dump_callstack(app_pc pc, app_pc ebp, file_t outfile, bool dump_xml)
{
    internal_dump_callstack(pc, ebp, outfile, dump_xml, true/*header*/);
}

void
dump_callstack_to_buffer(char *buf, size_t bufsz, size_t *sofar,
                         app_pc pc, app_pc ebp, uint flags)
{
    internal_dump_callstack_to_buffer(buf, bufsz, sofar,
                                      pc, ebp, flags);
}

#ifdef DEBUG
void
dump_mcontext_callstack(dcontext_t *dcontext)
{
    LOG(THREAD, LOG_ALL, 1, "Call stack:\n");
    internal_dump_callstack((app_pc)get_mcontext(dcontext)->pc,
                            (app_pc)get_mcontext(dcontext)->xbp, THREAD,
                            DUMP_NOT_XML, false/*!header*/);
}
#endif

void
dump_dr_callstack(file_t outfile)
{
    /* Since we're in DR we can't just clobber the saved app fields --
     * so we save them first
     */
    app_pc our_ebp = 0;
    GET_FRAME_PTR(our_ebp);
    LOG(outfile, LOG_ALL, 1, "DynamoRIO call stack:\n");
    internal_dump_callstack(NULL /* don't care about cur pc */, our_ebp,
                            outfile, DUMP_NOT_XML, false/*!header*/);
}

#endif /* !STANDALONE_DECODER */
/***************************************************************************/
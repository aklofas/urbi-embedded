/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_reg.c — the register discipline, function and block states,
 * upvalue resolution, and the instruction / constant / line / site
 * streams every arm writes through.  See uemit_internal.h for the
 * discipline itself. */

#include "emit/uemit_internal.h"
#include "emit/uintern.h"
#include "chunk/uchunk.h"

#include <stddef.h>
#include <stdint.h>

/* --- errors and interning -------------------------------------------------- */

bool uemit_fail(UEmitter *e, UEmitError code) {
    if (e->error == EMIT_OK) e->error = code;
    return false;
}

const char *uemit_intern(UEmitter *e, const char *bytes, int n) {
    const char *s = ustr_intern(e->vm, bytes, (size_t)n);
    if (s == NULL) (void)uemit_fail(e, EMIT_OOM);
    return s;
}

/* Grow `*data` (elements of `elem`) to at least `need` through the module
 * allocator, doubling from 16.  Latches EMIT_OOM on failure. */
static bool grow(UEmitter *e, void **data, size_t *cap, size_t need, size_t elem) {
    if (*cap >= need) return true;
    size_t target = (*cap == 0U) ? 16U : *cap;
    while (target < need) target *= 2U;
    void *fresh = emit_alloc_for(e->module)(*data, target * elem, e->module->alloc_ud);
    if (fresh == NULL) return uemit_fail(e, EMIT_OOM);
    *data = fresh;
    *cap = target;
    return true;
}

/* --- registers ----------------------------------------------------------- */

uint8_t ureg_alloc(UEmitter *e) {
    UFuncState *fs = e->fs;
    if (fs->freereg >= (uint8_t)(UFS_MAX_REGS - 1)) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "expression needs more than %d registers", UFS_MAX_REGS - 1);
        (void)uemit_fail(e, EMIT_REG_EXHAUSTED);
        return fs->freereg;
    }
    uint8_t r = fs->freereg++;
    if (r > fs->max_reg) fs->max_reg = r;
    return r;
}

uint8_t ureg_top(const UEmitter *e) {
    const UFuncState *fs = e->fs;
    return fs->nactvar == 0 ? 0U : (uint8_t)(fs->locals[fs->nactvar - 1].reg + 1U);
}

void ureg_free_to(UEmitter *e, uint8_t r) {
    uint8_t top = ureg_top(e);
    e->fs->freereg = (r < top) ? top : r;
}

/* Index of a new local holding register `reg`; -1 on overflow. */
static int local_push(UEmitter *e, const char *name, uint8_t reg, uint8_t flags) {
    UFuncState *fs = e->fs;
    if (fs->nactvar >= UFS_MAX_LOCALS) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "too many local variables in function (max %d)", UFS_MAX_LOCALS);
        (void)uemit_fail(e, EMIT_REG_EXHAUSTED);
        return -1;
    }
    ULocal *l = &fs->locals[fs->nactvar];
    l->name = name;
    l->reg = reg;
    l->flags = flags;
    l->lazy_mask = 0;
    return fs->nactvar++;
}

int ulocal_declare(UEmitter *e, const char *name) {
    UFuncState *fs = e->fs;
    int from = (fs->nblocks > 0) ? fs->blocks[fs->nblocks - 1].nactvar_on_enter : 0;
    for (int i = from; i < fs->nactvar; i++) {
        if (fs->locals[i].name == name) {
            if (e->error == EMIT_OK)
                urbi_emit_diag_error(e, NULL, "variable '%s' already declared in this scope", name);
            (void)uemit_fail(e, EMIT_LOCAL_REDECLARE);
            return -1;
        }
    }
    uint8_t r = ureg_alloc(e);
    if (e->error != EMIT_OK) return -1;
    return local_push(e, name, r, 0U);
}

uint8_t upin(UEmitter *e) {
    uint8_t r = ureg_alloc(e);
    if (e->error == EMIT_OK) (void)local_push(e, NULL, r, ULOCAL_PINNED);
    return r;
}

void uunpin(UEmitter *e, uint8_t r) {
    UFuncState *fs = e->fs;
    if (e->error != EMIT_OK) return;
    URBI_INTERNAL_ASSERT(fs->nactvar > 0 && fs->locals[fs->nactvar - 1].reg == r
                         && (fs->locals[fs->nactvar - 1].flags & ULOCAL_PINNED) != 0U);
    fs->nactvar--;
    fs->freereg = r;
}

int ulocal_find(const UFuncState *fs, const char *name) {
    for (int i = fs->nactvar - 1; i >= 0; i--) {
        if (fs->locals[i].name == name) return i;
    }
    return -1;
}

/* --- upvalues ------------------------------------------------------------ */

static int upval_install(UEmitter *e, UFuncState *fs, const char *name, uint8_t idx, bool in_stack) {
    for (int i = 0; i < fs->nupvals; i++) {
        if (fs->upvals[i].name == name) return i;
    }
    if (fs->nupvals >= UFS_MAX_UPVALUES) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "too many captured variables in function (max %d)", UFS_MAX_UPVALUES);
        (void)uemit_fail(e, EMIT_UPVAL_EXHAUSTED);
        return -1;
    }
    UUpval *u = &fs->upvals[fs->nupvals];
    u->name = name;
    u->idx = idx;
    u->in_stack = in_stack;
    return fs->nupvals++;
}

int uupval_find_or_install(UEmitter *e, UFuncState *fs, const char *name) {
    UFuncState *parent = fs->parent;
    if (parent == NULL) return -1;
    for (int i = 0; i < fs->nupvals; i++) {
        if (fs->upvals[i].name == name) return i;
    }
    int li = ulocal_find(parent, name);
    if (li >= 0) {
        /* The innermost block holding the local closes its cell when the
         * block ends; the function's own return closes the rest. */
        parent->locals[li].flags |= ULOCAL_CAPTURED;
        parent->ncaptures++;
        for (int b = parent->nblocks - 1; b >= 0; b--) {
            if (parent->blocks[b].nactvar_on_enter <= li) {
                parent->blocks[b].has_captured = true;
                break;
            }
        }
        return upval_install(e, fs, name, parent->locals[li].reg, true);
    }
    int gi = uupval_find_or_install(e, parent, name);
    if (gi < 0) return -1;
    return upval_install(e, fs, name, (uint8_t)gi, false);
}

/* --- blocks --------------------------------------------------------------- */

bool ublock_open(UEmitter *e, bool is_loop) {
    UFuncState *fs = e->fs;
    if (fs->nblocks >= UFS_MAX_BLOCKS) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "blocks nested too deeply (max %d)", UFS_MAX_BLOCKS);
        return uemit_fail(e, EMIT_NESTING_TOO_DEEP);
    }
    UBlock *b = &fs->blocks[fs->nblocks++];
    b->nactvar_on_enter = fs->nactvar;
    b->freereg_on_enter = fs->freereg;
    b->is_loop = is_loop;
    b->has_captured = false;
    return true;
}

bool ublock_close(UEmitter *e) {
    UFuncState *fs = e->fs;
    if (fs->nblocks == 0) return uemit_fail(e, EMIT_UNSUPPORTED_AST);
    const UBlock *b = &fs->blocks[fs->nblocks - 1];
    if (b->has_captured) {
        (void)uinstr_emit(e, uinstr_enc_abc(OP_CLOSE, b->freereg_on_enter, 0U, 0U), fs->prev_line);
        /* A break or continue jumps past this CLOSE; the enclosing block's
         * own close (at a lower or equal register) has to cover it. */
        if (fs->nblocks >= 2) fs->blocks[fs->nblocks - 2].has_captured = true;
    }
    fs->nactvar = b->nactvar_on_enter;
    fs->freereg = b->freereg_on_enter;
    fs->nblocks--;
    return true;
}

/* --- functions ------------------------------------------------------------ */

UFuncState *ufunc_open(UEmitter *e, UProto *proto) {
    UFuncState *fs = e->fs_free;
    if (fs != NULL) {
        e->fs_free = fs->parent;
    } else {
        fs = (UFuncState *)uarena_alloc(&e->fs_arena, sizeof(UFuncState));
        if (fs == NULL) { (void)uemit_fail(e, EMIT_OOM); return NULL; }
    }
    urbi_zero(fs, sizeof *fs);
    fs->parent = e->fs;
    fs->proto = proto;
    fs->r_nargs = 0xFFU;
    e->fs = fs;
    return fs;
}

/* Copy the function's site names into its proto as site_name_strs, the
 * form the chunk writer serializes.  site_names stays NULL: the runtime's
 * bind interns each name and allocates that array itself, exactly as it
 * does for a chunk the loader produced. */
static void sites_to_proto(UEmitter *e, const UFuncState *fs) {
    UProto *p = fs->proto;
    if (fs->nsites == 0U) return;
    UChunkAllocFn alloc = emit_alloc_for(e->module);
    void *ud = e->module->alloc_ud;
    char **strs = (char **)alloc(NULL, (size_t)fs->nsites * sizeof(char *), ud);
    if (strs == NULL) { (void)uemit_fail(e, EMIT_OOM); return; }
    for (uint16_t i = 0; i < fs->nsites; i++) strs[i] = NULL;
    p->site_count = fs->nsites;
    p->site_name_strs = strs;
    for (uint16_t i = 0; i < fs->nsites; i++) {
        size_t n = urbi_strlen(fs->site_names[i]);
        char *dup = (char *)alloc(NULL, n + 1U, ud);
        if (dup == NULL) { (void)uemit_fail(e, EMIT_OOM); return; }
        emit_memcpy(dup, fs->site_names[i], n + 1U);
        strs[i] = dup;
    }
}

void ufunc_close(UEmitter *e) {
    UFuncState *fs = e->fs;
    UProto *p = fs->proto;
    p->max_reg = fs->max_reg;
    p->nupvals = (uint8_t)fs->nupvals;
    if (e->error == EMIT_OK) sites_to_proto(e, fs);
    if (fs->site_names != NULL) {
        emit_alloc_for(e->module)((void *)fs->site_names, 0, e->module->alloc_ud);
        fs->site_names = NULL;
    }
    e->fs = fs->parent;
    fs->parent = e->fs_free;
    e->fs_free = fs;
}

/* --- instruction stream --------------------------------------------------- */

int uinstr_pc(const UEmitter *e) {
    return (int)e->fs->proto->instr_count;
}

int uinstr_emit(UEmitter *e, uint32_t ins, uint32_t line) {
    UFuncState *fs = e->fs;
    UProto *p = fs->proto;
    if (e->error != EMIT_OK) return (int)p->instr_count;
    if (p->instr_count == p->instr_cap) {
        /* line_deltas has no capacity field of its own: it tracks
         * instr_cap, and the writer only ever reads instr_count bytes. */
        size_t lcap = p->instr_cap;
        if (!grow(e, (void **)&p->instructions, &p->instr_cap, p->instr_count + 1U, sizeof(uint32_t)))
            return (int)p->instr_count;
        if (!grow(e, (void **)&p->line_deltas, &lcap, p->instr_cap, sizeof(int8_t)))
            return (int)p->instr_count;
    }
    size_t pc = p->instr_count;
    int8_t delta = (int8_t)-128;
    bool absolute = (fs->prev_line == 0U);
    if (!absolute) {
        int64_t d = (int64_t)line - (int64_t)fs->prev_line;
        if (d <= (int64_t)INT8_MIN || d > (int64_t)INT8_MAX) absolute = true;
        else delta = (int8_t)d;
    }
    if (absolute) {
        if (!grow(e, (void **)&p->abs_lines, &p->abs_line_cap, p->abs_line_count + 1U, sizeof(UAbsLine)))
            return (int)pc;
        p->abs_lines[p->abs_line_count].pc = (uint32_t)pc;
        p->abs_lines[p->abs_line_count].line = line;
        p->abs_line_count++;
    }
    p->instructions[pc] = ins;
    p->line_deltas[pc] = delta;
    p->instr_count = pc + 1U;
    fs->prev_line = line;
    return (int)pc;
}

void uinstr_patch(UEmitter *e, int pc, uint32_t ins) {
    if (e->error != EMIT_OK) return;
    e->fs->proto->instructions[pc] = ins;
}

int ujmp_emit(UEmitter *e, uint32_t line) {
    return uinstr_emit(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)UEMIT_JMP_BIAS), line);
}

void ujmp_patch_to(UEmitter *e, int jmp_pc, int target_pc) {
    if (e->error != EMIT_OK) return;
    int off = target_pc - jmp_pc - 1;
    if (off < 0 || off > INT16_MAX) {
        (void)uemit_fail(e, EMIT_JUMP_TOO_FAR);
        return;
    }
    uinstr_patch(e, jmp_pc, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(UEMIT_JMP_BIAS + off)));
}

void ujmp_patch_here(UEmitter *e, int jmp_pc) {
    ujmp_patch_to(e, jmp_pc, uinstr_pc(e));
}

void ujmp_back(UEmitter *e, int target_pc, uint32_t line) {
    if (e->error != EMIT_OK) return;
    /* The runtime takes a negative offset relative to the jump itself. */
    int off = target_pc - uinstr_pc(e);
    if (off >= 0 || off < INT16_MIN) {
        (void)uemit_fail(e, EMIT_JUMP_TOO_FAR);
        return;
    }
    (void)uinstr_emit(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(UEMIT_JMP_BIAS + off)), line);
}

/* --- constants ------------------------------------------------------------ */

/* Index of `v` in the current pool, appending it when absent. */
static uint16_t const_add(UEmitter *e, UValue v) {
    UProto *p = e->fs->proto;
    if (e->error != EMIT_OK) return 0U;
    for (size_t i = 0; i < p->const_count; i++) {
        const UValue *k = &p->constants[i];
        if (k->kind != v.kind) continue;
        if (v.kind == (uint8_t)UVAL_INT && k->v.i == v.v.i) return (uint16_t)i;
        if (v.kind == (uint8_t)UVAL_STR && k->v.p == v.v.p) return (uint16_t)i;
        if (v.kind == (uint8_t)UVAL_FLOAT) {
            /* Bitwise, so 0.0 and -0.0 stay two constants. */
            uint64_t a, b;
            emit_memcpy(&a, &k->v.f, sizeof a);
            emit_memcpy(&b, &v.v.f, sizeof b);
            if (a == b) return (uint16_t)i;
        }
    }
    if (p->const_count > (size_t)UINT16_MAX) {
        (void)uemit_fail(e, EMIT_CONSTANT_POOL_FULL);
        return 0U;
    }
    if (!grow(e, (void **)&p->constants, &p->const_cap, p->const_count + 1U, sizeof(UValue))) return 0U;
    p->constants[p->const_count] = v;
    return (uint16_t)p->const_count++;
}

uint16_t uconst_int(UEmitter *e, int64_t v) {
    UValue k;
    urbi_zero(&k, sizeof k);
    k.kind = (uint8_t)UVAL_INT;
    k.v.i = v;
    return const_add(e, k);
}

uint16_t uconst_float(UEmitter *e, double v) {
    UValue k;
    urbi_zero(&k, sizeof k);
    k.kind = (uint8_t)UVAL_FLOAT;
    k.v.f = v;
    return const_add(e, k);
}

uint16_t uconst_str(UEmitter *e, const char *interned) {
    UValue k;
    urbi_zero(&k, sizeof k);
    k.kind = (uint8_t)UVAL_STR;
    k.v.p = (void *)interned;
    return const_add(e, k);
}

/* --- slot sites ----------------------------------------------------------- */

uint16_t usite(UEmitter *e, const char *interned_name) {
    UFuncState *fs = e->fs;
    if (e->error != EMIT_OK) return 0U;
    if (fs->nsites == UINT16_MAX) {
        urbi_emit_diag_error(e, NULL, "too many slot-access sites in function (max %u)", (unsigned)UINT16_MAX);
        (void)uemit_fail(e, EMIT_TOO_MANY_SITES);
        return 0U;
    }
    if (fs->nsites == fs->site_cap) {
        size_t cap = fs->site_cap;
        if (!grow(e, (void **)&fs->site_names, &cap, (size_t)fs->nsites + 1U, sizeof(const char *)))
            return 0U;
        fs->site_cap = (cap > (size_t)UINT16_MAX) ? (uint16_t)UINT16_MAX : (uint16_t)cap;
    }
    fs->site_names[fs->nsites] = interned_name;
    return fs->nsites++;
}

void uslot_emit(UEmitter *e, UOpcode op, uint8_t a, uint8_t b, uint16_t site, uint32_t line) {
    if (site > 255U) (void)uinstr_emit(e, uinstr_enc_abx(OP_EXTARG, 0U, (uint16_t)(site >> 8)), line);
    (void)uinstr_emit(e, uinstr_enc_abc(op, a, b, (uint8_t)(site & 0xFFU)), line);
}

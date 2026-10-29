/*
 * result_spill.c — see result_spill.h.
 *
 * Encoding: each record is written as its raw struct bytes (every scalar
 * field, whatever its type) followed by its string and array fields in a
 * fixed order. Decoding copies the raw bytes back and then replaces every
 * pointer field with a copy in the new result's arena. A pointer field added
 * to one of these structs must be added here too; the size assertions below
 * fail the build until it is.
 */
#include "pipeline/result_spill.h"

#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#define spill_fseek _fseeki64
#else
#include <unistd.h>
#define spill_fseek fseeko
#endif

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(CBMDefinition) == 248, "update result_spill.c for CBMDefinition");
_Static_assert(sizeof(CBMCall) == 312, "update result_spill.c for CBMCall");
_Static_assert(sizeof(CBMCallArg) == 32, "update result_spill.c for CBMCallArg");
_Static_assert(sizeof(CBMImport) == 24, "update result_spill.c for CBMImport");
_Static_assert(sizeof(CBMUsage) == 32, "update result_spill.c for CBMUsage");
_Static_assert(sizeof(CBMThrow) == 16, "update result_spill.c for CBMThrow");
_Static_assert(sizeof(CBMReadWrite) == 24, "update result_spill.c for CBMReadWrite");
_Static_assert(sizeof(CBMTypeRef) == 16, "update result_spill.c for CBMTypeRef");
_Static_assert(sizeof(CBMEnvAccess) == 16, "update result_spill.c for CBMEnvAccess");
_Static_assert(sizeof(CBMTypeAssign) == 24, "update result_spill.c for CBMTypeAssign");
_Static_assert(sizeof(CBMStringRef) == 32, "update result_spill.c for CBMStringRef");
_Static_assert(sizeof(CBMInfraBinding) == 24, "update result_spill.c for CBMInfraBinding");
_Static_assert(sizeof(CBMChannel) == 32, "update result_spill.c for CBMChannel");
_Static_assert(sizeof(CBMImplTrait) == 16, "update result_spill.c for CBMImplTrait");
_Static_assert(sizeof(CBMResolvedCall) == 56, "update result_spill.c for CBMResolvedCall");
_Static_assert(sizeof(CBMFileResult) == 4480, "update result_spill.c for CBMFileResult");
#endif

enum { SPILL_NULL = 0xFFFFFFFFU, SPILL_MAGIC = 0x31535243U /* "CRS1" */ };

/* ── Encoder ─────────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} spill_buf_t;

static void put(spill_buf_t *b, const void *p, size_t n) {
    if (b->failed || n == 0) {
        return;
    }
    if (n > SIZE_MAX - b->len) {
        b->failed = true;
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) {
            cap *= 2;
        }
        char *data = realloc(b->data, cap);
        if (!data) {
            b->failed = true;
            return;
        }
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put_u32(spill_buf_t *b, uint32_t v) {
    put(b, &v, sizeof(v));
}

static void put_bytes(spill_buf_t *b, const char *s, size_t n) {
    if (!s) {
        put_u32(b, SPILL_NULL);
        return;
    }
    if (n >= SPILL_NULL) {
        b->failed = true;
        return;
    }
    put_u32(b, (uint32_t)n);
    put(b, s, n);
}

static void put_str(spill_buf_t *b, const char *s) {
    put_bytes(b, s, s ? strlen(s) : 0);
}

/* NULL-terminated string array. */
static void put_strv(spill_buf_t *b, const char *const *v) {
    if (!v) {
        put_u32(b, SPILL_NULL);
        return;
    }
    uint32_t n = 0;
    while (v[n]) {
        n++;
    }
    put_u32(b, n);
    for (uint32_t i = 0; i < n; i++) {
        put_str(b, v[i]);
    }
}

/* ── Decoder ─────────────────────────────────────────────────────── */

typedef struct {
    const char *p;
    size_t left;
    CBMArena *arena;
    bool failed;
} spill_rd_t;

static void take(spill_rd_t *r, void *out, size_t n) {
    if (r->failed || n > r->left) {
        r->failed = true;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->p, n);
    r->p += n;
    r->left -= n;
}

static uint32_t take_u32(spill_rd_t *r) {
    uint32_t v = 0;
    take(r, &v, sizeof(v));
    return v;
}

/* A length-prefixed byte string copied into the arena and NUL-terminated. */
static const char *take_bytes(spill_rd_t *r, int *out_len) {
    uint32_t n = take_u32(r);
    if (out_len) {
        *out_len = 0;
    }
    if (r->failed || n == SPILL_NULL) {
        return NULL;
    }
    if (n > r->left) {
        r->failed = true;
        return NULL;
    }
    char *s = cbm_arena_alloc(r->arena, (size_t)n + 1);
    if (!s) {
        r->failed = true;
        return NULL;
    }
    memcpy(s, r->p, n);
    s[n] = '\0';
    r->p += n;
    r->left -= n;
    if (out_len) {
        *out_len = (int)n;
    }
    return s;
}

static const char *take_str(spill_rd_t *r) {
    return take_bytes(r, NULL);
}

static const char **take_strv(spill_rd_t *r) {
    uint32_t n = take_u32(r);
    if (r->failed || n == SPILL_NULL) {
        return NULL;
    }
    /* Each entry costs at least its 4-byte length. */
    if (n > r->left / sizeof(uint32_t)) {
        r->failed = true;
        return NULL;
    }
    const char **v = cbm_arena_alloc(r->arena, ((size_t)n + 1) * sizeof(*v));
    if (!v) {
        r->failed = true;
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        v[i] = take_str(r);
    }
    v[n] = NULL;
    return v;
}

/* Array header: the item count, then that many raw records. Returns arena
 * storage for the records (NULL for zero) with the count in *out_count. */
static void *take_items(spill_rd_t *r, size_t item_size, int *out_count) {
    uint32_t n = take_u32(r);
    *out_count = 0;
    if (r->failed || n == 0) {
        return NULL;
    }
    if (n > INT32_MAX || n > r->left / item_size) {
        r->failed = true;
        return NULL;
    }
    void *items = cbm_arena_alloc(r->arena, (size_t)n * item_size);
    if (!items) {
        r->failed = true;
        return NULL;
    }
    take(r, items, (size_t)n * item_size);
    *out_count = (int)n;
    return items;
}

/* ── Records ─────────────────────────────────────────────────────── */

/* Each record type lists its pointer fields once (X-macros). An array is
 * written as its count, then every record's raw bytes with the pointer
 * fields nulled (so encoding is a pure function of the content), then each
 * record's strings in field order. */
#define ENC_STR(f) put_str(b, x->f);
#define DEC_STR(f) x->f = take_str(r);
#define ENC_STRV(f) put_strv(b, x->f);
#define DEC_STRV(f) x->f = take_strv(r);
#define NULL_FIELD(f) x->f = NULL;

#define DEF_STRS(X)                                                                \
    X(name) X(qualified_name) X(label) X(file_path) X(signature) X(return_type)    \
    X(receiver) X(docstring) X(parent_class) X(route_path) X(route_method)         \
    X(structural_profile) X(body_tokens) X(variants) X(alias_value)                 \
    X(http_client) X(http_base_url)
#define DEF_STRVS(X) X(decorators) X(base_classes) X(param_names) X(param_types) X(return_types)

static void null_def(CBMDefinition *x) {
    DEF_STRS(NULL_FIELD)
    DEF_STRVS(NULL_FIELD)
    x->fingerprint = NULL;
}

static void enc_def(spill_buf_t *b, const CBMDefinition *x) {
    DEF_STRS(ENC_STR)
    DEF_STRVS(ENC_STRV)
    if (x->fingerprint && x->fingerprint_k > 0) {
        put_u32(b, (uint32_t)x->fingerprint_k);
        put(b, x->fingerprint, (size_t)x->fingerprint_k * sizeof(*x->fingerprint));
    } else {
        put_u32(b, SPILL_NULL);
    }
}

static void dec_def(spill_rd_t *r, CBMDefinition *x) {
    DEF_STRS(DEC_STR)
    DEF_STRVS(DEC_STRV)
    uint32_t k = take_u32(r);
    x->fingerprint = NULL;
    if (r->failed || k == SPILL_NULL) {
        return;
    }
    if (k > r->left / sizeof(*x->fingerprint)) {
        r->failed = true;
        return;
    }
    x->fingerprint = cbm_arena_alloc(r->arena, (size_t)k * sizeof(*x->fingerprint));
    if (!x->fingerprint) {
        r->failed = true;
        return;
    }
    take(r, x->fingerprint, (size_t)k * sizeof(*x->fingerprint));
}

#define CALL_STRS(X) X(callee_name) X(enclosing_func_qn) X(first_string_arg) X(second_arg_name)
#define ARG_STRS(X) X(expr) X(value) X(keyword)

static void null_call(CBMCall *x) {
    CALL_STRS(NULL_FIELD)
    for (int i = 0; i < CBM_MAX_CALL_ARGS; i++) {
        x->args[i].expr = NULL;
        x->args[i].value = NULL;
        x->args[i].keyword = NULL;
    }
}

static void enc_call(spill_buf_t *b, const CBMCall *x) {
    CALL_STRS(ENC_STR)
    for (int i = 0; i < CBM_MAX_CALL_ARGS; i++) {
        put_str(b, x->args[i].expr);
        put_str(b, x->args[i].value);
        put_str(b, x->args[i].keyword);
    }
}

static void dec_call(spill_rd_t *r, CBMCall *x) {
    CALL_STRS(DEC_STR)
    for (int i = 0; i < CBM_MAX_CALL_ARGS; i++) {
        x->args[i].expr = take_str(r);
        x->args[i].value = take_str(r);
        x->args[i].keyword = take_str(r);
    }
}

/* Records whose pointer fields are only strings. */
#define STRING_RECORD(T, sfx, FIELDS)                                              \
    static void null_##sfx(T *x) { FIELDS(NULL_FIELD) }                            \
    static void enc_##sfx(spill_buf_t *b, const T *x) { FIELDS(ENC_STR) }          \
    static void dec_##sfx(spill_rd_t *r, T *x) { FIELDS(DEC_STR) }

#define IMPORT_STRS(X) X(local_name) X(module_path)
#define USAGE_STRS(X) X(ref_name) X(enclosing_func_qn)
#define THROW_STRS(X) X(exception_name) X(enclosing_func_qn)
#define RW_STRS(X) X(var_name) X(enclosing_func_qn)
#define TYPEREF_STRS(X) X(type_name) X(enclosing_func_qn)
#define ENV_STRS(X) X(env_key) X(enclosing_func_qn)
#define TYPEASSIGN_STRS(X) X(var_name) X(type_name) X(enclosing_func_qn)
#define IMPLTRAIT_STRS(X) X(trait_name) X(struct_name)
#define RESOLVED_STRS(X) X(caller_qn) X(callee_qn) X(strategy) X(reason)
#define STRREF_STRS(X) X(value) X(enclosing_func_qn) X(key_path)
#define INFRA_STRS(X) X(source_name) X(target_url) X(broker)
#define CHANNEL_STRS(X) X(channel_name) X(transport) X(enclosing_func_qn)

STRING_RECORD(CBMImport, import, IMPORT_STRS)
STRING_RECORD(CBMUsage, usage, USAGE_STRS)
STRING_RECORD(CBMThrow, throw, THROW_STRS)
STRING_RECORD(CBMReadWrite, rw, RW_STRS)
STRING_RECORD(CBMTypeRef, typeref, TYPEREF_STRS)
STRING_RECORD(CBMEnvAccess, env, ENV_STRS)
STRING_RECORD(CBMTypeAssign, typeassign, TYPEASSIGN_STRS)
STRING_RECORD(CBMImplTrait, impltrait, IMPLTRAIT_STRS)
STRING_RECORD(CBMResolvedCall, resolved, RESOLVED_STRS)
STRING_RECORD(CBMStringRef, strref, STRREF_STRS)
STRING_RECORD(CBMInfraBinding, infra, INFRA_STRS)
STRING_RECORD(CBMChannel, channel, CHANNEL_STRS)

#define ENCODE_ARRAY(b, arr, sfx)                                                  \
    do {                                                                           \
        put_u32((b), (uint32_t)(arr).count);                                       \
        for (int i_ = 0; i_ < (arr).count; i_++) {                                 \
            __typeof__(*(arr).items) raw_;                                         \
            memcpy(&raw_, &(arr).items[i_], sizeof(raw_));                         \
            null_##sfx(&raw_);                                                     \
            put((b), &raw_, sizeof(raw_));                                         \
        }                                                                          \
        for (int i_ = 0; i_ < (arr).count; i_++) {                                 \
            enc_##sfx((b), &(arr).items[i_]);                                      \
        }                                                                          \
    } while (0)

#define DECODE_ARRAY(r, arr, sfx)                                                  \
    do {                                                                           \
        (arr).items = take_items((r), sizeof(*(arr).items), &(arr).count);         \
        (arr).cap = (arr).count;                                                   \
        for (int i_ = 0; i_ < (arr).count && !(r)->failed; i_++) {                 \
            dec_##sfx((r), &(arr).items[i_]);                                      \
        }                                                                          \
    } while (0)

/* Scalar result fields, written as one block so a decode restores them in
 * one step. */
typedef struct {
    bool has_error;
    bool parse_incomplete;
    bool parse_unusable;
    bool is_test_file;
    int32_t error_region_count;
    int32_t imports_count;
    int32_t cached_lang;
} spill_scalars_t;

char *cbm_result_spill_encode(const CBMFileResult *result, size_t *out_len) {
    spill_buf_t b = {0};
    put_u32(&b, SPILL_MAGIC);
    ENCODE_ARRAY(&b, result->defs, def);
    ENCODE_ARRAY(&b, result->calls, call);
    ENCODE_ARRAY(&b, result->imports, import);
    ENCODE_ARRAY(&b, result->usages, usage);
    ENCODE_ARRAY(&b, result->throws, throw);
    ENCODE_ARRAY(&b, result->rw, rw);
    ENCODE_ARRAY(&b, result->type_refs, typeref);
    ENCODE_ARRAY(&b, result->env_accesses, env);
    ENCODE_ARRAY(&b, result->type_assigns, typeassign);
    ENCODE_ARRAY(&b, result->impl_traits, impltrait);
    ENCODE_ARRAY(&b, result->resolved_calls, resolved);
    ENCODE_ARRAY(&b, result->string_refs, strref);
    ENCODE_ARRAY(&b, result->infra_bindings, infra);
    ENCODE_ARRAY(&b, result->channels, channel);
    put_str(&b, result->module_qn);
    put_str(&b, result->namespace_name);
    put_strv(&b, result->exports);
    put_strv(&b, result->constants);
    put_strv(&b, result->global_vars);
    put_strv(&b, result->macros);
    put_str(&b, result->error_msg);
    put_str(&b, result->error_ranges);
    spill_scalars_t scalars = {
        .has_error = result->has_error,
        .parse_incomplete = result->parse_incomplete,
        .parse_unusable = result->parse_unusable,
        .is_test_file = result->is_test_file,
        .error_region_count = result->error_region_count,
        .imports_count = result->imports_count,
        .cached_lang = (int32_t)result->cached_lang,
    };
    put(&b, &scalars, sizeof(scalars));
    put_bytes(&b, result->source, result->source ? (size_t)result->source_len : 0);
    if (b.failed) {
        free(b.data);
        return NULL;
    }
    *out_len = b.len;
    return b.data;
}

CBMFileResult *cbm_result_spill_decode(const char *data, size_t len) {
    CBMFileResult *result = calloc(1, sizeof(*result));
    if (!result) {
        return NULL;
    }
    cbm_arena_init(&result->arena);
    spill_rd_t r = {.p = data, .left = len, .arena = &result->arena};
    if (take_u32(&r) != SPILL_MAGIC) {
        r.failed = true;
    }
    DECODE_ARRAY(&r, result->defs, def);
    DECODE_ARRAY(&r, result->calls, call);
    DECODE_ARRAY(&r, result->imports, import);
    DECODE_ARRAY(&r, result->usages, usage);
    DECODE_ARRAY(&r, result->throws, throw);
    DECODE_ARRAY(&r, result->rw, rw);
    DECODE_ARRAY(&r, result->type_refs, typeref);
    DECODE_ARRAY(&r, result->env_accesses, env);
    DECODE_ARRAY(&r, result->type_assigns, typeassign);
    DECODE_ARRAY(&r, result->impl_traits, impltrait);
    DECODE_ARRAY(&r, result->resolved_calls, resolved);
    DECODE_ARRAY(&r, result->string_refs, strref);
    DECODE_ARRAY(&r, result->infra_bindings, infra);
    DECODE_ARRAY(&r, result->channels, channel);
    result->module_qn = take_str(&r);
    result->namespace_name = take_str(&r);
    result->exports = take_strv(&r);
    result->constants = take_strv(&r);
    result->global_vars = take_strv(&r);
    result->macros = take_strv(&r);
    result->error_msg = take_str(&r);
    result->error_ranges = take_str(&r);
    spill_scalars_t scalars;
    take(&r, &scalars, sizeof(scalars));
    result->has_error = scalars.has_error;
    result->parse_incomplete = scalars.parse_incomplete;
    result->parse_unusable = scalars.parse_unusable;
    result->is_test_file = scalars.is_test_file;
    result->error_region_count = scalars.error_region_count;
    result->imports_count = scalars.imports_count;
    result->cached_lang = (CBMLanguage)scalars.cached_lang;
    result->source = take_bytes(&r, &result->source_len);
    if (r.failed || r.left != 0) {
        cbm_free_result(result);
        return NULL;
    }
    return result;
}

/* ── Store ───────────────────────────────────────────────────────── */

typedef struct {
    int64_t offset; /* -1: empty */
    int64_t len;
    int infra_bindings;
    int string_refs;
} spill_slot_t;

struct cbm_result_spill {
    char path[1024];
    FILE *file; /* opened on the first park */
    bool open_failed;
    int64_t end;
    cbm_mutex_t mu;
    spill_slot_t *slots;
    int slot_count;
    _Atomic int64_t parked;
    _Atomic int64_t bytes;
    _Atomic int64_t loads;
    _Atomic int64_t load_failures;
};

cbm_result_spill_t *cbm_result_spill_open(const char *dir, int slots) {
    if (!dir || slots <= 0) {
        return NULL;
    }
    cbm_result_spill_t *sp = calloc(1, sizeof(*sp));
    spill_slot_t *table = calloc((size_t)slots, sizeof(*table));
    if (!sp || !table) {
        free(sp);
        free(table);
        return NULL;
    }
    static _Atomic int sequence;
    int n = snprintf(sp->path, sizeof(sp->path), "%s/spill-%d-%d.bin", dir, (int)getpid(),
                     atomic_fetch_add(&sequence, 1));
    if (n < 0 || (size_t)n >= sizeof(sp->path)) {
        free(sp);
        free(table);
        return NULL;
    }
    for (int i = 0; i < slots; i++) {
        table[i].offset = -1;
    }
    sp->slots = table;
    sp->slot_count = slots;
    cbm_mutex_init(&sp->mu);
    return sp;
}

/* Under sp->mu. Creates <dir> and the file on first use. */
static bool spill_ensure_file(cbm_result_spill_t *sp) {
    if (sp->file || sp->open_failed) {
        return sp->file != NULL;
    }
    char dir[sizeof(sp->path)];
    snprintf(dir, sizeof(dir), "%s", sp->path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        cbm_mkdir_p(dir, 0700);
    }
    sp->file = fopen(sp->path, "w+b");
    if (!sp->file) {
        sp->open_failed = true;
        cbm_log_warn("mem.spill.open_failed", "path", sp->path);
    }
    return sp->file != NULL;
}

bool cbm_result_spill_park(cbm_result_spill_t *sp, int slot, CBMFileResult *result) {
    if (!sp || !result || slot < 0 || slot >= sp->slot_count) {
        return false;
    }
    size_t len = 0;
    char *data = cbm_result_spill_encode(result, &len);
    if (!data) {
        return false;
    }
    bool ok = false;
    cbm_mutex_lock(&sp->mu);
    if (sp->slots[slot].offset < 0 && spill_ensure_file(sp) &&
        spill_fseek(sp->file, sp->end, SEEK_SET) == 0 && fwrite(data, 1, len, sp->file) == len) {
        sp->slots[slot] = (spill_slot_t){
            .offset = sp->end,
            .len = (int64_t)len,
            .infra_bindings = result->infra_bindings.count,
            .string_refs = result->string_refs.count,
        };
        sp->end += (int64_t)len;
        ok = true;
    }
    cbm_mutex_unlock(&sp->mu);
    free(data);
    if (ok) {
        atomic_fetch_add(&sp->parked, 1);
        atomic_fetch_add(&sp->bytes, (int64_t)len);
        cbm_free_result(result);
    }
    return ok;
}

bool cbm_result_spill_has(const cbm_result_spill_t *sp, int slot) {
    /* A slot is written before its result leaves the cache and read only by
     * later phases, so no lock is needed here. */
    return sp && slot >= 0 && slot < sp->slot_count && sp->slots[slot].offset >= 0;
}

int cbm_result_spill_infra_binding_count(const cbm_result_spill_t *sp, int slot) {
    return cbm_result_spill_has(sp, slot) ? sp->slots[slot].infra_bindings : 0;
}

int cbm_result_spill_string_ref_count(const cbm_result_spill_t *sp, int slot) {
    return cbm_result_spill_has(sp, slot) ? sp->slots[slot].string_refs : 0;
}

CBMFileResult *cbm_result_spill_load(cbm_result_spill_t *sp, int slot) {
    if (!cbm_result_spill_has(sp, slot)) {
        return NULL;
    }
    spill_slot_t entry = sp->slots[slot];
    char *data = malloc((size_t)entry.len);
    if (!data) {
        return NULL;
    }
    cbm_mutex_lock(&sp->mu);
    bool read = fflush(sp->file) == 0 && spill_fseek(sp->file, entry.offset, SEEK_SET) == 0 &&
                fread(data, 1, (size_t)entry.len, sp->file) == (size_t)entry.len;
    cbm_mutex_unlock(&sp->mu);
    CBMFileResult *result = read ? cbm_result_spill_decode(data, (size_t)entry.len) : NULL;
    free(data);
    if (result) {
        atomic_fetch_add(&sp->loads, 1);
    } else {
        atomic_fetch_add(&sp->load_failures, 1);
        cbm_log_error("mem.spill.load_failed", "path", sp->path);
    }
    return result;
}

int64_t cbm_result_spill_load_failures(const cbm_result_spill_t *sp) {
    return sp ? atomic_load(&sp->load_failures) : 0;
}

void cbm_result_spill_stats(const cbm_result_spill_t *sp, int64_t *parked, int64_t *bytes,
                            int64_t *loads) {
    *parked = sp ? atomic_load(&sp->parked) : 0;
    *bytes = sp ? atomic_load(&sp->bytes) : 0;
    *loads = sp ? atomic_load(&sp->loads) : 0;
}

void cbm_result_spill_close(cbm_result_spill_t *sp) {
    if (!sp) {
        return;
    }
    if (sp->file) {
        fclose(sp->file);
        cbm_unlink(sp->path);
    }
    cbm_mutex_destroy(&sp->mu);
    free(sp->slots);
    free(sp);
}

/* ── Cache access ────────────────────────────────────────────────── */

CBMFileResult *cbm_result_cache_acquire(CBMFileResult **cache, cbm_result_spill_t *sp, int i) {
    if (cache[i]) {
        return cache[i];
    }
    return cbm_result_spill_load(sp, i);
}

void cbm_result_cache_release(CBMFileResult **cache, int i, CBMFileResult *result) {
    if (result && result != cache[i]) {
        cbm_free_result(result);
    }
}

bool cbm_result_cache_present(CBMFileResult *const *cache, const cbm_result_spill_t *sp, int i) {
    return cache[i] != NULL || cbm_result_spill_has(sp, i);
}

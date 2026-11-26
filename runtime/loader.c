// NSO loading at the fixed layout bases, dynamic linking with host overrides, the nnMain identity check, sealing,
// and --probe-loader.
//
// The lifted C was generated from the unrelocated words of .text and .rodata and embeds absolute guest addresses, so
// each module must sit at its layout base and relocations may only write the data segment.
// Contains adapted code under the MIT licence: authors and licence texts in THIRD_PARTY_NOTICES.md.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define RT_MAX_MODULES 16

// hostfn.c, for the loader only.
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original);

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_FINI = 13, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27,
       DT_FINI_ARRAYSZ = 28, DT_RELRSZ = 0x23, DT_RELR = 0x24, DT_GNU_HASH = 0x6ffffef5 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027, R_TLSDESC = 1031 };
enum { K_RELATIVE, K_ABS64, K_GLOB_DAT, K_JUMP_SLOT, K_COUNT };
enum { STB_WEAK = 2, STT_FUNC = 2 };
#define RUNTIME_MODULE "nro"   // the module nn::ro::LoadModule brings in (MTF: the game's code)

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

// How a symbol of a relocation was bound.
enum { RES_UNSET, RES_GUEST, RES_OVERRIDE, RES_OBSERVER, RES_WEAK, RES_STUB };
typedef struct { uint64_t value; uint8_t kind; } Res;

typedef struct {
    RtLoaded pub;
    uint32_t seg_off[3], seg_size[3];   // text, rodata, data (data without bss)
    const Elf64Sym *syms;
    const char *strtab;
    uint32_t nsyms;
    const Elf64Rela *rela, *jmprel;
    uint64_t nrela, njmprel;
    const uint64_t *relr;               // DT_RELR: packed RELATIVE relocations
    uint64_t nrelr, relr_count;
    uint32_t *hash, hash_mask;          // defined symbols by name
    Res *res_plain, *res_jump;          // per symbol; JUMP_SLOT may bind an observer
    uint32_t *funcs, nfuncs;            // defined functions sorted by value (ldr_describe)
    uint64_t *plt_slot, *plt_stub, nplt;   // PLT stubs sorted by GOT slot (ldr_plt_stub), built on first use
    // statistics
    uint64_t kinds[K_COUNT], sym_import, sym_own;
    uint32_t imports, import_to[RT_MAX_MODULES], weak_unresolved, strong_unresolved;
    uint32_t init_off;
    uint32_t tlsdesc;                   // TLS descriptor relocations left to the module's DT_INIT
} Mod;

static Mod mods[RT_MAX_MODULES];
static int nmods;
static uint64_t identity_refs, stubs;
static int *ovr_used, *obs_used;   // per linked table entry, plus probe overrides counted separately

int ldr_count(void) { return nmods; }
const RtLoaded *ldr_module(int i) { return i >= 0 && i < nmods ? &mods[i].pub : NULL; }

static uint32_t rd32(uint64_t a) { return LD32(a); }

// ---- files
static uint8_t *read_file(const char *path, size_t *len) {
    uint64_t size = 0;
    uint8_t *p = plat_file_read(path, &size);
    if (p) *len = (size_t)size;
    return p;
}

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

uint8_t *ldr_read_verified(const char *dir, const char *file, size_t *len) {
    const char *want = NULL;
    for (int i = 0; i < rt_exefs_sha256_count; i++)
        if (!strcmp(rt_exefs_sha256[i].file, file)) want = rt_exefs_sha256[i].sha256;
    if (!want) rt_fatal("loader: no expected SHA-256 for %s in modules.c", file);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    uint8_t *p = read_file(path, len);
    if (!p) rt_fatal("loader: cannot read %s (extract the game first: tools/extract.py)", path);
    uint8_t d[32];
    char hex[65];
    rt_sha256(p, *len, d);
    hex32(d, hex);
    if (strcmp(hex, want)) rt_fatal("loader: %s has SHA-256 %s, expected %s", path, hex, want);
    return p;
}

// ---- NSO
static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; lit += b; } while (b == 255); }
        if ((size_t)(de - d) < lit || (size_t)(se - s) < lit) return 0;
        memcpy(d, s, lit);
        d += lit;
        s += lit;
        if (s >= se) break;
        if (se - s < 2) return 0;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > (size_t)(d - dst) || (size_t)(de - d) < ml) return 0;
        uint8_t *m = d - off;
        while (ml--) *d++ = *m++;
    }
    return (size_t)(d - dst);
}

typedef struct { uint32_t file_off, mem_off, size, csize; int compressed; } Seg;

static uint64_t nso_segments(const uint8_t *raw, size_t len, const char *name, Seg seg[3]) {
    if (len < 0x100 || memcmp(raw, "NSO0", 4)) rt_fatal("loader: %s is not an NSO", name);
    uint32_t flags = *(const uint32_t *)(raw + 0xC);
    for (int i = 0; i < 3; i++) {
        memcpy(&seg[i], raw + 0x10 + i * 0x10, 12);
        seg[i].csize = *(const uint32_t *)(raw + 0x60 + i * 4);
        seg[i].compressed = (flags >> i) & 1;
        if ((uint64_t)seg[i].file_off + (seg[i].compressed ? seg[i].csize : seg[i].size) > len)
            rt_fatal("loader: %s segment %d lies outside the file", name, i);
    }
    uint32_t bss = *(const uint32_t *)(raw + 0x3C);
    return ((uint64_t)seg[2].mem_off + seg[2].size + bss + 0xFFF) & ~0xFFFull;
}

static void nso_segment(const uint8_t *raw, const Seg *s, uint8_t *dst, const char *name, int i) {
    if (!s->compressed) {
        memcpy(dst, raw + s->file_off, s->size);
    } else if (lz4_decompress(raw + s->file_off, s->csize, dst, s->size) != s->size) {
        rt_fatal("loader: %s segment %d: LZ4 data is corrupt", name, i);
    }
}

// ---- symbols
static const char *sym_name(const Mod *m, uint32_t i) { return m->strtab + m->syms[i].name; }
static int sym_defined(const Mod *m, uint32_t i) { return m->syms[i].shndx != 0; }

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void make_hash(Mod *m) {
    uint32_t cap = 16;
    while (cap < m->nsyms * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof *m->hash);
    if (!m->hash) rt_fatal("loader: symbol hash allocation");
    m->hash_mask = cap - 1;
    for (uint32_t i = 1; i < m->nsyms; i++) {
        if (!sym_defined(m, i) || !*sym_name(m, i)) continue;
        const char *s = sym_name(m, i);
        uint32_t p = fnv(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, sym_name(m, m->hash[p]))) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}

static uint64_t module_symbol(const Mod *m, const char *name) {
    for (uint32_t p = fnv(name) & m->hash_mask; m->hash[p]; p = (p + 1) & m->hash_mask) {
        uint32_t i = m->hash[p];
        if (!strcmp(name, sym_name(m, i))) return m->pub.base + m->syms[i].value;
    }
    return 0;
}

// The defined symbol in load order (main, subsdk0, sdk); *which is its module index.
static uint64_t find_symbol(const char *name, int *which) {
    for (int i = 0; i < nmods; i++) {
        uint64_t a = module_symbol(&mods[i], name);
        if (a) {
            if (which) *which = i;
            return a;
        }
    }
    return 0;
}

uint64_t ldr_symbol(const char *name) { return find_symbol(name, NULL); }

static const Mod *sort_mod;
static int by_value(const void *a, const void *b) {
    uint64_t x = sort_mod->syms[*(const uint32_t *)a].value, y = sort_mod->syms[*(const uint32_t *)b].value;
    return x < y ? -1 : x > y;
}

static void make_funcs(Mod *m) {
    m->funcs = malloc((m->nsyms ? m->nsyms : 1) * sizeof *m->funcs);
    if (!m->funcs) rt_fatal("loader: function table allocation");
    for (uint32_t i = 1; i < m->nsyms; i++)
        if (sym_defined(m, i) && (m->syms[i].info & 0xF) == STT_FUNC && *sym_name(m, i)) m->funcs[m->nfuncs++] = i;
    sort_mod = m;
    qsort(m->funcs, m->nfuncs, sizeof *m->funcs, by_value);
}

// ---- loading
static void parse_dynamic(Mod *m) {
    uint64_t base = m->pub.base;
    uint64_t mod0 = base + rd32(base + 4);
    if (memcmp((const void *)(uintptr_t)mod0, "MOD0", 4)) rt_fatal("loader: %s has no MOD0 header", m->pub.name);
    uint64_t dyn = mod0 + (int64_t)(int32_t)rd32(mod0 + 4);
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, hash = 0, gnu_hash = 0, relr = 0, relrsz = 0;
    for (const uint64_t *d = (const uint64_t *)(uintptr_t)dyn; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->syms = (const Elf64Sym *)(uintptr_t)(base + d[1]); break;
        case DT_STRTAB: m->strtab = (const char *)(uintptr_t)(base + d[1]); break;
        case DT_HASH: hash = base + d[1]; break;
        case DT_GNU_HASH: gnu_hash = base + d[1]; break;
        case DT_RELR: relr = base + d[1]; break;
        case DT_RELRSZ: relrsz = d[1]; break;
        case DT_RELA: rela = base + d[1]; break;
        case DT_RELASZ: relasz = d[1]; break;
        case DT_JMPREL: jmprel = base + d[1]; break;
        case DT_PLTRELSZ: pltsz = d[1]; break;
        case DT_INIT: m->pub.init = base + d[1]; m->init_off = (uint32_t)d[1]; break;
        case DT_INIT_ARRAY: m->pub.init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->pub.init_array_size = d[1]; break;
        }
    }
    if (!m->syms || !m->strtab || (!hash && !gnu_hash))
        rt_fatal("loader: %s lacks DT_SYMTAB, DT_STRTAB or a symbol hash", m->pub.name);
    if (hash) {
        m->nsyms = rd32(hash + 4);   // nchain
    } else {
        // DT_GNU_HASH only (some modules): one past the last symbol the highest bucket's chain reaches.
        uint32_t nbuckets = rd32(gnu_hash), symoffset = rd32(gnu_hash + 4), bloom = rd32(gnu_hash + 8);
        uint64_t buckets = gnu_hash + 16 + 8ull * bloom, chains = buckets + 4ull * nbuckets;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++)
            if (rd32(buckets + 4ull * i) > last) last = rd32(buckets + 4ull * i);
        if (last < symoffset) {
            m->nsyms = symoffset;
        } else {
            while (!(rd32(chains + 4ull * (last - symoffset)) & 1)) last++;
            m->nsyms = last + 1;
        }
    }
    m->relr = (const uint64_t *)(uintptr_t)relr;
    m->nrelr = relrsz / 8;
    m->rela = (const Elf64Rela *)(uintptr_t)rela;
    m->nrela = relasz / sizeof(Elf64Rela);
    m->jmprel = (const Elf64Rela *)(uintptr_t)jmprel;
    m->njmprel = pltsz / sizeof(Elf64Rela);
}

// The constructor state flag DT_INIT checks: ADRP xN at DT_INIT+16, LDR wM, [xN, #imm] at +20 or +24.
// The first ADRP in DT_INIT's prologue (+8..+28) followed within two instructions by the LDR of that register.
// Modules without PACIBSP have the ADRP at +16; MTF's, after a PACIBSP and a larger frame, at +20.
static uint64_t decode_ctor_flag(const Mod *m) {
    uint64_t init = m->pub.init;
    if (!init) return 0;
    for (int adrp_at = 8; adrp_at <= 28; adrp_at += 4) {
        uint32_t a = rd32(init + adrp_at);
        if ((a & 0x9f000000u) != 0x90000000u) continue;
        uint32_t reg = a & 31;
        for (int at = adrp_at + 4; at <= adrp_at + 8; at += 4) {
            uint32_t w = rd32(init + at);
            if ((w & 0xffc00000u) != 0xb9400000u || ((w >> 5) & 31) != reg) continue;
            int64_t imm = (int64_t)(((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2));
            imm = (imm ^ (1 << 20)) - (1 << 20);
            uint64_t flag = ((init + adrp_at) & ~0xFFFull) + (uint64_t)(imm << 12) + ((w >> 10) & 0xfff) * 4;
            uint64_t data = m->pub.base + m->pub.data_off;
            return flag >= data && flag < m->pub.base + m->pub.size ? flag : 0;
        }
        return 0;
    }
    return 0;
}

static void load_module(Mod *m, const char *dir, const RtLayoutEntry *lay) {
    size_t len;
    uint8_t *raw = ldr_read_verified(dir, lay->name, &len);
    Seg seg[3];
    uint64_t size = nso_segments(raw, len, lay->name, seg);
    if (size != lay->size)
        rt_fatal("loader: %s image is %#llx bytes, layout.h says %#llx: generated C and game files differ", lay->name,
                 (unsigned long long)size, (unsigned long long)lay->size);
    if (!plat_mem_commit(lay->base, size))
        rt_fatal("loader: cannot commit %s at %#llx", lay->name, (unsigned long long)lay->base);
    for (int i = 0; i < 3; i++) {
        if ((uint64_t)seg[i].mem_off + seg[i].size > size) rt_fatal("loader: %s segment %d exceeds the image", lay->name, i);
        nso_segment(raw, &seg[i], (uint8_t *)(uintptr_t)(lay->base + seg[i].mem_off), lay->name, i);
        m->seg_off[i] = seg[i].mem_off;
        m->seg_size[i] = seg[i].size;
    }
    free(raw);
    m->pub.name = lay->name;
    m->pub.base = lay->base;
    m->pub.size = size;
    m->pub.text_end = seg[0].mem_off + seg[0].size;
    m->pub.data_off = seg[2].mem_off;
    if (m->pub.data_off & 0xFFF) rt_fatal("loader: %s data segment is not page aligned", lay->name);
    if (seg[1].mem_off < m->pub.text_end || seg[1].mem_off + seg[1].size > m->pub.data_off)
        rt_fatal("loader: %s segments are out of order", lay->name);
    parse_dynamic(m);
    make_hash(m);
    make_funcs(m);
    m->res_plain = calloc(m->nsyms, sizeof(Res));
    m->res_jump = calloc(m->nsyms, sizeof(Res));
    if (!m->res_plain || !m->res_jump) rt_fatal("loader: resolution table allocation");
}

// ---- relocation
// A strong import nothing defines: calling it is fatal with its name (expected count 0).
static void unresolved_stub(Cpu *c) {
    char lr[96];
    rt_fatal("call to unresolved import %s from %s", hostfn_name(c->pc), ldr_describe(c->x[30] - 4, lr, sizeof lr));
}

static void mark_used(int observers, const char *name) {
    int n = hostfn_table_size(observers);
    for (int i = 0; i < n; i++)
        if (!strcmp(hostfn_table_name(observers, i), name)) (observers ? obs_used : ovr_used)[i] = 1;
}

// Order: observer (JUMP_SLOT only), override by name, own definition, other modules in load
// order, weak -> 0, strong -> named fatal stub. Overrides also replace a module's own symbols.
static Res resolve(Mod *m, uint32_t si, int jump) {
    Res *cache = jump ? &m->res_jump[si] : &m->res_plain[si];
    if (cache->kind != RES_UNSET) return *cache;
    const char *name = sym_name(m, si);
    uint64_t guest = sym_defined(m, si) ? m->pub.base + m->syms[si].value : find_symbol(name, NULL);
    GuestFn f;
    Res r;
    if (jump && (f = hostfn_observer(name))) {
        r = (Res){hostfn_register_observer(name, f, guest), RES_OBSERVER};
        mark_used(1, name);
    } else if ((f = hostfn_override(name))) {
        r = (Res){hostfn_register(name, f), RES_OVERRIDE};
        mark_used(0, name);
    } else if (guest) {
        r = (Res){guest, RES_GUEST};
    } else if ((m->syms[si].info >> 4) == STB_WEAK) {
        r = (Res){0, RES_WEAK};
    } else {
        r = (Res){hostfn_register(name, unresolved_stub), RES_STUB};
        stubs++;
        rt_log("%s: strong import %s is unresolved", m->pub.name, name);
    }
    *cache = r;
    return r;
}

static void relocate_table(Mod *m, const Elf64Rela *r, uint64_t n) {
    uint64_t base = m->pub.base;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t type = (uint32_t)r[i].info, si = (uint32_t)(r[i].info >> 32);
        if (r[i].off < m->pub.data_off || r[i].off > m->pub.size - 8)
            rt_fatal("loader: %s relocation %llu writes +%#llx, outside the data segment", m->pub.name,
                     (unsigned long long)i, (unsigned long long)r[i].off);
        uint64_t v;
        if (type == R_TLSDESC) {
            // The Switch 2 SDK resolves a module's TLS descriptors itself: the module header's third word lists them
            // (count words, then these RELA entries), and the module's DT_INIT fills them once musl has given the
            // module its TLS. The slot is left as the image has it.
            m->tlsdesc++;
            continue;
        }
        if (type == R_RELATIVE) {
            m->kinds[K_RELATIVE]++;
            v = base + (uint64_t)r[i].addend;
        } else {
            int k = type == R_ABS64 ? K_ABS64 : type == R_GLOB_DAT ? K_GLOB_DAT : type == R_JUMP_SLOT ? K_JUMP_SLOT : -1;
            if (k < 0) rt_fatal("loader: %s relocation %llu has type %u", m->pub.name, (unsigned long long)i, type);
            if (si == 0 || si >= m->nsyms) rt_fatal("loader: %s relocation %llu names symbol %u", m->pub.name,
                                                    (unsigned long long)i, si);
            m->kinds[k]++;
            if (sym_defined(m, si)) m->sym_own++;
            else m->sym_import++;
            Res s = resolve(m, si, type == R_JUMP_SLOT);
            v = s.kind == RES_WEAK ? 0 : s.value + (type == R_JUMP_SLOT ? 0 : (uint64_t)r[i].addend);
        }
        ST64(base + r[i].off, v);
    }
}

// DT_RELR: an even entry is an offset to relocate (the next bitmap starts after it); an odd entry is a bitmap of the
// 63 words that follow. Each relocated word holds its addend, as R_AARCH64_RELATIVE with an implicit addend.
static void relocate_relr(Mod *m) {
    uint64_t base = m->pub.base, where = 0;
    for (uint64_t i = 0; i < m->nrelr; i++) {
        uint64_t e = m->relr[i];
        for (int k = -1; k < 63; k++) {
            uint64_t off;
            if (k < 0) {
                if (e & 1) continue;
                off = e;
                where = e + 8;
            } else {
                if (!(e & 1) || !((e >> (k + 1)) & 1)) continue;
                off = where + 8ull * k;
            }
            if (off < m->pub.data_off || off > m->pub.size - 8)
                rt_fatal("loader: %s RELR entry %llu writes +%#llx, outside the data segment", m->pub.name,
                         (unsigned long long)i, (unsigned long long)off);
            ST64(base + off, base + LD64(base + off));
            m->kinds[K_RELATIVE]++;
            m->relr_count++;
        }
        if (e & 1) where += 63 * 8;
    }
}

// Import table statistics: every undefined named symbol, bound by guest definition (overrides aside).
static void count_imports(Mod *m) {
    for (uint32_t i = 1; i < m->nsyms; i++) {
        if (sym_defined(m, i) || !*sym_name(m, i)) continue;
        m->imports++;
        int which = -1;
        if (find_symbol(sym_name(m, i), &which)) m->import_to[which]++;
        else if ((m->syms[i].info >> 4) == STB_WEAK) m->weak_unresolved++;
        else m->strong_unresolved++;
    }
}

// Every non-JUMP_SLOT reference to nnMain holds the real address.
static void verify_identity(void) {
    uint64_t real = ldr_symbol("nnMain");
    if (!real) rt_fatal("loader: nnMain is not defined");
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        for (int t = 0; t < 2; t++) {
            const Elf64Rela *r = t ? m->jmprel : m->rela;
            uint64_t n = t ? m->njmprel : m->nrela;
            for (uint64_t j = 0; j < n; j++) {
                uint32_t type = (uint32_t)r[j].info, si = (uint32_t)(r[j].info >> 32);
                if (type == R_RELATIVE || type == R_JUMP_SLOT || strcmp(sym_name(m, si), "nnMain")) continue;
                uint64_t actual = LD64(m->pub.base + r[j].off);
                if (actual != real)
                    rt_fatal("loader: %s +%#llx refers to nnMain as %#llx, not %#llx", m->pub.name,
                             (unsigned long long)r[j].off, (unsigned long long)actual, (unsigned long long)real);
                identity_refs++;
            }
        }
    }
    if (!identity_refs) rt_fatal("loader: no reference to nnMain to check");
}

// .text and .rodata read-only: guest code never executes from guest memory, and nothing may write them.
static void seal(Mod *m) {
    if (!plat_mem_protect(m->pub.base, m->pub.data_off, PLAT_PROT_READ))
        rt_fatal("loader: cannot seal %s", m->pub.name);
}

void ldr_load(const char *exefs_dir) {
    if (nmods) rt_fatal("ldr_load called twice");
    if (!mem_layout()->pool) rt_fatal("ldr_load before mem_init");
    if (rt_layout_count > RT_MAX_MODULES) rt_fatal("loader: %d modules in the layout", rt_layout_count);
    // The runtime-loaded NRO has a place in the layout but no ExeFS file: nn::ro::LoadModule loads it (ldr_load_image).
    for (int i = 0; i < rt_layout_count; i++)
        if (strcmp(rt_layout[i].name, RUNTIME_MODULE)) load_module(&mods[nmods++], exefs_dir, &rt_layout[i]);
    ovr_used = calloc(hostfn_table_size(0) + 1, sizeof(int));
    obs_used = calloc(hostfn_table_size(1) + 1, sizeof(int));
    for (int i = 0; i < nmods; i++) {
        relocate_table(&mods[i], mods[i].rela, mods[i].nrela);
        relocate_table(&mods[i], mods[i].jmprel, mods[i].njmprel);
        relocate_relr(&mods[i]);
        count_imports(&mods[i]);
        mods[i].pub.ctor_flag = decode_ctor_flag(&mods[i]);
    }
    verify_identity();
    for (int i = 0; i < nmods; i++) seal(&mods[i]);
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        rt_log("loaded %-8s at %#llx size %#llx: %llu relocations, %u imports", m->pub.name,
               (unsigned long long)m->pub.base, (unsigned long long)m->pub.size,
               (unsigned long long)(m->nrela + m->njmprel + m->relr_count), m->imports);
    }
}

// ---- the runtime-loaded module (nn::ro::LoadModule, ro.c)
static uint32_t bind_to_new(Mod *k, const Mod *nm);
// An NRO is its own memory image: header, then text, rodata and data at the offsets of its segment table (+0x20, 8
// bytes each: offset, size), bss after data (+0x38). It is copied to its layout base, relocated against the modules
// loaded at start and the host overrides, and sealed like them. The caller runs its DT_INIT.
const RtLoaded *ldr_load_image(const char *name, const uint8_t *image, uint64_t size) {
    const RtLayoutEntry *lay = NULL;
    for (int i = 0; i < rt_layout_count; i++)
        if (!strcmp(rt_layout[i].name, name)) lay = &rt_layout[i];
    if (!lay) rt_fatal("loader: %s has no place in the layout (lift it first: README.md)", name);
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].pub.name, name)) rt_fatal("loader: %s is loaded already", name);
    if (nmods == RT_MAX_MODULES) rt_fatal("loader: no module slot for %s", name);
    if (size < 0x80 || memcmp(image + 0x10, "NRO0", 4)) rt_fatal("loader: %s is not an NRO image", name);
    uint32_t seg[3][2], bss;
    memcpy(seg, image + 0x20, sizeof seg);
    memcpy(&bss, image + 0x38, 4);
    uint64_t span = ((uint64_t)seg[2][0] + seg[2][1] + bss + 0xFFF) & ~0xFFFull;
    if (span != lay->size || size > span)
        rt_fatal("loader: %s spans %#llx bytes, layout.h says %#llx: generated C and the image differ", name,
                 (unsigned long long)span, (unsigned long long)lay->size);
    Mod *m = &mods[nmods];
    memset(m, 0, sizeof *m);
    if (!plat_mem_commit(lay->base, span)) rt_fatal("loader: cannot commit %s at %#llx", name, (unsigned long long)lay->base);
    memcpy((void *)(uintptr_t)lay->base, image, size);
    for (int i = 0; i < 3; i++) m->seg_off[i] = seg[i][0], m->seg_size[i] = seg[i][1];
    m->pub.name = lay->name;
    m->pub.base = lay->base;
    m->pub.size = span;
    m->pub.text_end = seg[0][0] + seg[0][1];
    m->pub.data_off = seg[2][0];
    if (m->pub.data_off & 0xFFF) rt_fatal("loader: %s data segment is not page aligned", name);
    parse_dynamic(m);
    make_hash(m);
    make_funcs(m);
    m->res_plain = calloc(m->nsyms, sizeof(Res));
    m->res_jump = calloc(m->nsyms, sizeof(Res));
    if (!m->res_plain || !m->res_jump) rt_fatal("loader: resolution table allocation");
    __atomic_store_n(&nmods, nmods + 1, __ATOMIC_RELEASE);   // its own symbols resolve from here on
    relocate_table(m, m->rela, m->nrela);
    relocate_table(m, m->jmprel, m->njmprel);
    relocate_relr(m);
    count_imports(m);
    m->pub.ctor_flag = decode_ctor_flag(m);
    seal(m);
    for (int i = 0; i < nmods - 1; i++) {
        uint32_t bound = bind_to_new(&mods[i], m);
        if (bound) rt_log("loader: %u of %s's imports now bind to %s", bound, mods[i].pub.name, m->pub.name);
    }
    rt_log("loaded %-8s at %#llx size %#llx: %llu relocations (%u TLS descriptors left to DT_INIT), %u imports, %u "
           "strong unresolved", m->pub.name, (unsigned long long)m->pub.base, (unsigned long long)m->pub.size,
           (unsigned long long)(m->nrela + m->njmprel + m->relr_count), m->tlsdesc, m->imports, m->strong_unresolved);
    return &m->pub;
}

// After a module loads, the earlier modules' imports that nothing defined until now bind to it, as Horizon's rtld
// binds variables and lazily bound functions. MTF's main
// imports _Z6Abort_v, which only the NRO defines. A slot may sit in a RELRO range the guest made read-only, so the
// page is opened for the write and given back its protection.
static void write_slot(uint64_t where, uint64_t v) {
    uint64_t end;
    int prot = plat_mem_state(where, where + 8, &end);
    if (prot == PLAT_PROT_RW) {
        ST64(where, v);
        return;
    }
    uint64_t page = where & ~0xFFFull;
    if (!plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) rt_fatal("loader: cannot open %#llx to bind", (unsigned long long)where);
    ST64(where, v);
    plat_mem_protect(page, 0x1000, prot);
}

static uint32_t bind_to_new(Mod *k, const Mod *nm) {
    uint32_t n = 0;
    for (int t = 0; t < 2; t++) {
        const Elf64Rela *r = t ? k->jmprel : k->rela;
        uint64_t count = t ? k->njmprel : k->nrela;
        for (uint64_t j = 0; j < count; j++) {
            uint32_t type = (uint32_t)r[j].info, si = (uint32_t)(r[j].info >> 32);
            if (type == R_RELATIVE || type == R_TLSDESC || !si || sym_defined(k, si)) continue;
            int jump = type == R_JUMP_SLOT;
            Res *cache = jump ? &k->res_jump[si] : &k->res_plain[si];
            if (cache->kind != RES_STUB && cache->kind != RES_WEAK) continue;
            uint64_t a = module_symbol(nm, sym_name(k, si));
            if (!a) continue;
            write_slot(k->pub.base + r[j].off, a + (jump ? 0 : (uint64_t)r[j].addend));
            n++;
        }
    }
    for (uint32_t si = 1; si < k->nsyms; si++) {   // the resolution caches follow, for later lookups
        if (sym_defined(k, si)) continue;
        uint64_t a = 0;
        for (int jump = 0; jump < 2; jump++) {
            Res *cache = jump ? &k->res_jump[si] : &k->res_plain[si];
            if ((cache->kind == RES_STUB || cache->kind == RES_WEAK) && (a || (a = module_symbol(nm, sym_name(k, si)))))
                *cache = (Res){a, RES_GUEST};
        }
    }
    return n;
}

int ldr_in_layout(const char *name) {
    for (int i = 0; i < rt_layout_count; i++)
        if (!strcmp(rt_layout[i].name, name)) return 1;
    return 0;
}

// ---- lookups
static int module_of(uint64_t a) {
    for (int i = 0; i < nmods; i++)
        if (a - mods[i].pub.base < mods[i].pub.size) return i;
    return -1;
}

const char *ldr_describe(uint64_t a, char *buf, int n) {
    int i = module_of(a);
    if (i < 0) {
        const char *h = hostfn_name(a);
        if (h) snprintf(buf, n, "host %s", h);
        else snprintf(buf, n, "%#llx", (unsigned long long)a);
        return buf;
    }
    const Mod *m = &mods[i];
    uint64_t off = a - m->pub.base;
    uint32_t lo = 0, hi = m->nfuncs;   // last function with value <= off
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->syms[m->funcs[mid]].value <= off) lo = mid + 1;
        else hi = mid;
    }
    if (lo) {
        const Elf64Sym *s = &m->syms[m->funcs[lo - 1]];
        if (off - s->value < (s->size ? s->size : 0x100000)) {
            snprintf(buf, n, "%s+%#llx (%s+%#llx)", m->pub.name, (unsigned long long)off, m->strtab + s->name,
                     (unsigned long long)(off - s->value));
            return buf;
        }
    }
    snprintf(buf, n, "%s+%#llx", m->pub.name, (unsigned long long)off);
    return buf;
}

// ---- --probe-loader
// Loads and relocates the modules, then prints what was bound: imports per module, relocation kinds, constructors,
// overrides that matched nothing, and every strong import left unresolved. It fails on an unresolved strong import or
// on .text/.rodata that relocation changed.
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static void check_segments(const char *dir) {
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        size_t len;
        uint8_t *raw = ldr_read_verified(dir, m->pub.name, &len);
        Seg seg[3];
        nso_segments(raw, len, m->pub.name, seg);
        int same = 1;
        for (int s = 0; s < 2; s++) {
            uint8_t *buf = malloc(seg[s].size ? seg[s].size : 1);
            if (!buf) rt_fatal("probe: allocation");
            nso_segment(raw, &seg[s], buf, m->pub.name, s);
            if (memcmp(buf, (const void *)(uintptr_t)(m->pub.base + seg[s].mem_off), seg[s].size)) same = 0;
            free(buf);
        }
        free(raw);
        printf("  %-8s text %#x+%#x rodata %#x+%#x: bytes %s the NSO\n", m->pub.name, seg[0].mem_off, seg[0].size,
               seg[1].mem_off, seg[1].size, same ? "equal" : "DIFFER FROM");
        CHECK(same, "%s .text/.rodata changed after relocation", m->pub.name);
    }
}

int probe_loader(void) {
    const RtConfig *cfg = rt_config();
    ldr_load(cfg->exefs);
    printf("modules (exefs %s):\n", cfg->exefs);
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        char to[128] = "";
        for (int k = 0; k < nmods; k++)
            if (m->import_to[k])
                snprintf(to + strlen(to), sizeof to - strlen(to), "%s%s %u", *to ? ", " : "", mods[k].pub.name,
                         m->import_to[k]);
        uint64_t total = m->kinds[0] + m->kinds[1] + m->kinds[2] + m->kinds[3];
        printf("  %-8s base %#llx image %#llx DT_INIT +%#x init_array %llu imports %u (%s), weak unresolved %u, "
               "strong unresolved %u\n", m->pub.name, (unsigned long long)m->pub.base, (unsigned long long)m->pub.size,
               m->init_off, (unsigned long long)(m->pub.init_array_size / 8), m->imports, to, m->weak_unresolved,
               m->strong_unresolved);
        printf("           RELATIVE %llu ABS64 %llu GLOB_DAT %llu JUMP_SLOT %llu (total %llu, RELR %llu)\n",
               (unsigned long long)m->kinds[K_RELATIVE], (unsigned long long)m->kinds[K_ABS64],
               (unsigned long long)m->kinds[K_GLOB_DAT], (unsigned long long)m->kinds[K_JUMP_SLOT],
               (unsigned long long)total, (unsigned long long)m->relr_count);
        char d[128];
        printf("           constructor flag %s\n", m->pub.ctor_flag ? ldr_describe(m->pub.ctor_flag, d, sizeof d)
                                                                     : "not decoded");
    }
    check_segments(cfg->exefs);
    int linked = hostfn_table_size(0), installed = 0;
    for (int i = 0; i < linked; i++) {
        installed += ovr_used[i];
        if (!ovr_used[i]) printf("  override %s matches no relocation\n", hostfn_table_name(0, i));
    }
    int olinked = hostfn_table_size(1), oinstalled = 0;
    for (int i = 0; i < olinked; i++) oinstalled += obs_used[i];
    printf("overrides installed %d of %d linked, observers %d of %d\n", installed, linked, oinstalled, olinked);
    printf("nnMain identity ok (%llu refs)\n", (unsigned long long)identity_refs);
    // main imports _Z6Abort_v, which neither main nor sdk defines (the runtime-loaded NRO may); calling it is fatal
    // with its name. Any other unresolved strong import is a failure.
    CHECK(stubs <= 1 && (stubs == 0 || mods[0].strong_unresolved == 1), "%llu strong imports are unresolved (named "
          "in the log); only main's _Z6Abort_v is expected", (unsigned long long)stubs);
    for (int i = 0; i < nmods; i++)
        CHECK(mods[i].pub.ctor_flag, "%s constructor flag not decoded", mods[i].pub.name);
    printf("probe-loader: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

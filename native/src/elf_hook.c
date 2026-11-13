#include "elf_hook.h"
#include "log.h"

#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif

typedef struct {
    uintptr_t base;
    char path[512];
} module_info;

/*
 * Scans /proc/self/maps for the first mapping whose path contains
 * name_substr, and returns the lowest mapped address for that path
 * (the start of the ELF header, since the first segment is mapped
 * first and at the lowest address).
 */
static int find_module_base(const char *name_substr, module_info *out) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        LOGE("elf_hook: failed to open /proc/self/maps");
        return -1;
    }

    char line[1024];
    int found = 0;
    uintptr_t lowest = (uintptr_t)-1;

    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, name_substr)) continue;

        unsigned long start_ul, end_ul, offset_ul;
        char perms[8];
        char path[512] = {0};

        int n = sscanf(line, "%lx-%lx %7s %lx %*s %*d %511[^\n]",
                        &start_ul, &end_ul, perms, &offset_ul, path);
        if (n < 5) continue;

        uintptr_t start = (uintptr_t)start_ul;

        // We only care about the mapping that starts at file offset 0 —
        // that's the one containing the ELF header + program headers.
        if (offset_ul == 0 && start < lowest) {
            lowest = start;
            strncpy(out->path, path, sizeof(out->path) - 1);
            found = 1;
        }
    }
    fclose(f);

    if (!found) {
        LOGW("elf_hook: module matching '%s' not found in /proc/self/maps (not loaded yet?)", name_substr);
        return -1;
    }

    out->base = lowest;
    return 0;
}

static int mprotect_rw(void *addr) {
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page_start = (uintptr_t)addr & ~(page_size - 1);
    return mprotect((void *)page_start, (size_t)page_size, PROT_READ | PROT_WRITE);
}

/*
 * Walks one relocation table (either .rel.plt or .rel.dyn), looking for
 * a relocation against `symbol_name`. On match, overwrites the GOT slot
 * at bias + rel->r_offset with `replacement`, after saving the current
 * value (the real, already-resolved function pointer) into *original_out.
 * Returns 1 if patched, 0 if not found in this table.
 */
static int patch_in_table(const Elf32_Rel *rel_table, size_t rel_count,
                           const Elf32_Sym *symtab, const char *strtab,
                           uintptr_t bias, const char *symbol_name,
                           void *replacement, void **original_out) {
    for (size_t i = 0; i < rel_count; i++) {
        const Elf32_Rel *rel = &rel_table[i];
        unsigned char type = ELF32_R_TYPE(rel->r_info);
        unsigned int sym_idx = ELF32_R_SYM(rel->r_info);

        if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT && type != R_ARM_ABS32) {
            continue;
        }

        const char *name = strtab + symtab[sym_idx].st_name;
        if (strcmp(name, symbol_name) != 0) continue;

        void **got_slot = (void **)(bias + rel->r_offset);

        if (mprotect_rw(got_slot) != 0) {
            LOGE("elf_hook: mprotect failed for GOT slot of '%s'", symbol_name);
            return 0;
        }

        if (original_out) *original_out = *got_slot;
        *got_slot = replacement;

        LOGI("elf_hook: hooked '%s' (GOT slot %p, orig %p, new %p)",
             symbol_name, (void *)got_slot, original_out ? *original_out : NULL, replacement);
        return 1;
    }
    return 0;
}

int vsl_hook_symbol(const vsl_hook_request *req) {
    module_info mi;
    if (find_module_base(req->lib_name_substr, &mi) != 0) {
        return -1;
    }

    uintptr_t base = mi.base;
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)base;

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("elf_hook: bad ELF magic at base %p for %s", (void *)base, req->lib_name_substr);
        return -2;
    }

    const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(base + ehdr->e_phoff);
    uintptr_t bias = 0;
    int have_bias = 0;
    const Elf32_Phdr *dyn_phdr = NULL;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_LOAD && !have_bias) {
            bias = base - ph->p_vaddr;
            have_bias = 1;
        }
        if (ph->p_type == PT_DYNAMIC) {
            dyn_phdr = ph;
        }
    }

    if (!have_bias || !dyn_phdr) {
        LOGE("elf_hook: could not locate PT_LOAD/PT_DYNAMIC in %s", req->lib_name_substr);
        return -3;
    }

    const Elf32_Dyn *dyn = (const Elf32_Dyn *)(bias + dyn_phdr->p_vaddr);

    const Elf32_Sym *symtab = NULL;
    const char *strtab = NULL;
    const Elf32_Rel *jmprel = NULL;
    size_t pltrelsz = 0;
    const Elf32_Rel *reldyn = NULL;
    size_t relsz = 0;

    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB: symtab = (const Elf32_Sym *)(bias + dyn->d_un.d_ptr); break;
            case DT_STRTAB: strtab = (const char *)(bias + dyn->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = (const Elf32_Rel *)(bias + dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
            case DT_REL: reldyn = (const Elf32_Rel *)(bias + dyn->d_un.d_ptr); break;
            case DT_RELSZ: relsz = dyn->d_un.d_val; break;
            default: break;
        }
    }

    if (!symtab || !strtab) {
        LOGE("elf_hook: missing SYMTAB/STRTAB in %s", req->lib_name_substr);
        return -4;
    }

    int patched = 0;
    if (jmprel && pltrelsz) {
        patched = patch_in_table(jmprel, pltrelsz / sizeof(Elf32_Rel),
                                  symtab, strtab, bias,
                                  req->symbol_name, req->replacement, req->original_out);
    }
    if (!patched && reldyn && relsz) {
        patched = patch_in_table(reldyn, relsz / sizeof(Elf32_Rel),
                                  symtab, strtab, bias,
                                  req->symbol_name, req->replacement, req->original_out);
    }

    if (!patched) {
        LOGW("elf_hook: symbol '%s' not referenced via PLT/GOT by %s (maybe inlined, "
             "or not called from this lib at all)", req->symbol_name, req->lib_name_substr);
        return -5;
    }

    return 0;
}

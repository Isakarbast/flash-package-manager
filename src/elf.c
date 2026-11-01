/* Read DT_NEEDED and DT_SONAME from ELF files (32- and 64-bit, little endian).
 * Builds record which shared libraries a package provides and needs, so
 * dependencies on libraries are found automatically. */
#include "fpm.h"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Map a virtual address to a file offset through the PT_LOAD segments. */
#define DEFINE_PARSER(BITS)                                                              \
static int parse##BITS(const unsigned char *base, size_t size, strvec *needed, char **soname) \
{                                                                                        \
    const Elf##BITS##_Ehdr *eh = (const void *)base;                                    \
    if (sizeof *eh > size || eh->e_phentsize != sizeof(Elf##BITS##_Phdr) ||              \
        eh->e_phoff > size || (size - eh->e_phoff) / sizeof(Elf##BITS##_Phdr) < eh->e_phnum) \
        return -1;                                                                       \
    const Elf##BITS##_Phdr *ph = (const void *)(base + eh->e_phoff);                     \
    const Elf##BITS##_Phdr *dyn = NULL;                                                  \
    for (int i = 0; i < eh->e_phnum; i++)                                                \
        if (ph[i].p_type == PT_DYNAMIC)                                                  \
            dyn = &ph[i];                                                                \
    if (!dyn || dyn->p_offset > size || dyn->p_filesz > size - dyn->p_offset)            \
        return -1;                                                                       \
    const Elf##BITS##_Dyn *d = (const void *)(base + dyn->p_offset);                     \
    size_t nd = dyn->p_filesz / sizeof *d;                                               \
    uint64_t strtab = 0, strsz = 0;                                                      \
    for (size_t i = 0; i < nd && d[i].d_tag != DT_NULL; i++) {                           \
        if (d[i].d_tag == DT_STRTAB)                                                     \
            strtab = d[i].d_un.d_ptr;                                                    \
        else if (d[i].d_tag == DT_STRSZ)                                                 \
            strsz = d[i].d_un.d_val;                                                     \
    }                                                                                    \
    uint64_t stroff = 0;                                                                 \
    int found = 0;                                                                       \
    for (int i = 0; i < eh->e_phnum && !found; i++)                                      \
        if (ph[i].p_type == PT_LOAD && strtab >= ph[i].p_vaddr &&                        \
            strtab < (uint64_t)ph[i].p_vaddr + ph[i].p_filesz) {                         \
            stroff = strtab - ph[i].p_vaddr + ph[i].p_offset;                            \
            found = 1;                                                                   \
        }                                                                                \
    if (!found || stroff >= size)                                                        \
        return -1;                                                                       \
    if (!strsz || strsz > size - stroff)                                                 \
        strsz = size - stroff;                                                           \
    const char *str = (const char *)base + stroff;                                       \
    for (size_t i = 0; i < nd && d[i].d_tag != DT_NULL; i++) {                           \
        if (d[i].d_tag != DT_NEEDED && d[i].d_tag != DT_SONAME)                          \
            continue;                                                                    \
        uint64_t off = d[i].d_un.d_val;                                                  \
        if (off >= strsz || !memchr(str + off, 0, strsz - off))                          \
            continue;                                                                    \
        if (d[i].d_tag == DT_NEEDED) {                                                   \
            if (!sv_contains(needed, str + off))                                         \
                sv_push(needed, str + off);                                              \
        } else if (soname && !*soname) {                                                 \
            *soname = xstrdup(str + off);                                                \
        }                                                                                \
    }                                                                                    \
    return 0;                                                                            \
}

DEFINE_PARSER(32)
DEFINE_PARSER(64)

int elf_dynamic(const char *path, strvec *needed, char **soname)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < EI_NIDENT) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return -1;
    const unsigned char *b = map;
    int rc = -1;
    if (!memcmp(b, ELFMAG, SELFMAG) && b[EI_DATA] == ELFDATA2LSB) {
        if (b[EI_CLASS] == ELFCLASS64)
            rc = parse64(b, size, needed, soname);
        else if (b[EI_CLASS] == ELFCLASS32)
            rc = parse32(b, size, needed, soname);
    }
    munmap(map, size);
    return rc;
}

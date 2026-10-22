/* Raw .bin front for build/ps4mount-ps4.elf, for loaders that copy the payload into memory and
 * jump to its first byte (GoldHEN's BinLoader up to at least 2.4b18, Payload Guest, Vue).
 * build.sh appends the ELF right after this code (at __payload). Here we map its PT_LOAD
 * segments, apply its R_X86_64_RELATIVE relocations and call its entry; the SDK's crt1 then
 * loads the system libraries and resolves the other relocations itself.
 * Same scheme as bootstrap-bin.c in github.com/ps4-payload-dev/elfldr.
 * Freestanding: no libc, no data, no absolute relocations (the code runs at any address). */

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;

typedef struct {
    unsigned char e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} ehdr_t;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} phdr_t;

typedef struct {
    long d_tag;
    u64 d_val;
} dyn_t;

typedef struct {
    u64 r_offset, r_info;
    long r_addend;
} rela_t;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define ET_DYN 3
#define EM_X86_64 62
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define R_X86_64_RELATIVE 8

/* FreeBSD's numbers; test/binboot_test.c builds it with Linux's */
#ifndef SYS_mmap
#define SYS_mmap 477
#endif
#ifndef MAP_ANON
#define MAP_ANON 0x1000
#endif
#define PROT_RWX 7
#define MAP_PRIVATE 0x0002
#define PAGE 0x4000UL

extern unsigned char __payload[] __attribute__((visibility("hidden")));

static long sys6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    unsigned long ret;
    char err;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ __volatile__("syscall"
                         : "=a"(ret), "=@ccc"(err), "+r"(r10), "+r"(r8), "+r"(r9)
                         : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return err ? -1 : (long)ret;
}

static void copy(unsigned char *d, const unsigned char *s, u64 n) {
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

__attribute__((section(".text.start"), used)) int _start(void) {
    const unsigned char *elf = __payload;
    const ehdr_t *eh = (const ehdr_t *)elf;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2 || eh->e_type != ET_DYN || eh->e_machine != EM_X86_64)
        return -1;
    const phdr_t *ph = (const phdr_t *)(elf + eh->e_phoff);
    u64 end = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end)
            end = ph[i].p_vaddr + ph[i].p_memsz;
    end = (end + PAGE - 1) & ~(PAGE - 1);
    if (!end)
        return -1;
    /* anonymous memory comes zeroed, which covers .bss */
    long m = sys6(SYS_mmap, 0, (long)end, PROT_RWX, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == -1)
        return -1;
    unsigned char *img = (unsigned char *)m;
    const dyn_t *dyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz)
            copy(img + ph[i].p_vaddr, elf + ph[i].p_offset, ph[i].p_filesz);
        else if (ph[i].p_type == PT_DYNAMIC)
            dyn = (const dyn_t *)(img + ph[i].p_vaddr);
    }
    u64 rela = 0, relasz = 0;
    for (; dyn && dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_RELA)
            rela = dyn->d_val;
        else if (dyn->d_tag == DT_RELASZ)
            relasz = dyn->d_val;
    }
    const rela_t *r = (const rela_t *)(img + rela);
    for (u64 i = 0; rela && i < relasz / sizeof(rela_t); i++)
        if ((u32)r[i].r_info == R_X86_64_RELATIVE)
            *(u64 *)(img + r[i].r_offset) = (u64)img + (u64)r[i].r_addend;
    return ((int (*)(void))(img + eh->e_entry))();
}

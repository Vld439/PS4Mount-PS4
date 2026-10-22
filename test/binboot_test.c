/* Runs a .bin made like build/ps4mount-ps4.bin (tools/binboot + an ELF) the way GoldHEN's
 * BinLoader does: copied to executable memory at some address and called at its first byte. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    /* not page-aligned, as nothing promises the loader's buffer is */
    unsigned char *mem = mmap(0, n + 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 2;
    unsigned char *code = mem + 0x10;
    for (size_t i = 0; i < n; i++)
        code[i] = buf[i];
    int rc = ((int (*)(void))code)();
    if (rc != 42) {
        printf("FAIL: the payload returned %d, expected 42\n", rc);
        return 1;
    }
    printf("ok: the .bin loaded the ELF and ran it\n");
    return 0;
}

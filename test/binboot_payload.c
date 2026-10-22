/* Stand-in for the payload ELF in test/binboot_test.c: a PIE with no libc whose entry only
 * works when the loader applied its R_X86_64_RELATIVE relocations and zeroed its .bss. */

static int seven(void) { return 7; }
static int six(void) { return 6; }

/* globals read through volatile indexes, so the pointers stay as RELATIVE relocations */
int (*table[])(void) = {seven, six};
const char *name = "binboot";
int zeroed[4096]; /* .bss */
volatile int one = 1;

int entry(void) {
    for (int i = 0; i < 4096; i++)
        if (zeroed[i])
            return -2;
    if (name[one - 1] != 'b' || name[6] != 't')
        return -3;
    return table[one - 1]() * table[one](); /* 42 */
}

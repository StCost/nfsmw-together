// Runs speed.exe's own HUFF decoder (0x64DCA0: eax = packed data after the 16-byte header, [esp+4] = out)
// on blocks of gameplay.bin. usage: huff <speed.exe> <gameplay.bin> <outdir>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char* g_fn; static unsigned char* g_src; static unsigned char* g_dst;
static void callit(void) {
    unsigned char* s = g_src; unsigned char* d = g_dst; unsigned char* fn = g_fn;
    __asm {
        mov eax, s
        push d
        mov ecx, fn
        call ecx
        add esp, 4
    }
}
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* tmp = malloc(n); fread(tmp, 1, n, f); fclose(f);
    static unsigned int zero = 0;
    unsigned char* code = (unsigned char*)VirtualAlloc(0, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    memcpy(code, tmp + 0x24DCA0, 0xE00);
    for (int i = 0; i + 4 <= 0xE00; ++i) if (*(unsigned int*)(code + i) == 0x92592C) *(unsigned int**)(code + i) = &zero;
    g_fn = code;
    f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long gn = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* g = malloc(gn); fread(g, 1, gn, f); fclose(f);
    int count = 0;
    for (long p = 0; p + 16 < gn; ++p) {
        if (memcmp(g + p, "HUFF\x01\x10", 6)) continue;
        unsigned int outsz = *(unsigned int*)(g + p + 8);
        if (outsz == 0 || outsz > 4000000) continue;
        g_src = g + p + 16; g_dst = calloc(1, outsz + 64);
        __try { callit(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("fault at %lx\n", p); continue; }
        char path[512]; sprintf(path, "%s\\%07lx.bin", argv[3], p);
        FILE* o = fopen(path, "wb"); fwrite(g_dst, 1, outsz, o); fclose(o);
        free(g_dst); ++count;
    }
    printf("%d blocks\n", count);
    return 0;
}

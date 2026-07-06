#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "endian_utils.h"

#include <mint/osbind.h>
#define gemdos_cconin() ((void)Cconin())


#define INST_PATTERN 0x48412281  // swap d1; move.l d1,(a1)
#define STUB_PATTERN 0x6C847566  // "läuf" <- inject code here
#define MAX_PATCH_SITES 2
#define HEADER_SIZE 28

static const uint16_t patch_stub_code[] = {
    0x3281,             // move.w d1,(a1)
    0x4841,             // swap d1
    0x3341, 0x0002,     // move.w d1,2(a1)
    0x4E75              // rts
};

struct __attribute__((packed, scalar_storage_order("big-endian"))) opcode16_offset16 {
    uint16_t opcode;
    int16_t offset;
};

static inline bool apply_bsr_patch(uint8_t *data, uint32_t site_offset, uint32_t target_runtime_offset, const char *label) {
    if (site_offset & 1) {
        printf("%s: Patch site not word-aligned (0x%X)\n", label, (int)site_offset);
        return false;
    }

    uint32_t site_runtime_offset = site_offset - HEADER_SIZE;
    int32_t rel = (int32_t)target_runtime_offset - (int32_t)(site_runtime_offset + 2);

    if (rel < INT16_MIN || rel > INT16_MAX) {
        printf("%s: Relative offset %d out of range for bsr.w\n", label, (int)rel);
        return false;
    }

    struct opcode16_offset16 instr = {
        .opcode = 0x6100,
        .offset = (int16_t)rel
    };

    *(struct opcode16_offset16 *)&data[site_offset] = instr;

    printf("%s: Patched bsr.w at file offset 0x%X (rel %d)\n", label, (int)site_offset, (int)rel);
    return true;
}

static void write_patch_stub(uint8_t *data, uint32_t offset) {
    memcpy(&data[offset], patch_stub_code, sizeof(patch_stub_code));
    printf("Replacement code written at file offset: 0x%X\r\n", (unsigned int)offset);
}

static uint32_t find_aligned_pattern(const uint8_t *data, size_t len, uint32_t pattern, int alignment) {
    for (size_t i = 0; i <= len - 4; i += (alignment ? alignment : 1)) {
        if (*(const uint32_t *)&data[i] == pattern)
            return (uint32_t)i;
    }
    return UINT32_MAX;
}

uint32_t find_string(const uint8_t *data, size_t size) {
    const uint8_t prefix = (STUB_PATTERN >> 24) & 0xFF;  // optimization: first byte of pattern

    for (uint32_t i = 0; i <= size - 4; ++i) {
        if (data[i] != prefix)
            continue;

        uint32_t val = (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3];
        if (val == STUB_PATTERN) {
            uint32_t aligned = (i & 1) ? i + 1 : i;

            if (aligned + 8 > size) {
                printf("site too close to EOF, no room for code\r\n");
                return UINT32_MAX;
            }

            printf("Found string pattern at offset 0x%X (aligned: 0x%X)\r\n", (unsigned int)i, (unsigned int)aligned);
            return aligned;
        }
    }

    printf("Target string pattern not found\r\n");
    return UINT32_MAX;
}

// the version string, e.g. "12.61", directly precedes this marker
static const char version_marker[] = "HDDRIVER\0Uwe Seimet";

static int hddriver_version(const uint8_t *data, size_t size) {
    for (size_t i = 1; i + sizeof(version_marker) <= size; i++) {
        if (memcmp(&data[i], version_marker, sizeof(version_marker)) != 0)
            continue;

        const char *ver = (const char *)&data[i - 1];   // NUL ending the version string
        while (ver > (const char *)data &&
               ((ver[-1] >= '0' && ver[-1] <= '9') || ver[-1] == '.'))
            ver--;

        printf("HDDRIVER %s detected\r\n", ver);
        const char *minor = strchr(ver, '.');
        return atoi(ver) * 100 + (minor ? atoi(minor + 1) : 0);
    }

    printf("HDDRIVER version not found\r\n");
    return 0;
}

/*
 * IDE copy-loop fix.
 *
 * HDDRIVER (verified on 12.70 - 13.01) transfers IDE sectors with a 128x
 * unrolled move loop, rewritten at driver startup by self-modifying code
 * (SMC) into 128x 'move.l' on Atari IDE. The resulting loop body (256
 * bytes of moves + 4-byte dbf) does not fit the 68030's 256-byte
 * instruction cache, so every iteration refetches code from RAM.
 *
 * This patch shrinks both loops (read and write) to 32 moves:
 *
 *   before (as shipped / after SMC):     after (as shipped / after SMC):
 *     add.l  d1,d1    / subq.l #1,d1       lsl.l  #3,d1  /  lsl.l #2,d1
 *     subq.l #1,d1                         subq.l #1,d1
 *     128x move.w     / 128x move.l        32x move.w    /  32x move.l
 *     dbf    d1,loop                       dbf    d1,loop
 *     rts                                  rts
 */

#define LOOP_ENTRY_PATTERN 0xD2815381  // add.l d1,d1; subq.l #1,d1
#define MOVEW_READ_PATTERN  0x30D1     // move.w (a1),(a0)+
#define MOVEW_WRITE_PATTERN 0x3298     // move.w (a0)+,(a1)
#define LOOP_UNROLL_OLD  128
#define LOOP_UNROLL_NEW  32

// first dead byte inside a patched loop, right after the new dbf + rts
#define LOOP_DEAD_OFFSET (10 + 2 * LOOP_UNROLL_NEW)

/*
 * The SMC routine (20 words), preceded by 'lea <read loop>(pc),a0' and
 * 'lea <write loop>(pc),a1'. Two variants exist: 12.7x (d0 as scratch),
 * 13.0x (d1). The replacement keeps the variant's register: it writes
 * 'lsl.l #2,d1' over 'add.l d1,d1', skips the 'subq.l #1,d1', then
 * writes 32x move.l.
 */
#define SMC_WORDS 20

struct smc_variant {
    const char *name;
    uint16_t old[SMC_WORDS];
    uint16_t rep[SMC_WORDS];
};

static const struct smc_variant smc_variants[] = {
    { "12.7x",
      { 0x303C, 0x5381,           // move.w #<subq.l #1,d1>,d0
        0x32C0, 0x30C0,           // move.w d0,(a1)+ / d0,(a0)+
        0x707F,                   // moveq #127,d0
        0x32FC, 0x2298,           // move.w #<move.l (a0)+,(a1)>,(a1)+
        0x30FC, 0x20D1,           // move.w #<move.l (a1),(a0)+>,(a0)+
        0x51C8, 0xFFF6,           // dbf d0,.-8
        0x203C, 0x51C9, 0xFEFE,   // move.l #<dbf d1,.-258>,d0
        0x22C0, 0x20C0,           // move.l d0,(a1)+ / d0,(a0)+
        0x303C, 0x4E75,           // move.w #<rts>,d0
        0x3280, 0x3080 },         // move.w d0,(a1) / d0,(a0)
      { 0x303C, 0xE589,           // move.w #<lsl.l #2,d1>,d0
        0x32C0, 0x30C0,           // move.w d0,(a1)+ / d0,(a0)+
        0x5488, 0x5489,           // addq.l #2,a0 / #2,a1: keep the subq.l
        0x701F,                   // moveq #31,d0
        0x32FC, 0x2298,           // move.w #<move.l (a0)+,(a1)>,(a1)+
        0x30FC, 0x20D1,           // move.w #<move.l (a1),(a0)+>,(a0)+
        0x51C8, 0xFFF6,           // dbf d0,.-8
        0x4E71, 0x4E71, 0x4E71,   // nop
        0x4E71, 0x4E71, 0x4E71,
        0x4E71 } },
    { "13.0x",                   // as 12.7x, but d1 instead of d0
      { 0x323C, 0x5381,           // move.w #<subq.l #1,d1>,d1
        0x32C1, 0x30C1,           // move.w d1,(a1)+ / d1,(a0)+
        0x727F,                   // moveq #127,d1
        0x32FC, 0x2298,           // move.w #<move.l (a0)+,(a1)>,(a1)+
        0x30FC, 0x20D1,           // move.w #<move.l (a1),(a0)+>,(a0)+
        0x51C9, 0xFFF6,           // dbf d1,.-8
        0x223C, 0x51C9, 0xFEFE,   // move.l #<dbf d1,.-258>,d1
        0x22C1, 0x20C1,           // move.l d1,(a1)+ / d1,(a0)+
        0x323C, 0x4E75,           // move.w #<rts>,d1
        0x3281, 0x3081 },         // move.w d1,(a1) / d1,(a0)
      { 0x323C, 0xE589,           // move.w #<lsl.l #2,d1>,d1
        0x32C1, 0x30C1,           // move.w d1,(a1)+ / d1,(a0)+
        0x5488, 0x5489,           // addq.l #2,a0 / #2,a1: keep the subq.l
        0x721F,                   // moveq #31,d1
        0x32FC, 0x2298,           // move.w #<move.l (a0)+,(a1)>,(a1)+
        0x30FC, 0x20D1,           // move.w #<move.l (a1),(a0)+>,(a0)+
        0x51C9, 0xFFF6,           // dbf d1,.-8
        0x4E71, 0x4E71, 0x4E71,   // nop
        0x4E71, 0x4E71, 0x4E71,
        0x4E71 } },
};

// find 'add.l d1,d1 / subq.l #1,d1 / 128x <move> / dbf d1,.-258 / rts'
static uint32_t find_ide_loop(const uint8_t *data, size_t size, uint16_t move_opcode, const char *label) {
    for (size_t i = 0; i + 4 + 2 * (LOOP_UNROLL_OLD + 3) <= size; i += 2) {
        if (*(const uint32_t *)&data[i] != LOOP_ENTRY_PATTERN)
            continue;

        const uint16_t *w = (const uint16_t *)&data[i + 4];
        int j;
        for (j = 0; j < LOOP_UNROLL_OLD; j++)
            if (w[j] != move_opcode)
                break;
        if (j < LOOP_UNROLL_OLD)
            continue;
        if (w[LOOP_UNROLL_OLD] != 0x51C9 || w[LOOP_UNROLL_OLD + 1] != 0xFEFE ||
            w[LOOP_UNROLL_OLD + 2] != 0x4E75)
            continue;

        return (uint32_t)i;
    }

    printf("%s not found\r\n", label);
    return UINT32_MAX;
}

static uint32_t find_ide_smc(const uint8_t *data, size_t size, const struct smc_variant **variant) {
    for (size_t i = 0; i + sizeof(smc_variants[0].old) <= size; i += 2) {
        for (size_t v = 0; v < sizeof(smc_variants) / sizeof(smc_variants[0]); v++) {
            if (memcmp(&data[i], smc_variants[v].old, sizeof(smc_variants[v].old)) == 0) {
                *variant = &smc_variants[v];
                return (uint32_t)i;
            }
        }
    }

    printf("IDE SMC routine not found\r\n");
    return UINT32_MAX;
}

// target of 'lea <disp>(pc),aX' located at file offset off
static uint32_t lea_target(const uint8_t *data, uint32_t off) {
    return off + 2 + *(const int16_t *)&data[off + 2];
}

static void patch_ide_loop(uint8_t *data, uint32_t off) {
    uint16_t *w = (uint16_t *)&data[off];

    // add.l d1,d1 -> lsl.l #3,d1 (sectors -> groups of 32 words);
    // the SMC turns this into lsl.l #2,d1 (groups of 32 longs)
    w[0] = 0xE789;
    // dbf d1,.-66 / rts over moves 33-35
    w[2 + LOOP_UNROLL_NEW] = 0x51C9;
    w[3 + LOOP_UNROLL_NEW] = (uint16_t)-(2 * LOOP_UNROLL_NEW + 2);
    w[4 + LOOP_UNROLL_NEW] = 0x4E75;
}

// returns the read loop's file offset, UINT32_MAX when nothing was patched
static uint32_t apply_ide_loop_fix(uint8_t *data, size_t size) {
    const struct smc_variant *variant = NULL;

    uint32_t read_loop = find_ide_loop(data, size, MOVEW_READ_PATTERN, "IDE read loop");
    uint32_t write_loop = find_ide_loop(data, size, MOVEW_WRITE_PATTERN, "IDE write loop");
    uint32_t smc = find_ide_smc(data, size, &variant);

    if (read_loop == UINT32_MAX || write_loop == UINT32_MAX || smc == UINT32_MAX)
        return UINT32_MAX;

    // the two lea's right before the SMC routine must point at the loops
    if (*(const uint16_t *)&data[smc - 8] != 0x41FA ||    // lea <read loop>(pc),a0
        *(const uint16_t *)&data[smc - 4] != 0x43FA ||    // lea <write loop>(pc),a1
        lea_target(data, smc - 8) != read_loop ||
        lea_target(data, smc - 4) != write_loop) {
        printf("IDE SMC routine does not reference the loops, refusing to patch\r\n");
        return UINT32_MAX;
    }

    printf("HDDRIVER %s SMC layout detected\r\n", variant->name);
    printf("IDE read loop patched at file offset 0x%X\r\n", (unsigned int)read_loop);
    printf("IDE write loop patched at file offset 0x%X\r\n", (unsigned int)write_loop);
    printf("IDE SMC routine patched at file offset 0x%X\r\n", (unsigned int)smc);

    patch_ide_loop(data, read_loop);
    patch_ide_loop(data, write_loop);
    memcpy(&data[smc], variant->rep, sizeof(variant->rep));

    return read_loop;
}

int main(int argc, char **argv)
{
    printf("\r\nHDDriver STE DMA fix patcher v0.1\r\n\r\n");
    printf("    by Carpet Ritz Crumbs\n\r\n\r\n");
    printf("NB! ALPHA LEVEL SOFTWARE \r\nBACK YOUR FILES UP BEFORE USE!\r\n\r\n");
    printf("   Educational use only.\r\n\r\n");

    if (argc != 3) {
        printf("\r\nUsage: %s original.sys patched.sys\r\n", argv[0]);
        goto error;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) {
        printf("Can't open input file: %s\r\n", argv[1]);
        goto error;
    }

    fseek(fin, 0, SEEK_END);
    long insize = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *data = malloc(insize);
    if (!data) {
        printf("malloc failed\r\n");
        fclose(fin);
        goto error;
    }

    fread(data, 1, insize, fin);
    fclose(fin);

    if (read_be16(data) != 0x601A) {
        printf("Not a GEMDOS executable file\r\n");
        free(data);
        goto error;
    }

    if (hddriver_version(data, insize) < 1270) {
        printf("Only HDDRIVER 12.70 or newer is supported\r\n");
        free(data);
        goto error;
    }

    uint32_t ide_read_loop = apply_ide_loop_fix(data, insize);
    if (ide_read_loop == UINT32_MAX)
        printf("IDE copy-loop fix skipped\r\n");

    uint32_t patch1_offset = find_aligned_pattern(data, insize, INST_PATTERN, 2);
    if (patch1_offset == UINT32_MAX) {
        printf("First occurrence of problematic code not found\r\n");
        free(data);
        goto error;
    }

    uint32_t patch2_offset = find_aligned_pattern(data + patch1_offset + 4,
                                              insize - patch1_offset - 4,
                                              INST_PATTERN, 2);
    if (patch2_offset == UINT32_MAX) {
        printf("Second occurrence of problematic code not found\r\n");
        free(data);
        goto error;
    }

    patch2_offset += patch1_offset + 4;

    uint32_t stub_offset = find_string(data, insize);       // file offset
    uint32_t stub_runtime_offset = stub_offset - HEADER_SIZE;

    apply_bsr_patch(data, patch1_offset, stub_runtime_offset, "#1 move.l -> double move.w");
    apply_bsr_patch(data, patch2_offset, stub_runtime_offset, "#2 move.l -> double move.w");
    write_patch_stub(data, stub_offset);

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) {
        printf("Can't open output file: %s\r\n", argv[2]);
        free(data);
        goto error;
    }

    fwrite(data, 1, insize, fout);
    fclose(fout);
    free(data);

    printf("Patched driver written to: %s\r\n", argv[2]);
    gemdos_cconin();
    return 0;

error:
    gemdos_cconin();
    return 1;

}


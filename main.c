#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "endian_utils.h"

#include <mint/osbind.h>
#define gemdos_cconin() ((void)Cconin())


#define INST_PATTERN 0x484121C1  // swap d1; move.l d1,$ffff8604.w (6 bytes, 12.70+ encoding)
#define MAX_PATCH_SITES 2
#define HEADER_SIZE 28

static const uint16_t patch_stub_code[] = {
    0x31C1, 0x8604,     // move.w d1,$ffff8604.w
    0x4841,             // swap d1
    0x31C1, 0x8606,     // move.w d1,$ffff8606.w
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
    printf("STE DMA fix, stub written at file offset 0x%X\r\n", (unsigned int)offset);
}

static uint32_t find_aligned_pattern(const uint8_t *data, size_t len, uint32_t pattern, int alignment) {
    for (size_t i = 0; i <= len - 4; i += (alignment ? alignment : 1)) {
        if (*(const uint32_t *)&data[i] == pattern)
            return (uint32_t)i;
    }
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
static uint32_t apply_ide_loop_fix(uint8_t *data, size_t size, uint32_t *write_loop_out) {
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

    *write_loop_out = write_loop;
    return read_loop;
}

/*
 * 040/060 cache flush fix.
 *
 * The routine called after ACSI/SCSI DMA transfers (5 call sites)
 * invalidates the caches so the CPU sees the memory the DMA chip wrote
 * behind their back. It only knows the 020/030 CACR: the or.w #$0808
 * hits reserved bits on 040/060, nothing is flushed there and stale
 * cache lines over the DMA buffer survive. Route the routine through
 * new code in the dead bytes the IDE fix freed inside the write loop:
 * 'cpusha bc' on 040/060 (the CPU type variable exists since 12.70),
 * the original CACR code below.
 */

// move.b <flag>(pc),d2; beq.s <rts>; followed by:
static const uint16_t flush_tail[] = {
    0x40E7,             // move.w sr,-(sp)
    0x007C, 0x0700,     // ori.w #$0700,sr
    0x4E7A, 0x2002,     // movec cacr,d2
    0x847C, 0x0808,     // or.w #$0808,d2 (030: clear I+D cache)
    0x4E7B, 0x2002,     // movec d2,cacr
    0x46DF,             // move.w (sp)+,sr
    0x4E75              // rts
};

static uint32_t find_flush_routine(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + 6 + sizeof(flush_tail) <= size; i += 2) {
        const uint16_t *w = (const uint16_t *)&data[i];
        if (w[0] != 0x143A ||                 // move.b <flag>(pc),d2
            (w[2] & 0xFF00) != 0x6700 ||      // beq.s
            memcmp(&w[3], flush_tail, sizeof(flush_tail)) != 0)
            continue;
        return (uint32_t)i;
    }

    printf("Cache flush routine not found\r\n");
    return UINT32_MAX;
}

/*
 * The CPU type variable, located via the 'cpusha bc' dispatch following
 * the SMC routine:
 *   12.70/12.71: move.w <cputype>(pc),d0; cmp.w #40,d0; bcs ...
 *   12.72+:      moveq #39,d0; sub.w <cputype>(pc),d0; bcc ...
 */
static uint32_t find_cputype(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + 12 <= size; i += 2) {
        const uint16_t *w = (const uint16_t *)&data[i];
        if (w[0] == 0x303A && w[2] == 0xB07C && w[3] == 0x0028 &&
            (w[4] & 0xFF00) == 0x6500)
            return (uint32_t)(i + 2) + *(const int16_t *)&data[i + 2];
        if (w[0] == 0x7027 && w[1] == 0x907A && (w[3] & 0xFF00) == 0x6400)
            return (uint32_t)(i + 4) + *(const int16_t *)&data[i + 4];
    }

    printf("CPU type variable not found\r\n");
    return UINT32_MAX;
}

static bool apply_flush_fix(uint8_t *data, size_t size, uint32_t write_loop) {
    uint32_t flush = find_flush_routine(data, size);
    uint32_t cputype = find_cputype(data, size);
    if (flush == UINT32_MAX || cputype == UINT32_MAX)
        return false;

    uint32_t flag = flush + 2 + *(const int16_t *)&data[flush + 2];
    uint32_t code = write_loop + LOOP_DEAD_OFFSET;
    uint16_t *w = (uint16_t *)&data[code];

    w[0] = 0x343A;                                    // move.w <cputype>(pc),d2
    w[1] = (uint16_t)(cputype - (code + 2));
    w[2] = 0x0C42;                                    // cmpi.w #40,d2
    w[3] = 0x0028;
    w[4] = 0x6504;                                    // bcs.s .old
    w[5] = 0xF4F8;                                    // cpusha bc
    w[6] = 0x4E75;                                    // rts
    w[7] = 0x143A;                                    // .old: move.b <flag>(pc),d2
    w[8] = (uint16_t)(flag - (code + 16));
    w[9] = 0x6704;                                    // beq.s .ret
    w[10] = 0x6000;                                   // bra.w <original CACR code>
    w[11] = (uint16_t)(flush + 6 - (code + 22));
    w[12] = 0x4E75;                                   // .ret: rts

    // route the original routine through the new code
    w = (uint16_t *)&data[flush];
    w[0] = 0x6000;                                    // bra.w <new code>
    w[1] = (uint16_t)(code - (flush + 2));

    printf("Cache flush routine patched at file offset 0x%X\r\n", (unsigned int)flush);
    printf("cpusha dispatch written at file offset 0x%X\r\n", (unsigned int)code);
    return true;
}

int main(int argc, char **argv)
{
    printf("\r\nHDDriver STE DMA fix patcher v0.2\r\n\r\n");
    printf("    by Carpet Ritz Crumbs\n\r\n\r\n");
    printf("NB! ALPHA LEVEL SOFTWARE \r\nBACK YOUR FILES UP BEFORE USE!\r\n\r\n");
    printf("   Educational use only.\r\n\r\n");

    if (argc != 2) {
        printf("\r\nUsage: DMAPATCH.TTP X:\\HDDRIVER.SYS\r\n");
        printf("(or drag & drop your HDDRIVER.SYS / HDDRIVER.PRG onto this program)\r\n");
        printf("The original driver is kept as *.BAK\r\n");
        goto error;
    }

    size_t len = strlen(argv[1]);
    if (len < 4 || (stricmp(&argv[1][len - 4], ".sys") != 0 &&
                    stricmp(&argv[1][len - 4], ".prg") != 0)) {
        printf("%s is neither a .SYS nor a .PRG file\r\n", argv[1]);
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

    uint32_t ide_write_loop = UINT32_MAX;
    uint32_t ide_read_loop = apply_ide_loop_fix(data, insize, &ide_write_loop);
    if (ide_read_loop == UINT32_MAX)
        printf("IDE copy-loop fix skipped\r\n");

    // STE DMA fix: the stub code goes into the dead bytes the IDE fix
    // freed inside the read loop
    bool dma_patched = false;
    uint32_t patch1_offset = find_aligned_pattern(data, insize, INST_PATTERN, 2);
    uint32_t patch2_offset = UINT32_MAX;
    if (patch1_offset == UINT32_MAX) {
        printf("First occurrence of problematic code not found\r\n");
    } else {
        patch2_offset = find_aligned_pattern(data + patch1_offset + 4,
                                              insize - patch1_offset - 4,
                                              INST_PATTERN, 2);
        if (patch2_offset == UINT32_MAX)
            printf("Second occurrence of problematic code not found\r\n");
    }

    if (patch1_offset != UINT32_MAX && patch2_offset != UINT32_MAX && ide_read_loop != UINT32_MAX) {
        patch2_offset += patch1_offset + 4;

        uint32_t stub_offset = ide_read_loop + LOOP_DEAD_OFFSET;
        uint32_t stub_runtime_offset = stub_offset - HEADER_SIZE;

        if (*(const uint16_t *)&data[patch1_offset + 4] != 0x8604 ||
            *(const uint16_t *)&data[patch2_offset + 4] != 0x8604) {
            printf("Problematic code does not write to $FFFF8604, refusing to patch\r\n");
        } else {
            dma_patched = apply_bsr_patch(data, patch1_offset, stub_runtime_offset, "STE DMA fix, site #1");
            dma_patched &= apply_bsr_patch(data, patch2_offset, stub_runtime_offset, "STE DMA fix, site #2");
            // the 6-byte sites leave one word after the bsr.w
            *(uint16_t *)&data[patch1_offset + 4] = 0x4E71;    // nop
            *(uint16_t *)&data[patch2_offset + 4] = 0x4E71;
            write_patch_stub(data, stub_offset);
        }
    }
    if (!dma_patched)
        printf("STE DMA fix skipped\r\n");

    // 040/060 cache flush fix: the dispatch goes into the dead bytes the
    // IDE fix freed inside the write loop
    bool flush_patched = false;
    if (ide_read_loop != UINT32_MAX)
        flush_patched = apply_flush_fix(data, insize, ide_write_loop);
    if (!flush_patched)
        printf("040/060 cache flush fix skipped\r\n");

    if (!dma_patched && ide_read_loop == UINT32_MAX) {
        printf("Nothing patched, no output written\r\n");
        free(data);
        goto error;
    }

    // backup name: input path with the extension replaced by .BAK
    char *bakname = malloc(strlen(argv[1]) + 1);
    if (!bakname) {
        printf("malloc failed\r\n");
        free(data);
        goto error;
    }
    strcpy(bakname, argv[1]);
    strcpy(&bakname[len - 4], ".BAK");

    if (rename(argv[1], bakname) != 0) {
        printf("Can't rename %s to %s\r\n", argv[1], bakname);
        printf("(is there a .BAK file from a previous run? delete it first)\r\n");
        free(bakname);
        free(data);
        goto error;
    }

    FILE *fout = fopen(argv[1], "wb");
    if (!fout || fwrite(data, 1, insize, fout) != (size_t)insize) {
        if (fout) {
            fclose(fout);
            remove(argv[1]);                                 // half-written file
        }
        rename(bakname, argv[1]);                            // restore the original
        printf("Can't write %s, original restored\r\n", argv[1]);
        free(bakname);
        free(data);
        goto error;
    }
    fclose(fout);
    free(data);

    printf("Original driver kept as: %s\r\n", bakname);
    printf("Patched driver written to: %s\r\n", argv[1]);
    free(bakname);
    gemdos_cconin();
    return 0;

error:
    gemdos_cconin();
    return 1;

}


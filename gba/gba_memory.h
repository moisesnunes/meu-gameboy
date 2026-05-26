#ifndef _GBA_MEMORY_H_
#define _GBA_MEMORY_H_

#include <stdint.h>

struct gba;

/* GBA memory map regions */
#define GBA_BIOS_BASE    0x00000000U
#define GBA_BIOS_SIZE    0x00004000U  /* 16KB */
#define GBA_EWRAM_BASE   0x02000000U
#define GBA_EWRAM_SIZE   0x00040000U  /* 256KB */
#define GBA_IWRAM_BASE   0x03000000U
#define GBA_IWRAM_SIZE   0x00008000U  /* 32KB */
#define GBA_IO_BASE      0x04000000U
#define GBA_IO_SIZE      0x00000400U  /* 1KB */
#define GBA_PAL_BASE     0x05000000U
#define GBA_PAL_SIZE     0x00000400U  /* 1KB = 512 colors × 2B */
#define GBA_VRAM_BASE    0x06000000U
#define GBA_VRAM_SIZE    0x00018000U  /* 96KB */
#define GBA_OAM_BASE     0x07000000U
#define GBA_OAM_SIZE     0x00000400U  /* 1KB = 128 sprites × 8B */
#define GBA_ROM_BASE     0x08000000U
#define GBA_ROM_MAX_SIZE 0x02000000U  /* 32MB */
#define GBA_SRAM_BASE    0x0E000000U
#define GBA_SRAM_SIZE    0x00008000U  /* 32KB */

/* I/O register addresses */
#define REG_DISPCNT     0x04000000U
#define REG_GREENSWAP   0x04000002U
#define REG_DISPSTAT    0x04000004U
#define REG_VCOUNT      0x04000006U
#define REG_BG0CNT      0x04000008U
#define REG_BG1CNT      0x0400000AU
#define REG_BG2CNT      0x0400000CU
#define REG_BG3CNT      0x0400000EU
#define REG_BG0HOFS     0x04000010U
#define REG_BG0VOFS     0x04000012U
#define REG_BG1HOFS     0x04000014U
#define REG_BG1VOFS     0x04000016U
#define REG_BG2HOFS     0x04000018U
#define REG_BG2VOFS     0x0400001AU
#define REG_BG3HOFS     0x0400001CU
#define REG_BG3VOFS     0x0400001EU
#define REG_BG2PA       0x04000020U
#define REG_BG2PB       0x04000022U
#define REG_BG2PC       0x04000024U
#define REG_BG2PD       0x04000026U
#define REG_BG2X        0x04000028U
#define REG_BG2Y        0x0400002CU
#define REG_BG3PA       0x04000030U
#define REG_BG3PB       0x04000032U
#define REG_BG3PC       0x04000034U
#define REG_BG3PD       0x04000036U
#define REG_BG3X        0x04000038U
#define REG_BG3Y        0x0400003CU
#define REG_WIN0H       0x04000040U
#define REG_WIN1H       0x04000042U
#define REG_WIN0V       0x04000044U
#define REG_WIN1V       0x04000046U
#define REG_WININ       0x04000048U
#define REG_WINOUT      0x0400004AU
#define REG_MOSAIC      0x0400004CU
#define REG_BLDCNT      0x04000050U
#define REG_BLDALPHA    0x04000052U
#define REG_BLDY        0x04000054U
#define REG_SOUND1CNT_L 0x04000060U
#define REG_SOUND1CNT_H 0x04000062U
#define REG_SOUND1CNT_X 0x04000064U
#define REG_SOUND2CNT_L 0x04000068U
#define REG_SOUND2CNT_H 0x0400006CU
#define REG_SOUND3CNT_L 0x04000070U
#define REG_SOUND3CNT_H 0x04000072U
#define REG_SOUND3CNT_X 0x04000074U
#define REG_SOUND4CNT_L 0x04000078U
#define REG_SOUND4CNT_H 0x0400007CU
#define REG_SOUNDCNT_L  0x04000080U
#define REG_SOUNDCNT_H  0x04000082U
#define REG_SOUNDCNT_X  0x04000084U
#define REG_SOUNDBIAS   0x04000088U
#define REG_WAVE_RAM    0x04000090U
#define REG_FIFO_A      0x040000A0U
#define REG_FIFO_B      0x040000A4U
#define REG_DMA0SAD     0x040000B0U
#define REG_DMA0DAD     0x040000B4U
#define REG_DMA0CNT_L   0x040000B8U
#define REG_DMA0CNT_H   0x040000BAU
#define REG_DMA1SAD     0x040000BCU
#define REG_DMA1DAD     0x040000C0U
#define REG_DMA1CNT_L   0x040000C4U
#define REG_DMA1CNT_H   0x040000C6U
#define REG_DMA2SAD     0x040000C8U
#define REG_DMA2DAD     0x040000CCU
#define REG_DMA2CNT_L   0x040000D0U
#define REG_DMA2CNT_H   0x040000D2U
#define REG_DMA3SAD     0x040000D4U
#define REG_DMA3DAD     0x040000D8U
#define REG_DMA3CNT_L   0x040000DCU
#define REG_DMA3CNT_H   0x040000DEU
#define REG_TM0CNT_L    0x04000100U
#define REG_TM0CNT_H    0x04000102U
#define REG_TM1CNT_L    0x04000104U
#define REG_TM1CNT_H    0x04000106U
#define REG_TM2CNT_L    0x04000108U
#define REG_TM2CNT_H    0x0400010AU
#define REG_TM3CNT_L    0x0400010CU
#define REG_TM3CNT_H    0x0400010EU
#define REG_SIODATA32   0x04000120U
#define REG_SIOCNT      0x04000128U
#define REG_KEYINPUT    0x04000130U
#define REG_KEYCNT      0x04000132U
#define REG_RCNT        0x04000134U
#define REG_IE          0x04000200U
#define REG_IF          0x04000202U
#define REG_WAITCNT     0x04000204U
#define REG_IME         0x04000208U
#define REG_POSTFLG     0x04000300U
#define REG_HALTCNT     0x04000301U

uint8_t  gba_memory_read8(struct gba *gba, uint32_t addr);
uint16_t gba_memory_read16(struct gba *gba, uint32_t addr);
uint32_t gba_memory_read32(struct gba *gba, uint32_t addr);
uint16_t gba_memory_fetch16(struct gba *gba, uint32_t addr, bool sequential);
uint32_t gba_memory_fetch32(struct gba *gba, uint32_t addr, bool sequential);

void gba_memory_write8(struct gba *gba, uint32_t addr, uint8_t val);
void gba_memory_write16(struct gba *gba, uint32_t addr, uint16_t val);
void gba_memory_write32(struct gba *gba, uint32_t addr, uint32_t val);

/* Peek: read without side-effects (for debugger) */
uint8_t  gba_memory_peek8(struct gba *gba, uint32_t addr);
uint16_t gba_memory_peek16(struct gba *gba, uint32_t addr);
uint32_t gba_memory_peek32(struct gba *gba, uint32_t addr);

void gba_memory_reset(struct gba *gba);

#endif /* _GBA_MEMORY_H_ */

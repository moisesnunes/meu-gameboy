#ifndef _GB_RTC_H_
#define _GB_RTC_H_

/*
 * Registradores de data/hora do RTC do MBC3.
 * Mapeados nos registradores 0x08–0x0C do cartucho.
 */
struct gb_rtc_date
{
     uint8_t s;  /* Segundos (0–59)  — registrador 0x08 */
     uint8_t m;  /* Minutos  (0–59)  — registrador 0x09 */
     uint8_t h;  /* Horas    (0–23)  — registrador 0x0A */
     uint8_t dl; /* Dias, 8 bits baixos (0–255) — registrador 0x0B */
     /* Dias MSB + flags — registrador 0x0C:
      *   bit 0: bit 8 do contador de dias
      *   bit 6: HALT — congela o RTC quando 1
      *   bit 7: carry — setado quando o contador passa de 511, limpo pela ROM */
     uint8_t dh;
};

struct gb_rtc
{
     /* Timestamp Unix correspondente ao instante "dia 0, 00:00:00" do RTC.
      * O tempo decorrido é calculado como (now - base). */
     uint64_t base;
     /* Timestamp Unix no momento em que o HALT foi ativado.
      * Enquanto halted, o tempo decorrido é medido até este ponto. */
     uint64_t halt_date;
     /* Estado atual do latch (escrito pela ROM em 0x6000–0x7FFF).
      * A data é capturada na transição 0→1. */
     bool latch;
     /* Snapshot da data/hora capturado no último latch */
     struct gb_rtc_date latched_date;
};

void gb_rtc_init(struct gb *gb);
void gb_rtc_latch(struct gb *gb, bool latch);
uint8_t gb_rtc_read(struct gb *gb, unsigned r);
void gb_rtc_write(struct gb *gb, unsigned r, uint8_t v);
void gb_rtc_dump(struct gb *gb, FILE *f);
void gb_rtc_load(struct gb *gb, FILE *f);

#endif /* _GB_RTC_H_ */

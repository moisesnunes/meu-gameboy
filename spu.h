#ifndef _SPU_H_
#define _SPU_H_

/* A APU roda a ~4.2 MHz; gerar uma amostra por ciclo seria caro demais.
 * Em vez disso, geramos uma amostra a cada GB_SPU_SAMPLE_RATE_DIVISOR ciclos. */
#define GB_SPU_SAMPLE_RATE_DIVISOR 64

/* Taxa de amostragem efetiva entregue ao frontend */
#define GB_SPU_SAMPLE_RATE_HZ (GB_CPU_FREQ_HZ / GB_SPU_SAMPLE_RATE_DIVISOR)

/* Número de frames de áudio por buffer. Cada frame = 2 amostras (L + R estéreo). */
#define GB_SPU_SAMPLE_BUFFER_LENGTH 2048

/* Número de buffers no anel de double-buffering entre a APU e o frontend */
#define GB_SPU_SAMPLE_BUFFER_COUNT 2

/* Tamanho da RAM de onda do canal 3 (CH3) em bytes — 32 amostras de 4 bits */
#define GB_NR3_RAM_SIZE 16

struct gb_spu_sample_buffer
{
     /* Pares de amostras estéreo [L, R] */
     int16_t samples[GB_SPU_SAMPLE_BUFFER_LENGTH][2];
     /* Sinaliza que o frontend terminou de enviar o buffer e ele está livre
      * para ser preenchido novamente pela APU. */
     sem_t free;
     /* Sinaliza que a APU terminou de preencher o buffer e ele está pronto
      * para ser enviado pelo frontend. */
     sem_t ready;
};

/* Duração funciona igual nos 4 canais, mas o valor máximo difere por canal */
#define GB_SPU_NR1_T1_MAX 0x3f
#define GB_SPU_NR2_T1_MAX 0x3f
#define GB_SPU_NR3_T1_MAX 0xff
#define GB_SPU_NR4_T1_MAX 0x3f

struct gb_spu_duration
{
     /* true quando o contador de duração está ativo */
     bool enable;
     /* Ticks restantes; decrementado a 256 Hz pelo frame sequencer */
     uint16_t counter;
};

/*
 * Divisor de frequência dos canais de onda (CH1, CH2, CH3).
 * O período em T-cycles é 2 × (0x800 − offset).
 */
struct gb_spu_divider
{
     /* Valor de frequência escrito pela ROM (0–0x7FF) */
     uint16_t offset;
     /* Contagem regressiva até o próximo tick do gerador de onda */
     uint16_t counter;
};

/*
 * Sweep de frequência do CH1 (registrador NR10).
 * A cada passo, a frequência é somada/subtraída por (shadow_offset >> shift).
 */
struct gb_spu_sweep
{
     /* Divisor de frequência do canal (compartilhado com o gerador de onda) */
     struct gb_spu_divider divider;
     /* Quantidade de bits a deslocar para calcular o delta de frequência */
     uint8_t shift;
     /* true: subtrai o delta; false: soma */
     bool subtract;
     /* Intervalo entre passos em 1/128 s (0 = sweep desabilitado) */
     uint8_t time;
     /* Ticks do frame sequencer até o próximo passo de sweep */
     uint8_t counter;
     /* Cópia da frequência no trigger; modificada a cada passo */
     uint16_t shadow_offset;
     /* true enquanto o sweep está ativo após o trigger */
     bool enable;
     /* true após a primeira cálculo de subtração — necessário para a quirk
      * do hardware que desabilita o canal se subtração foi calculada antes */
     bool subtract_calculated;
};

struct gb_spu_rectangle_wave
{
     /* Fase atual no ciclo de 16 passos (0–15) */
     uint8_t phase;
     /* Índice do duty cycle: 0=1/8, 1=1/4, 2=1/2, 3=3/4 */
     uint8_t duty_cycle;
};

struct gb_spu_envelope
{
     /* Duração de cada passo em ticks de 64 Hz (1/64 s). 0 = envelope parado. */
     uint8_t step_duration;
     /* Volume atual (0–15) */
     uint8_t value;
     /* true: incrementa volume a cada passo; false: decrementa */
     bool increment;
     /* Contagem regressiva até o próximo passo */
     uint8_t counter;
     /* true enquanto o envelope ainda pode alterar o volume (false ao saturar) */
     bool running;
};

/* CH1 — onda retangular com envelope de volume e sweep de frequência (NR10–NR14) */
struct gb_spu_nr1
{
     bool running;                      /* true enquanto o canal está ativo */
     struct gb_spu_duration duration;   /* contador de duração */
     struct gb_spu_sweep sweep;         /* divisor de frequência + sweep */
     struct gb_spu_rectangle_wave wave; /* gerador de onda retangular */
     uint8_t envelope_config;           /* valor bruto de NR12 (relido no trigger) */
     struct gb_spu_envelope envelope;   /* estado ativo do envelope */
};

/* CH2 — onda retangular com envelope de volume, sem sweep (NR21–NR24) */
struct gb_spu_nr2
{
     bool running;                      /* true enquanto o canal está ativo */
     struct gb_spu_duration duration;   /* contador de duração */
     struct gb_spu_divider divider;     /* divisor de frequência */
     struct gb_spu_rectangle_wave wave; /* gerador de onda retangular */
     uint8_t envelope_config;           /* valor bruto de NR22 (relido no trigger) */
     struct gb_spu_envelope envelope;   /* estado ativo do envelope */
};

/* CH3 — forma de onda definida pela ROM (Wave RAM), sem envelope (NR30–NR34) */
struct gb_spu_nr3
{
     /* true quando NR30 bit 7 está setado (DAC habilitado) */
     bool enable;
     /* true enquanto o canal está ativo e gerando amostras */
     bool running;
     struct gb_spu_duration duration; /* contador de duração */
     /* Valor bruto de NR31 — guardado separado pois NR31 é legível */
     uint8_t t1;
     struct gb_spu_divider divider; /* divisor de frequência */
     /* Deslocamento de volume: 0=mudo, 1=100%, 2=50%, 3=25% */
     uint8_t volume_shift;
     /* 32 amostras de 4 bits compactadas em 16 bytes (2 amostras/byte) */
     uint8_t ram[GB_NR3_RAM_SIZE];
     /* Índice da amostra atual (0–31) */
     uint8_t index;
     /* DMG: ciclos restantes da janela de acesso à Wave RAM pela CPU.
      * Abre (= 2) cada vez que a unidade de onda lê um novo byte e conta
      * regressivamente por ciclo. Quando chega a 0, leituras retornam 0xFF
      * e escritas são ignoradas. Sempre 0 no CGB. */
     int32_t access_cycles;
};

/* CH4 — gerador de ruído por LFSR com envelope de volume (NR41–NR44) */
struct gb_spu_nr4
{
     bool running;                    /* true enquanto o canal está ativo */
     struct gb_spu_duration duration; /* contador de duração */
     uint8_t envelope_config;         /* valor bruto de NR42 (relido no trigger) */
     struct gb_spu_envelope envelope; /* estado ativo do envelope */
     /* Registrador de deslocamento com feedback linear (15 bits).
      * Inicializado em 0x7FFF no trigger; bit 0 determina a amostra de saída. */
     uint16_t lfsr;
     /* Valor bruto de NR43: divisor (bits 2:0), modo 7 bits (bit 3), shift (bits 7:4) */
     uint8_t lfsr_config;
     /* Contagem regressiva em T-cycles até o próximo passo do LFSR */
     uint32_t counter;
};

struct gb_spu
{
     /* NR52 bit 7: habilita toda a APU. Quando false, todos os circuitos são
      * resetados e os registradores ficam inacessíveis (exceto a Wave RAM do CH3). */
     bool enable;

     /* Ciclos residuais do último sync que não completaram um período de amostra.
      * Evita acumular erro de fase entre syncs. */
     uint8_t sample_period_frac;

     /* Frame sequencer a 512 Hz: cadência de length/sweep/envelope */
     uint16_t frame_seq_counter; /* contagem regressiva até o próximo tick */
     uint8_t frame_seq_step;     /* passo atual (0–7) */

     /* NR50 (0xFF24): volume mestre de saída L/R (bits 6:4 = L, bits 2:0 = R) */
     uint8_t output_level;
     /* NR51 (0xFF25): roteamento dos 4 canais para saída L/R */
     uint8_t sound_mux;

     /* Fator de amplitude pré-calculado por canal e por saída [canal][0=L,1=R].
      * Recalculado sempre que NR50/NR51 mudam via gb_spu_update_sound_amp(). */
     int16_t sound_amp[4][2];
     /* Mute por canal controlado pelo frontend (UI). Não altera registradores da APU. */
     bool frontend_mute[4];
     /* Saida digital atual dos canais, exposta nos registradores CGB PCM12/PCM34. */
     uint8_t pcm[4];

     struct gb_spu_nr1 nr1; /* CH1 — onda retangular com sweep */
     struct gb_spu_nr2 nr2; /* CH2 — onda retangular */
     struct gb_spu_nr3 nr3; /* CH3 — forma de onda personalizada */
     struct gb_spu_nr4 nr4; /* CH4 — ruído LFSR */

     /* Anel de buffers de áudio trocados com o frontend via semáforos */
     struct gb_spu_sample_buffer buffers[GB_SPU_SAMPLE_BUFFER_COUNT];
     /* Índice do buffer sendo preenchido atualmente */
     unsigned buffer_index;
     /* Posição dentro do buffer atual (0–GB_SPU_SAMPLE_BUFFER_LENGTH-1) */
     unsigned sample_index;
};

void gb_spu_reset(struct gb *gb);
void gb_spu_power_off(struct gb *gb);
void gb_spu_sync(struct gb *gb);
void gb_spu_div_reset(struct gb *gb, uint16_t old_divider);
void gb_spu_update_sound_amp(struct gb *gb);
uint8_t gb_spu_pcm12(struct gb *gb);
uint8_t gb_spu_pcm34(struct gb *gb);
void gb_spu_nr1_start(struct gb *gb);
void gb_spu_nr2_start(struct gb *gb);
void gb_spu_nr3_start(struct gb *gb);
void gb_spu_nr4_start(struct gb *gb);
void gb_spu_duration_reload(struct gb_spu_duration *d,
                            unsigned duration_max,
                            uint8_t t1);
void gb_spu_duration_set_enable(struct gb *gb,
                                struct gb_spu_duration *d,
                                bool enable,
                                bool *running,
                                bool trigger,
                                unsigned length_max);
void gb_spu_sweep_reload(struct gb_spu_sweep *f, uint8_t conf);

#endif /* _SPU_H_ */

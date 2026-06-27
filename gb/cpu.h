#ifndef _GB_CPU_H_
#define _GB_CPU_H_

struct gb_cpu
{
     /* Flag IME (Interrupt Master Enable) */
     bool irq_enable;

     /* Valor do IME quando o atraso do EI terminar */
     bool irq_enable_next;

     /* Instrucoes restantes ate aplicar irq_enable_next */
     uint8_t irq_enable_delay;

     /* Verdadeiro se a CPU está em estado de halt */
     bool halted;

     /* Verdadeiro se a CPU está parada por STOP sem troca de velocidade. */
     bool stopped;

     /* Bug do HALT: quando HALT é executado com IME=0 e há uma IRQ pendente,
      * o próximo fetch de opcode não incrementa o PC (o byte é lido duas vezes) */
     bool halt_bug;

     /* Program Counter */
     uint16_t pc;

     /* Stack Pointer */
     uint16_t sp;

     /* Registrador A */
     uint8_t a;

     /* Registrador B */
     uint8_t b;

     /* Registrador C */
     uint8_t c;

     /* Registrador D */
     uint8_t d;

     /* Registrador E */
     uint8_t e;

     /* Registrador H */
     uint8_t h;

     /* Registrador L */
     uint8_t l;

     /* Flag Zero */
     bool f_z;

     /* Flag Subtração */
     bool f_n;

     /* Flag Half-Carry */
     bool f_h;

     /* Flag Carry */
     bool f_c;

     /* Execution trace ring buffer — last GB_CPU_TRACE_SIZE PC values */
#define GB_CPU_TRACE_SIZE 64
     uint16_t trace_buf[GB_CPU_TRACE_SIZE];
     unsigned trace_head; /* index of the next slot to write */
};

void gb_cpu_reset(struct gb *gb);
int32_t gb_cpu_run_cycles(struct gb *gb, int32_t cycles);

#endif /* _GB_CPU_H_ */

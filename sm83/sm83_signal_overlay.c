#include "sm83_signal_overlay.h"
#include "sm83_node_map.h"
#include "sm83_netlist_data.h"
#include "gb.h"
#include <string.h>

/* Find the instance index in sm83_instances[] whose name equals inst_name.
 * Returns -1 if not found. */
static int find_instance(const char *inst_name)
{
     for (int i = 0; i < SM83_INSTANCE_COUNT; i++)
     {
          if (strcmp(sm83_instances[i].name, inst_name) == 0)
               return i;
     }
     return -1;
}

/* Map signal group from node-map entry index.
 * Must stay in sync with the order of entries in sm83_node_map[]. */
static int signal_group(int map_idx)
{
     if (map_idx < 16)
          return SM83_SIG_GROUP_PC;
     if (map_idx < 24)
          return SM83_SIG_GROUP_A;
     if (map_idx < 32)
          return SM83_SIG_GROUP_B;
     if (map_idx < 40)
          return SM83_SIG_GROUP_IR;
     if (map_idx < 44)
          return SM83_SIG_GROUP_FLAGS;
     if (map_idx < 52)
          return SM83_SIG_GROUP_IDU;
     if (map_idx < 60)
          return SM83_SIG_GROUP_C;
     if (map_idx < 68)
          return SM83_SIG_GROUP_D;
     if (map_idx < 76)
          return SM83_SIG_GROUP_E;
     if (map_idx < 84)
          return SM83_SIG_GROUP_H;
     if (map_idx < 92)
          return SM83_SIG_GROUP_L;
     if (map_idx < 108)
          return SM83_SIG_GROUP_SP;
     return SM83_SIG_GROUP_DBUS;
}

void sm83_overlay_init(Sm83SignalOverlay *ov)
{
     memset(ov, 0, sizeof(*ov));
     int n = SM83_NODE_MAP_COUNT;
     if (n > SM83_OVERLAY_SIGNAL_COUNT)
          n = SM83_OVERLAY_SIGNAL_COUNT;
     ov->count = n;

     for (int i = 0; i < n; i++)
     {
          const Sm83NodeMapEntry *e = &sm83_node_map[i];
          Sm83OverlaySignal *s = &ov->signals[i];
          s->label = e->signal_name;
          s->group = signal_group(i);
          s->value = 0;
          s->fade = 0.0f;

          int inst_idx = find_instance(e->inst_prefix);
          if (inst_idx >= 0)
          {
               s->nx = sm83_instances[inst_idx].nx;
               s->ny = sm83_instances[inst_idx].ny;
               s->valid = true;
          }
          else
          {
               s->valid = false;
          }
     }
}

void sm83_overlay_update(Sm83SignalOverlay *ov, const struct gb *gb)
{
     const struct gb_cpu *cpu = &gb->cpu;

     /* Helper: set signal value and trigger fade if changed */
#define SET_SIG(idx, val)                    \
     do                                      \
     {                                       \
          uint8_t _v = (uint8_t)(val);       \
          if (ov->signals[idx].value != _v)  \
          {                                  \
               ov->signals[idx].value = _v;  \
               ov->signals[idx].fade = 1.0f; \
          }                                  \
     } while (0)

     /* PCL (bits 0-7 of PC) */
     for (int i = 0; i < 8; i++)
          SET_SIG(i, (cpu->pc >> i) & 1);
     /* PCH (bits 8-15 of PC) */
     for (int i = 0; i < 8; i++)
          SET_SIG(8 + i, (cpu->pc >> (8 + i)) & 1);
     /* A */
     for (int i = 0; i < 8; i++)
          SET_SIG(16 + i, (cpu->a >> i) & 1);
     /* B */
     for (int i = 0; i < 8; i++)
          SET_SIG(24 + i, (cpu->b >> i) & 1);
     /* IR - last fetched opcode from debug snapshot */
     uint8_t opcode = gb->debug.cpu_viz.opcode;
     for (int i = 0; i < 8; i++)
          SET_SIG(32 + i, (opcode >> i) & 1);
     /* Flags */
     SET_SIG(40, cpu->f_z ? 1 : 0);
     SET_SIG(41, cpu->f_n ? 1 : 0);
     SET_SIG(42, cpu->f_h ? 1 : 0);
     SET_SIG(43, cpu->f_c ? 1 : 0);
     /* IDU - use addr_bus from debug snapshot as a proxy */
     uint16_t addr = gb->debug.cpu_viz.addr_bus;
     for (int i = 0; i < 8; i++)
          SET_SIG(44 + i, (addr >> i) & 1);
     /* C */
     for (int i = 0; i < 8; i++)
          SET_SIG(52 + i, (cpu->c >> i) & 1);
     /* D */
     for (int i = 0; i < 8; i++)
          SET_SIG(60 + i, (cpu->d >> i) & 1);
     /* E */
     for (int i = 0; i < 8; i++)
          SET_SIG(68 + i, (cpu->e >> i) & 1);
     /* H */
     for (int i = 0; i < 8; i++)
          SET_SIG(76 + i, (cpu->h >> i) & 1);
     /* L */
     for (int i = 0; i < 8; i++)
          SET_SIG(84 + i, (cpu->l >> i) & 1);
     /* SP */
     for (int i = 0; i < 8; i++)
          SET_SIG(92 + i, (cpu->sp >> i) & 1);
     for (int i = 0; i < 8; i++)
          SET_SIG(100 + i, (cpu->sp >> (8 + i)) & 1);
     /* Data bus */
     uint8_t dbus = gb->debug.cpu_viz.data_bus;
     for (int i = 0; i < 8; i++)
          SET_SIG(108 + i, (dbus >> i) & 1);

#undef SET_SIG
}

void sm83_overlay_tick(Sm83SignalOverlay *ov, float dt)
{
     float decay = dt * 4.0f;
     for (int i = 0; i < ov->count; i++)
     {
          if (ov->signals[i].fade > decay)
               ov->signals[i].fade -= decay;
          else
               ov->signals[i].fade = 0.0f;
     }
}

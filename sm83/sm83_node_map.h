#ifndef SM83_NODE_MAP_H
#define SM83_NODE_MAP_H

/* sm83_node_map.h
 * Maps emulator-visible signals to instance names from the SM83 die.
 * Each entry gives the instance name used in sm83_instances[] and the
 * bit index within that instance's bus (0 for single-bit signals).
 *
 * Source: dmg-schematics/sm83_cells/sm83.jelib, top-level cell 'sm83',
 * instance names from 'I' lines.
 */

typedef struct
{
     const char *signal_name; /* human-readable label */
     const char *inst_prefix; /* prefix of instance name in sm83_instances[] */
     int bit;                 /* bus bit index; -1 = match any instance with prefix */
} Sm83NodeMapEntry;

/* Entries are ordered by functional group for overlay rendering */
static const Sm83NodeMapEntry sm83_node_map[] = {
    /* ── Program Counter ── */
    {"PCL[0]", "reg_pcl[0]", 0},
    {"PCL[1]", "reg_pcl[1]", 0},
    {"PCL[2]", "reg_pcl[2]", 0},
    {"PCL[3]", "reg_pcl[3]", 0},
    {"PCL[4]", "reg_pcl[4]", 0},
    {"PCL[5]", "reg_pcl[5]", 0},
    {"PCL[6]", "reg_pcl[6]", 0},
    {"PCL[7]", "reg_pcl[7]", 0},
    {"PCH[0]", "reg_pch[0]", 0},
    {"PCH[1]", "reg_pch[1]", 0},
    {"PCH[2]", "reg_pch[2]", 0},
    {"PCH[3]", "reg_pch[3]", 0},
    {"PCH[4]", "reg_pch[4]", 0},
    {"PCH[5]", "reg_pch[5]", 0},
    {"PCH[6]", "reg_pch[6]", 0},
    {"PCH[7]", "reg_pch[7]", 0},

    /* ── Accumulator A ── */
    {"A[0]", "reg_a[0]", 0},
    {"A[1]", "reg_a[1]", 0},
    {"A[2]", "reg_a[2]", 0},
    {"A[3]", "reg_a[3]", 0},
    {"A[4]", "reg_a[4]", 0},
    {"A[5]", "reg_a[5]", 0},
    {"A[6]", "reg_a[6]", 0},
    {"A[7]", "reg_a[7]", 0},

    /* ── Register B ── */
    {"B[0]", "reg_b[0]", 0},
    {"B[1]", "reg_b[1]", 0},
    {"B[2]", "reg_b[2]", 0},
    {"B[3]", "reg_b[3]", 0},
    {"B[4]", "reg_b[4]", 0},
    {"B[5]", "reg_b[5]", 0},
    {"B[6]", "reg_b[6]", 0},
    {"B[7]", "reg_b[7]", 0},

    /* ── Instruction Register IR ── */
    {"IR[0]", "reg_ir[0]", 0},
    {"IR[1]", "reg_ir[1]", 0},
    {"IR[2]", "reg_ir[2]", 0},
    {"IR[3]", "reg_ir[3]", 0},
    {"IR[4]", "reg_ir[4]", 0},
    {"IR[5]", "reg_ir[5]", 0},
    {"IR[6]", "reg_ir[6]", 0},
    {"IR[7]", "reg_ir[7]", 0},

    /* ── ALU Flags ── */
    {"F_Z", "flag_z", 0},
    {"F_N", "flag_n", 0},
    {"F_H", "flag_h", 0},
    {"F_C", "flag_c", 0},

    /* ── IDU (Increment/Decrement Unit) ── */
    {"IDU[0]", "idu[0]", 0},
    {"IDU[1]", "idu[1]", 0},
    {"IDU[2]", "idu[2]", 0},
    {"IDU[3]", "idu[3]", 0},
    {"IDU[4]", "idu[4]", 0},
    {"IDU[5]", "idu[5]", 0},
    {"IDU[6]", "idu[6]", 0},
    {"IDU[7]", "idu[7]", 0},

    /* ── Register C ── */
    {"C[0]", "reg_c[0]", 0},
    {"C[1]", "reg_c[1]", 0},
    {"C[2]", "reg_c[2]", 0},
    {"C[3]", "reg_c[3]", 0},
    {"C[4]", "reg_c[4]", 0},
    {"C[5]", "reg_c[5]", 0},
    {"C[6]", "reg_c[6]", 0},
    {"C[7]", "reg_c[7]", 0},

    /* ── Register D ── */
    {"D[0]", "reg_d[0]", 0},
    {"D[1]", "reg_d[1]", 0},
    {"D[2]", "reg_d[2]", 0},
    {"D[3]", "reg_d[3]", 0},
    {"D[4]", "reg_d[4]", 0},
    {"D[5]", "reg_d[5]", 0},
    {"D[6]", "reg_d[6]", 0},
    {"D[7]", "reg_d[7]", 0},

    /* ── Register E ── */
    {"E[0]", "reg_e[0]", 0},
    {"E[1]", "reg_e[1]", 0},
    {"E[2]", "reg_e[2]", 0},
    {"E[3]", "reg_e[3]", 0},
    {"E[4]", "reg_e[4]", 0},
    {"E[5]", "reg_e[5]", 0},
    {"E[6]", "reg_e[6]", 0},
    {"E[7]", "reg_e[7]", 0},

    /* ── Register H ── */
    {"H[0]", "reg_h[0]", 0},
    {"H[1]", "reg_h[1]", 0},
    {"H[2]", "reg_h[2]", 0},
    {"H[3]", "reg_h[3]", 0},
    {"H[4]", "reg_h[4]", 0},
    {"H[5]", "reg_h[5]", 0},
    {"H[6]", "reg_h[6]", 0},
    {"H[7]", "reg_h[7]", 0},

    /* ── Register L ── */
    {"L[0]", "reg_l[0]", 0},
    {"L[1]", "reg_l[1]", 0},
    {"L[2]", "reg_l[2]", 0},
    {"L[3]", "reg_l[3]", 0},
    {"L[4]", "reg_l[4]", 0},
    {"L[5]", "reg_l[5]", 0},
    {"L[6]", "reg_l[6]", 0},
    {"L[7]", "reg_l[7]", 0},

    /* ── Stack Pointer SP ── */
    {"SPL[0]", "reg_spl[0]", 0},
    {"SPL[1]", "reg_spl[1]", 0},
    {"SPL[2]", "reg_spl[2]", 0},
    {"SPL[3]", "reg_spl[3]", 0},
    {"SPL[4]", "reg_spl[4]", 0},
    {"SPL[5]", "reg_spl[5]", 0},
    {"SPL[6]", "reg_spl[6]", 0},
    {"SPL[7]", "reg_spl[7]", 0},
    {"SPH[0]", "reg_sph[0]", 0},
    {"SPH[1]", "reg_sph[1]", 0},
    {"SPH[2]", "reg_sph[2]", 0},
    {"SPH[3]", "reg_sph[3]", 0},
    {"SPH[4]", "reg_sph[4]", 0},
    {"SPH[5]", "reg_sph[5]", 0},
    {"SPH[6]", "reg_sph[6]", 0},
    {"SPH[7]", "reg_sph[7]", 0},

    /* ── Data Bus ── */
    {"DBUS[0]", "dbus_bridge[0]", 0},
    {"DBUS[1]", "dbus_bridge[1]", 0},
    {"DBUS[2]", "dbus_bridge[2]", 0},
    {"DBUS[3]", "dbus_bridge[3]", 0},
    {"DBUS[4]", "dbus_bridge[4]", 0},
    {"DBUS[5]", "dbus_bridge[5]", 0},
    {"DBUS[6]", "dbus_bridge[6]", 0},
    {"DBUS[7]", "dbus_bridge[7]", 0},
};

#define SM83_NODE_MAP_COUNT (int)(sizeof(sm83_node_map) / sizeof(sm83_node_map[0]))

/* Signal group indices for overlay coloring */
#define SM83_SIG_GROUP_PC 0
#define SM83_SIG_GROUP_A 1
#define SM83_SIG_GROUP_B 2
#define SM83_SIG_GROUP_IR 3
#define SM83_SIG_GROUP_FLAGS 4
#define SM83_SIG_GROUP_IDU 5
#define SM83_SIG_GROUP_C 6
#define SM83_SIG_GROUP_D 7
#define SM83_SIG_GROUP_E 8
#define SM83_SIG_GROUP_H 9
#define SM83_SIG_GROUP_L 10
#define SM83_SIG_GROUP_SP 11
#define SM83_SIG_GROUP_DBUS 12

#endif /* SM83_NODE_MAP_H */

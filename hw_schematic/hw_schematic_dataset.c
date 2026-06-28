/* hw_schematic_dataset.c — Dataset instances for each supported board. */
#include "hw_schematic_dataset.h"
#include "lcd_schematic_data.h"
#include "lcd_schematic_map.h"
#include <stddef.h>
#include <stdbool.h>

/* hw_net_map_count / hw_component_map_count are extern const int — cannot be
 * used as static initializers in C.  Store them via accessor instead. */
static int dmg_net_map_n(void)      { return hw_net_map_count; }
static int dmg_comp_map_n(void)     { return hw_component_map_count; }

/* Lookup helpers that operate on an arbitrary dataset's semantic maps. */
const HwNetSemantic *hw_dataset_find_net(const HwSchematicDataset *ds, int net_id)
{
    if (!ds || !ds->net_map) return NULL;
    for (int i = 0; i < ds->net_map_count; i++)
        if (ds->net_map[i].net_id == net_id)
            return &ds->net_map[i];
    return NULL;
}

const HwComponentSemantic *hw_dataset_find_component(const HwSchematicDataset *ds,
                                                      int comp_id)
{
    if (!ds || !ds->component_map) return NULL;
    for (int i = 0; i < ds->component_map_count; i++)
        if (ds->component_map[i].component_id == comp_id)
            return &ds->component_map[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * Dataset instances
 * net_map_count / component_map_count are patched at first use via
 * hw_schematic_dataset_dmg_init() below.
 * ------------------------------------------------------------------------- */
HwSchematicDataset hw_dataset_dmg = {
    .name            = "DMG Main Board",
    .source          = "DMG-CPU-06.kicad_sch",
    .components      = hw_components,
    .component_count = HW_COMPONENT_COUNT,
    .wires           = hw_wires,
    .wire_count      = HW_WIRE_COUNT,
    .nets            = hw_nets,
    .net_count       = HW_NET_COUNT,
    .labels          = hw_labels,
    .label_count     = HW_LABEL_COUNT,
    .junctions       = hw_junctions,
    .junction_count  = HW_JUNCTION_COUNT,
    .net_map         = hw_net_map,
    .net_map_count   = 0, /* patched below */
    .component_map   = hw_component_map,
    .component_map_count = 0,
};

HwSchematicDataset hw_dataset_lcd = {
    .name            = "DMG LCD Board",
    .source          = "DMG-LCD-06.kicad_sch",
    .components      = lcd_components,
    .component_count = LCD_COMPONENT_COUNT,
    .wires           = lcd_wires,
    .wire_count      = LCD_WIRE_COUNT,
    .nets            = lcd_nets,
    .net_count       = LCD_NET_COUNT,
    .labels          = lcd_labels,
    .label_count     = LCD_LABEL_COUNT,
    .junctions       = lcd_junctions,
    .junction_count  = LCD_JUNCTION_COUNT,
    .net_map             = lcd_net_map,
    .net_map_count       = 0, /* patched below */
    .component_map       = lcd_component_map,
    .component_map_count = 0,
};

static bool s_datasets_init = false;

static void datasets_init(void)
{
    if (s_datasets_init) return;
    hw_dataset_dmg.net_map_count       = dmg_net_map_n();
    hw_dataset_dmg.component_map_count = dmg_comp_map_n();
    hw_dataset_lcd.net_map_count       = lcd_net_map_count;
    hw_dataset_lcd.component_map_count = lcd_component_map_count;
    s_datasets_init = true;
}

const HwSchematicDataset *hw_schematic_dataset_for_gb(const struct gb *gb)
{
    datasets_init();
    (void)gb;
    return &hw_dataset_dmg;
}

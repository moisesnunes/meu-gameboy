/* hw_schematic_dataset.h — Multi-dataset abstraction for the HW schematic viewer.
 *
 * Each physical board (DMG main, DMG LCD, CGB main, ...) is represented by
 * one HwSchematicDataset that bundles geometry, nets and semantic maps.
 * The viewer and all helpers operate on a const pointer to the active dataset
 * rather than on the global arrays from hw_schematic_data.h directly.
 */
#pragma once
#include "hw_schematic_data.h"
#include "hw_schematic_map.h"

typedef struct
{
    const char *name;    /* display name, e.g. "DMG Main Board" */
    const char *source;  /* KiCad source file name              */

    const HwComponent *components;
    int component_count;

    const HwWire *wires;
    int wire_count;

    const HwNet *nets;
    int net_count;

    const HwLabel *labels;
    int label_count;

    const HwJunction *junctions;
    int junction_count;

    /* Semantic maps (may be NULL for datasets without a map yet) */
    const HwNetSemantic       *net_map;
    int                        net_map_count;
    const HwComponentSemantic *component_map;
    int                        component_map_count;
} HwSchematicDataset;

/* -------------------------------------------------------------------------
 * Static capacity caps for arrays that must be sized at compile time.
 * Must be >= the largest count across all datasets.
 *   DMG: nets=98  wires=411
 *   LCD: nets=59  wires=266
 * ------------------------------------------------------------------------- */
#define HW_DATASET_MAX_NETS       128
#define HW_DATASET_MAX_WIRES      512
#define HW_DATASET_MAX_COMPONENTS  64

/* -------------------------------------------------------------------------
 * Available datasets (non-const: map counts are patched at first use)
 * ------------------------------------------------------------------------- */
extern HwSchematicDataset hw_dataset_dmg;
extern HwSchematicDataset hw_dataset_lcd;

/* Per-dataset semantic lookups (mirror hw_map_find_net / hw_map_find_component
 * but operate on any dataset rather than the global DMG maps). */
const HwNetSemantic       *hw_dataset_find_net(const HwSchematicDataset *ds, int net_id);
const HwComponentSemantic *hw_dataset_find_component(const HwSchematicDataset *ds, int comp_id);

/* Return the appropriate main-board dataset for a running gb instance.
 * Currently always returns hw_dataset_dmg; will switch to CGB when ready. */
struct gb;
const HwSchematicDataset *hw_schematic_dataset_for_gb(const struct gb *gb);

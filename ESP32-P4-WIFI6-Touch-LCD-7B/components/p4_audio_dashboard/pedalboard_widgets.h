#pragma once

#include "lvgl.h"
#include "seedfx_catalog.h"

/* LVGL-task-only controls. Parameter units remain in DSP units (e.g. mix 0..1).
 * A knob reports a stepped value at most every 100 ms and once on release. */
typedef void (*pedal_knob_changed_t)(uint8_t parameter, float value, void *user);
typedef struct {
    lv_obj_t *arc;
    lv_obj_t *value_label;
    lv_obj_t *pointer;
    lv_point_precise_t pointer_points[2];
    SeedFxParameterDescriptor parameter;
    pedal_knob_changed_t changed;
    void *user;
    uint8_t index;
    float value;
    uint32_t last_sent;
    bool dirty;
} pedal_knob_t;

void pedal_knob_create(pedal_knob_t *knob, lv_obj_t *parent, int x, int y,
                       const SeedFxParameterDescriptor *parameter, float value,
                       uint32_t accent, uint8_t index,
                       pedal_knob_changed_t changed, void *user);
void pedal_knob_flush(pedal_knob_t *knob);

/* Storage must outlive the LVGL objects. Two polylines: cable and arrowhead. */
typedef struct {
    lv_point_precise_t path[8];
    lv_point_precise_t arrow[3];
} pedal_cable_t;
void pedal_cable_create(lv_obj_t *parent, pedal_cable_t *storage,
                        const lv_point_precise_t *points, unsigned count,
                        uint32_t color, uint32_t end_color);

#include "pedalboard_widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool logarithmic(const SeedFxParameterDescriptor *p)
{
    return p->minimum > 0 && p->maximum / p->minimum >= 40
           && (!strcmp(p->unit, "Hz") || !strcmp(p->unit, "ms"));
}

static float from_position(const SeedFxParameterDescriptor *p, int position)
{
    const float t = position / 1000.0f;
    float value = logarithmic(p)
        ? p->minimum * powf(p->maximum / p->minimum, t)
        : p->minimum + t * (p->maximum - p->minimum);
    if (p->step > 0) value = p->minimum
        + roundf((value - p->minimum) / p->step) * p->step;
    return fminf(p->maximum, fmaxf(p->minimum, value));
}

static void update_value(pedal_knob_t *k)
{
    char text[32];
    const bool percent = !strcmp(k->parameter.unit, "%");
    const float shown = k->value * (percent ? 100.0f : 1.0f);
    if (!strcmp(k->parameter.unit, "Hz") && shown >= 1000)
        snprintf(text, sizeof(text), "%.2f kHz", (double)shown / 1000.0);
    else
        snprintf(text, sizeof(text), "%.*f %s",
                 percent || k->parameter.step >= 1 ? 0
                     : k->parameter.step >= .1f ? 1 : 2,
                 (double)shown, k->parameter.unit);
    lv_label_set_text(k->value_label, text);
    const float angle = (135.0f + lv_arc_get_value(k->arc) * .27f) * .01745329252f;
    k->pointer_points[0] = (lv_point_precise_t){46 + 12 * cosf(angle), 46 + 12 * sinf(angle)};
    k->pointer_points[1] = (lv_point_precise_t){46 + 25 * cosf(angle), 46 + 25 * sinf(angle)};
    lv_line_set_points(k->pointer, k->pointer_points, 2);
}

void pedal_knob_flush(pedal_knob_t *k)
{
    if (!k->dirty) return;
    k->dirty = false;
    k->last_sent = lv_tick_get();
    if (k->changed) k->changed(k->index, k->value, k->user);
}

static void knob_event(lv_event_t *e)
{
    pedal_knob_t *k = lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        const float value = from_position(&k->parameter, lv_arc_get_value(k->arc));
        if (value != k->value) {
            k->value = value;
            k->dirty = true;
            update_value(k);
            if (lv_tick_elaps(k->last_sent) >= 100) pedal_knob_flush(k);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        pedal_knob_flush(k);
    }
}

void pedal_knob_create(pedal_knob_t *k, lv_obj_t *parent, int x, int y,
                       const SeedFxParameterDescriptor *parameter, float value,
                       uint32_t accent, uint8_t index,
                       pedal_knob_changed_t changed, void *user)
{
    *k = (pedal_knob_t){.parameter = *parameter, .index = index,
        .changed = changed, .user = user,
        .value = fminf(parameter->maximum, fmaxf(parameter->minimum, value))};
    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text(name, parameter->name);
    lv_obj_set_pos(name, x, y);
    lv_obj_set_size(name, 140, 20);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xe7edf5), 0);
    k->arc = lv_arc_create(parent);
    lv_obj_set_pos(k->arc, x + 24, y + 24);
    lv_obj_set_size(k->arc, 92, 92);
    lv_arc_set_range(k->arc, 0, 1000);
    lv_arc_set_rotation(k->arc, 135);
    lv_arc_set_bg_angles(k->arc, 0, 270);
    const float span = parameter->maximum - parameter->minimum;
    float t = span > 0 ? (k->value - parameter->minimum) / span : 0;
    if (logarithmic(parameter)) t = logf(k->value / parameter->minimum)
        / logf(parameter->maximum / parameter->minimum);
    lv_arc_set_value(k->arc, (int)(t * 1000 + .5f));
    lv_obj_set_style_arc_width(k->arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(k->arc, lv_color_hex(0x314255), LV_PART_MAIN);
    lv_obj_set_style_arc_width(k->arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(k->arc, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(k->arc, lv_color_hex(0xe8edf2), LV_PART_KNOB);
    lv_obj_set_style_pad_all(k->arc, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(k->arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(k->arc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *dial = lv_obj_create(k->arc);
    lv_obj_set_pos(dial, 15, 15);
    lv_obj_set_size(dial, 62, 62);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x203346), 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(0x41576b), 0);
    lv_obj_set_style_border_width(dial, 1, 0);
    lv_obj_set_style_shadow_width(dial, 0, 0);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    k->pointer = lv_line_create(k->arc);
    lv_obj_set_style_line_width(k->pointer, 3, 0);
    lv_obj_set_style_line_rounded(k->pointer, true, 0);
    lv_obj_set_style_line_color(k->pointer, lv_color_hex(0xe8edf2), 0);
    lv_obj_clear_flag(k->pointer, LV_OBJ_FLAG_CLICKABLE);
    k->value_label = lv_label_create(parent);
    lv_obj_set_pos(k->value_label, x, y + 124);
    lv_obj_set_size(k->value_label, 140, 20);
    lv_obj_set_style_text_align(k->value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(k->value_label, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(k->value_label, &lv_font_montserrat_14, 0);
    update_value(k);
    lv_obj_add_event_cb(k->arc, knob_event, LV_EVENT_ALL, k);
}

static lv_obj_t *cable_line(lv_obj_t *parent)
{
    lv_obj_t *object=lv_line_create(parent);
    lv_obj_set_style_line_width(object,3,0);
    lv_obj_set_style_line_rounded(object,true,0);
    lv_obj_clear_flag(object,LV_OBJ_FLAG_CLICKABLE);
    return object;
}
void pedal_cable_update(pedal_cable_t *s,const lv_point_precise_t *points,
                        unsigned count,uint32_t rgb,uint32_t end_rgb)
{
    if(!s->body) return;
    lv_obj_invalidate(s->body);lv_obj_invalidate(s->tail);lv_obj_invalidate(s->arrow_line);
    lv_obj_add_flag(s->tail,LV_OBJ_FLAG_HIDDEN);
    if(count<2 || count>8) {
        lv_obj_add_flag(s->body,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->arrow_line,LV_OBJ_FLAG_HIDDEN);return;
    }
    lv_obj_remove_flag(s->body,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s->arrow_line,LV_OBJ_FLAG_HIDDEN);
    memcpy(s->path,points,count*sizeof(*points));
    lv_obj_set_style_line_color(s->body,lv_color_hex(rgb),0);
    lv_obj_set_style_line_color(s->tail,lv_color_hex(end_rgb),0);
    lv_obj_set_style_line_color(s->arrow_line,lv_color_hex(end_rgb),0);
    if(rgb!=end_rgb) {
        const unsigned split=(count-1)/2+1;
        memmove(s->path+split+1,s->path+split,(count-split)*sizeof(*points));
        s->path[split]=(lv_point_precise_t){(points[split-1].x+points[split].x)/2,
                                           (points[split-1].y+points[split].y)/2};
        lv_line_set_points(s->body,s->path,split+1);
        lv_line_set_points(s->tail,s->path+split,count+1-split);
        lv_obj_remove_flag(s->tail,LV_OBJ_FLAG_HIDDEN);
    } else lv_line_set_points(s->body,s->path,count);
    const lv_point_precise_t end=points[count-1],prev=points[count-2];
    const float dx=end.x-prev.x,dy=end.y-prev.y,length=sqrtf(dx*dx+dy*dy);
    if(length<1) {lv_obj_add_flag(s->arrow_line,LV_OBJ_FLAG_HIDDEN);return;}
    const float ux=dx/length,uy=dy/length;
    s->arrow[0]=(lv_point_precise_t){end.x-9*ux+5*uy,end.y-9*uy-5*ux};
    s->arrow[1]=end;
    s->arrow[2]=(lv_point_precise_t){end.x-9*ux-5*uy,end.y-9*uy+5*ux};
    lv_line_set_points(s->arrow_line,s->arrow,3);
}
void pedal_cable_create(lv_obj_t *parent,pedal_cable_t *s,
                        const lv_point_precise_t *points,unsigned count,
                        uint32_t rgb,uint32_t end_rgb)
{
    memset(s,0,sizeof(*s));
    s->body=cable_line(parent);s->tail=cable_line(parent);s->arrow_line=cable_line(parent);
    pedal_cable_update(s,points,count,rgb,end_rgb);
}

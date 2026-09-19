#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct {
    uint32_t started;
    int x1,y1,x2,y2;
    bool active,dragging,menu,cancelled;
} pedal_gesture_t;
enum { PEDAL_GESTURE_NONE, PEDAL_GESTURE_DRAG, PEDAL_GESTURE_MENU };
static inline void pedal_gesture_start(pedal_gesture_t *g,uint32_t now,int x1,int y1,int x2,int y2)
{ *g=(pedal_gesture_t){.started=now,.x1=x1,.y1=y1,.x2=x2,.y2=y2,.active=true}; }
static inline unsigned pedal_gesture_update(pedal_gesture_t *g,uint32_t now,int x,int y)
{
    if(!g->active || g->cancelled || g->menu) return PEDAL_GESTURE_NONE;
    const bool outside=x<g->x1 || x>g->x2 || y<g->y1 || y>g->y2;
    const uint32_t elapsed=now-g->started;
    if(outside && elapsed<1000) { g->cancelled=true; return PEDAL_GESTURE_NONE; }
    if(g->dragging || (outside && elapsed>=1000)) {
        g->dragging=true; return PEDAL_GESTURE_DRAG;
    }
    if(elapsed>=2000) { g->menu=true; return PEDAL_GESTURE_MENU; }
    return PEDAL_GESTURE_NONE;
}

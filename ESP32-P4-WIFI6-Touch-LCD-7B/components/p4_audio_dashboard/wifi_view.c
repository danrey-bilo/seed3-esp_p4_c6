#include "wifi_view.h"
#include "p4_wifi_settings.h"
#include <stdio.h>
#include <string.h>
static lv_obj_t *s_overlay,*s_body,*s_message,*s_list,*s_ip,*s_reconnect;
static lv_obj_t *s_form,*s_ssid,*s_password,*s_keyboard;
static lv_obj_t *s_form_error;
static p4_wifi_status_t s_shown;
static uint32_t s_revision=UINT32_MAX;
static bool s_choose,s_editing,s_updating;
static lv_obj_t *label(lv_obj_t *p,int x,int y,int w,int h,const char *text)
{
    lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_text_font(o,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_color(o,lv_color_hex(0xe6edf5),0);
    lv_label_set_text(o,text);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);return o;
}
static lv_obj_t *panel(lv_obj_t *p,int x,int y,int w,int h)
{
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x102033),0);
    lv_obj_set_style_radius(o,12,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
static lv_obj_t *button(lv_obj_t *p,int x,int y,int w,const char *text,lv_event_cb_t cb,void *data)
{
    lv_obj_t *o=lv_button_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,44);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x29435b),0);lv_obj_set_style_shadow_width(o,0,0);
    lv_obj_t *t=lv_label_create(o);lv_label_set_text(t,text);lv_obj_center(t);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_14,0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);return o;
}
void wifi_view_close(void)
{
    if(!s_overlay) return;
    lv_textarea_set_text(s_password,"");s_editing=false;
    lv_obj_add_flag(s_form,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_HIDDEN);
}
static void close_event(lv_event_t *e) {(void)e;wifi_view_close();}
static void cancel_form(lv_event_t *e)
{
    (void)e;lv_textarea_set_text(s_password,"");s_editing=false;
    lv_obj_add_flag(s_form,LV_OBJ_FLAG_HIDDEN);s_revision=UINT32_MAX;
}
static void connect_event(lv_event_t *e)
{
    (void)e;
    if(!p4_wifi_connect(lv_textarea_get_text(s_ssid),lv_textarea_get_text(s_password))) {
        lv_label_set_text(s_form_error,"SSID: 1-32 bytes; password: 8-63 bytes, or empty for open Wi-Fi. Try again if busy.");
        return;
    }
    cancel_form(NULL);s_choose=false;
    lv_label_set_text(s_message,"Connection requested...");
}
static void focus_event(lv_event_t *e)
{
    lv_keyboard_set_textarea(s_keyboard,lv_event_get_target_obj(e));
    lv_obj_remove_flag(s_keyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}
static void network_event(lv_event_t *e)
{
    const unsigned index=(uintptr_t)lv_event_get_user_data(e);
    if(index>=s_shown.ap_count) return;
    lv_textarea_set_text(s_ssid,s_shown.aps[index].ssid);lv_textarea_set_text(s_password,"");
    lv_label_set_text(s_form_error,"Leave password empty only for an open network.");
    s_editing=true;lv_obj_clear_flag(s_form,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(s_form);
    lv_keyboard_set_textarea(s_keyboard,s_password);
    lv_obj_remove_flag(s_keyboard,LV_OBJ_FLAG_HIDDEN);
}
static void scan_event(lv_event_t *e)
{
    (void)e;s_choose=true;
    if(p4_wifi_scan()) lv_label_set_text(s_message,"Scan requested...");
    else lv_label_set_text(s_message,"Wi-Fi busy; try again shortly");
    s_revision=UINT32_MAX;
}
static void disconnect_event(lv_event_t *e)
{(void)e;if(!p4_wifi_disconnect()) lv_label_set_text(s_message,"Wi-Fi busy; try again shortly");}
static void reconnect_event(lv_event_t *e)
{
    if(s_updating) return;
    if(!p4_wifi_set_reconnect(lv_obj_has_state(lv_event_get_target_obj(e),LV_STATE_CHECKED)))
        s_revision=UINT32_MAX;
}
static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if(!s_overlay || lv_obj_has_flag(s_overlay,LV_OBJ_FLAG_HIDDEN) || s_editing) return;
    p4_wifi_status_t status;p4_wifi_get_status(&status);
    if(s_revision==status.revision) return;
    s_revision=status.revision;s_shown=status;
    lv_label_set_text(s_message,status.message);
    char text[128];snprintf(text,sizeof(text),status.connected?"%s\nIP: %s":"Not connected%s%s",
        status.connected?status.ssid:"",status.connected?status.ip:"");
    lv_label_set_text(s_ip,text);
    s_updating=true;
    if(status.reconnect) lv_obj_add_state(s_reconnect,LV_STATE_CHECKED);
    else lv_obj_remove_state(s_reconnect,LV_STATE_CHECKED);
    s_updating=false;
    lv_obj_clean(s_list);
    if(s_choose || !status.connected) {
        for(unsigned i=0;i<status.ap_count;++i) {
            snprintf(text,sizeof(text),"%s   %d dBm%s",status.aps[i].ssid,status.aps[i].rssi,
                status.aps[i].secured?"  [LOCKED]":"  [OPEN]");
            button(s_list,8,8+i*54,700,text,network_event,(void *)(uintptr_t)i);
        }
    } else label(s_list,20,26,680,70,"Network is ready. Disconnect or choose another network below.");
}
void wifi_view_open(void)
{
    if(!s_overlay) return;
    p4_wifi_get_status(&s_shown);s_choose=!s_shown.connected;s_revision=UINT32_MAX;
    lv_obj_clear_flag(s_overlay,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(s_overlay);
    refresh(NULL);
    if(!s_shown.connected && !s_shown.busy) (void)p4_wifi_scan();
}
static void open_event(lv_event_t *e) {(void)e;wifi_view_open();}
void wifi_view_create(lv_obj_t *page)
{
    button(page,80,538,864,"WI-FI  >",open_event,NULL);
    s_overlay=panel(page,0,0,1024,600);lv_obj_set_style_bg_color(s_overlay,lv_color_hex(0x07111f),0);
    lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_CLICKABLE);
    label(s_overlay,40,22,600,36,"WI-FI / 2.4 GHz");
    button(s_overlay,850,16,134,"CLOSE",close_event,NULL);
    s_body=panel(s_overlay,120,80,784,492);
    s_ip=label(s_body,26,18,730,52,"");s_message=label(s_body,26,76,730,44,"");
    s_list=panel(s_body,26,132,732,218);lv_obj_add_flag(s_list,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list,LV_DIR_VER);
    s_reconnect=lv_checkbox_create(s_body);lv_obj_set_pos(s_reconnect,26,366);
    lv_checkbox_set_text(s_reconnect,"Reconnect automatically");
    lv_obj_set_style_text_color(s_reconnect,lv_color_hex(0xe6edf5),0);
    lv_obj_add_event_cb(s_reconnect,reconnect_event,LV_EVENT_VALUE_CHANGED,NULL);
    button(s_body,26,426,344,"SCAN / CHANGE NETWORK",scan_event,NULL);
    button(s_body,392,426,366,"DISCONNECT",disconnect_event,NULL);
    s_form=panel(s_overlay,0,74,1024,526);lv_obj_add_flag(s_form,LV_OBJ_FLAG_CLICKABLE);
    label(s_form,32,10,110,28,"NETWORK");
    s_ssid=lv_textarea_create(s_form);lv_obj_set_pos(s_ssid,160,0);lv_obj_set_size(s_ssid,650,48);
    lv_textarea_set_one_line(s_ssid,true);lv_textarea_set_max_length(s_ssid,32);
    lv_obj_add_event_cb(s_ssid,focus_event,LV_EVENT_FOCUSED,NULL);
    lv_obj_add_event_cb(s_ssid,focus_event,LV_EVENT_CLICKED,NULL);
    label(s_form,32,74,130,28,"PASSWORD");
    s_password=lv_textarea_create(s_form);lv_obj_set_pos(s_password,160,62);lv_obj_set_size(s_password,650,48);
    lv_textarea_set_one_line(s_password,true);lv_textarea_set_max_length(s_password,63);
    lv_textarea_set_password_mode(s_password,true);
    lv_obj_add_event_cb(s_password,focus_event,LV_EVENT_FOCUSED,NULL);
    lv_obj_add_event_cb(s_password,focus_event,LV_EVENT_CLICKED,NULL);
    lv_obj_t *fields[]={s_ssid,s_password};
    for(unsigned i=0;i<2;++i) {
        lv_obj_set_style_bg_color(fields[i],lv_color_hex(0x20364b),0);
        lv_obj_set_style_text_color(fields[i],lv_color_hex(0xe6edf5),0);
        lv_obj_set_style_border_color(fields[i],lv_color_hex(0x63839f),0);
        lv_obj_set_style_border_color(fields[i],lv_color_hex(0xe6edf5),LV_PART_CURSOR);
        lv_obj_set_style_bg_color(fields[i],lv_color_hex(0xe6edf5),LV_PART_CURSOR);
        lv_obj_set_style_text_font(fields[i],&lv_font_montserrat_16,0);
        lv_obj_set_style_text_color(fields[i],lv_color_hex(0xe6edf5),LV_PART_CURSOR);
    }
    button(s_form,160,130,240,"CONNECT",connect_event,NULL);
    button(s_form,420,130,240,"CANCEL",cancel_form,NULL);
    s_form_error=label(s_form,160,186,700,48,"Leave password empty only for an open network.");
    s_keyboard=lv_keyboard_create(s_form);
    /* LVGL keyboard defaults to BOTTOM_MID; an absolute positive Y with that
     * alignment placed it below the display. Use explicit top-left coords. */
    lv_obj_set_align(s_keyboard,LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_keyboard,0,246);lv_obj_set_size(s_keyboard,1024,280);
    lv_obj_set_style_bg_color(s_keyboard,lv_color_hex(0x07111f),LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_keyboard,lv_color_hex(0x223a50),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard,lv_color_hex(0x29435b),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_keyboard,lv_color_hex(0x3f6b87),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keyboard,lv_color_hex(0xe6edf5),LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard,lv_color_hex(0xe6edf5),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(s_keyboard,0,LV_PART_ITEMS);
    lv_obj_add_event_cb(s_keyboard,connect_event,LV_EVENT_READY,NULL);
    lv_obj_add_event_cb(s_keyboard,cancel_form,LV_EVENT_CANCEL,NULL);
    lv_obj_add_flag(s_form,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(refresh,500,NULL);
}

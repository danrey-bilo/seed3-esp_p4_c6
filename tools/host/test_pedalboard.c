/* Exercise/render the real firmware UI with native LVGL, without hardware. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../../ESP32-P4-WIFI6-Touch-LCD-7B/components/p4_audio_dashboard/audio_dashboard.c"
#include "seedfx_extensions.h"
#include "../../ESP32-P4-WIFI6-Touch-LCD-7B/components/p4_audio_dashboard/wifi_view.c"
static p4_wifi_status_t mock_wifi={.reconnect=true};
static unsigned mock_scans;
static lv_point_t test_touch;
static bool test_pressed;
static void touch_read(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point=test_touch;data->state=test_pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void touch_advance(lv_indev_t *input,unsigned ms)
{for(unsigned i=0;i<ms;i+=10){lv_tick_inc(10);lv_indev_read(input);lv_timer_handler();}}
esp_err_t p4_wifi_settings_init(void) { return ESP_OK; }
void p4_wifi_get_status(p4_wifi_status_t *s) { *s=mock_wifi; }
bool p4_wifi_scan(void) { ++mock_scans; return true; }
bool p4_wifi_connect(const char *ssid,const char *password) { return ssid[0] && (!password[0] || strlen(password)>=8); }
bool p4_wifi_disconnect(void) { mock_wifi.connected=false;++mock_wifi.revision;return true; }
bool p4_wifi_set_reconnect(bool enable) {mock_wifi.reconnect=enable;++mock_wifi.revision;return true;}

static void snapshot(const char *path)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    assert(b && b->header.w == 1024 && b->header.h == 600);
    FILE *f = fopen(path, "wb"); assert(f);
    uint8_t header[54] = {0x42,0x4d};
    uint32_t size = 54 + 1024*600*3, offset = 54, dib = 40, w=1024, h=600;
    memcpy(header+2,&size,4); memcpy(header+10,&offset,4); memcpy(header+14,&dib,4);
    memcpy(header+18,&w,4); memcpy(header+22,&h,4); header[26]=1; header[28]=24;
    fwrite(header,1,54,f);
    for(int y=599;y>=0;--y) fwrite(b->data+y*b->header.stride,1,3072,f);
    fclose(f); lv_draw_buf_destroy(b);
}

static void render_frame(void)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    assert(b); lv_draw_buf_destroy(b);
}

static void add_effect(unsigned catalog_index)
{
    lv_obj_t *trigger = lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(trigger, effect_selector_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)catalog_index);
    open_effect_selector(UINT8_MAX);
    lv_obj_send_event(trigger, LV_EVENT_CLICKED, NULL);
    lv_obj_delete(trigger);
    audio_dashboard_command_t c;
    while(audio_dashboard_take_command(&c)) {}
    render_frame();
}

static void delete_effect(unsigned index)
{
    s_graph_target_index = index;
    lv_obj_t *trigger = lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(trigger, node_context_action_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)AUDIO_DASHBOARD_GRAPH_DELETE);
    lv_obj_send_event(trigger, LV_EVENT_CLICKED, NULL);
    lv_obj_delete(trigger);
    audio_dashboard_command_t c;
    while(audio_dashboard_take_command(&c)) {}
    render_frame();
}

static void drain_commands(void) { audio_dashboard_command_t c; while(audio_dashboard_take_command(&c)) {} }
static int ui_node_index(uint16_t id)
{
    for(unsigned i=0;i<s_pedal_node_count;++i) if(s_pedal_nodes[i].id==id) return i;
    return -1;
}
static void tap_jack(uint16_t id,uint8_t port,bool output)
{
    const int index=ui_node_index(id);
    lv_obj_t *jack=id?s_node_jacks[index][output?1:0][port]:s_pedal_io[output?0:1].channel_dot[port];
    assert(jack);
    lv_obj_send_event(jack,LV_EVENT_SHORT_CLICKED,NULL);
}
static void patch_pair(uint16_t a,uint8_t ap,bool ao,uint16_t b,uint8_t bp,bool bo)
{
    const unsigned edges=s_ui_edge_count;
    tap_jack(a,ap,ao); assert(s_patch.active);
    tap_jack(b,bp,bo); assert(!s_patch.active && s_ui_edge_count==edges+1);
    drain_commands(); render_frame();
}
static void blank_tap(void) { lv_obj_send_event(s_graph_node_layer,LV_EVENT_SHORT_CLICKED,NULL); }

static void check_layout(const SeedFxGraphDefinition *g,const pedal_grid_t *grid,int scroll_y)
{
    pedal_layout_t layout;
    pedal_layout_build_grid(&layout,g,grid,3,3,scroll_y);
    for(unsigned pass=0;pass<3;++pass) {
    if(pass) {
        const pedal_layout_t previous=layout;
        scroll_y=(scroll_y+593)%(PEDAL_GRID_HEIGHT-490);
        pedal_layout_scroll(&layout,g,3,3,scroll_y);
        for(unsigned e=0;e<g->edge_count;++e) if(g->edges[e].source_id && g->edges[e].destination_id)
            assert(!memcmp(&previous.routes[e],&layout.routes[e],sizeof(pedal_route_t)));
    }
    for(unsigned e=0;e<g->edge_count;++e) {
        const pedal_route_t *a=&layout.routes[e]; assert(a->count>=2);
        for(unsigned i=0;i<a->count;++i) {
            assert(a->points[i].x>=0 && a->points[i].x<1024);
            assert(a->points[i].y>=0 && a->points[i].y<layout.canvas_height);
        }
        for(unsigned i=1;i<a->count;++i) {
            const pedal_point_t p=a->points[i-1],q=a->points[i];
            assert(p.x==q.x || p.y==q.y);
            for(unsigned n=0;n<g->node_count;++n) {
                const int x=layout.nodes[n].x,y=layout.nodes[n].y;
                const int lo=p.x==q.x?(p.y<q.y?p.y:q.y):(p.x<q.x?p.x:q.x);
                const int hi=p.x==q.x?(p.y>q.y?p.y:q.y):(p.x>q.x?p.x:q.x);
                const bool hits=p.x==q.x ? p.x>x && p.x<x+layout.nodes[n].width && hi>y && lo<y+PEDAL_CARD_HEIGHT
                    : p.y>y && p.y<y+PEDAL_CARD_HEIGHT && hi>x && lo<x+layout.nodes[n].width;
                assert(!hits);
            }
        }
        if(!g->edges[e].source_id) assert(a->points[0].y-scroll_y==140+g->edges[e].source_port*204);
        if(!g->edges[e].destination_id) assert(a->points[a->count-1].y-scroll_y==140+g->edges[e].destination_port*204);
        for(unsigned f=0;f<e;++f) {
            const pedal_route_t *b=&layout.routes[f];
            for(unsigned i=1;i<a->count;++i) for(unsigned j=1;j<b->count;++j) {
                const pedal_point_t a0=a->points[i-1],a1=a->points[i];
                const pedal_point_t b0=b->points[j-1],b1=b->points[j];
                const bool av=a0.x==a1.x,bv=b0.x==b1.x;
                if(av!=bv) continue; // a crossing is not a shared parallel track
                const int distance=abs(av?a0.x-b0.x:a0.y-b0.y);
                int alo=av?a0.y:a0.x,ahi=av?a1.y:a1.x;
                int blo=av?b0.y:b0.x,bhi=av?b1.y:b1.x;
                if(alo>ahi){int t=alo;alo=ahi;ahi=t;}
                if(blo>bhi){int t=blo;blo=bhi;bhi=t;}
                const int overlap=(ahi<bhi?ahi:bhi)-(alo>blo?alo:blo);
                if(overlap>0 && distance<4) {
                    fprintf(stderr,"parallel collision edge %u/%u segments %u/%u distance=%d overlap=%d\n",e,f,i,j,distance,overlap);
                    assert(false);
                }
            }
        }
    }
    }
}
static void test_layout(void)
{
    uint32_t random=1234567;
    for(unsigned test=0;test<1536;++test) {
        SeedFxGraphDefinition g={0};g.node_count=12;
        for(unsigned n=0;n<12;++n) {
            g.nodes[n].id=test<512?n+1:12-n;
            g.nodes[n].effect_type=test>=512 && n%3==0?SEEDFX_EFFECT_COMPRESSOR:SEEDFX_EFFECT_GAIN;
            if(test>=1024 && (n%2==0 || test>1280)) g.nodes[n].effect_type=SEEDFX_EFFECT_CABINET_SIM;
        }
        bool used_source[13][2]={{0}},used_target[14][2]={{0}};
        for(unsigned attempt=0;attempt<1200;++attempt) {
            random=random*1664525U+1013904223U;unsigned src=(random>>8)%13;
            random=random*1664525U+1013904223U;unsigned dst=(random>>8)%13+1;
            random=random*1664525U+1013904223U;unsigned sp=(random>>8)%2;
            random=random*1664525U+1013904223U;unsigned dp=(random>>8)%2;
            if(src>=dst || used_source[src][sp] || used_target[dst][dp]) continue;
            if(sp>=seedfx_port_count(&g,src,true)
               || dp>=seedfx_port_count(&g,dst==13?0:dst,false)) continue;
            used_source[src][sp]=used_target[dst][dp]=true;
            g.edges[g.edge_count++]=(SeedFxEdgeDefinition){src,dst==13?0:dst,1,sp,dp,0};
        }
        pedal_grid_t grid={0};pedal_grid_sync(&grid,&g);
        for(unsigned n=0;n<g.node_count;++n) {
            random=random*1664525U+1013904223U;
            pedal_grid_move(&grid,g.nodes[n].id,(random>>8)%PEDAL_GRID_CELLS);
        }
        random=random*1664525U+1013904223U;
        check_layout(&g,&grid,(random>>8)%(PEDAL_GRID_HEIGHT-490));
    }
    puts("PASS: 1536 arbitrary single/wide grid layouts at 3 scroll offsets, unchanged internal wires, no parallel overlaps or body crossings, fixed I/O anchors");
}

int main(void)
{
    pedal_gesture_t gesture;
    pedal_gesture_start(&gesture,100,0,0,104,210);
    assert(pedal_gesture_update(&gesture,599,50,180)==PEDAL_GESTURE_NONE);
    assert(pedal_gesture_update(&gesture,600,105,180)==PEDAL_GESTURE_DRAG);
    assert(pedal_gesture_update(&gesture,2200,20,0)==PEDAL_GESTURE_DRAG);
    pedal_gesture_start(&gesture,0,0,0,104,210);
    assert(pedal_gesture_update(&gesture,999,90,180)==PEDAL_GESTURE_NONE);
    assert(pedal_gesture_update(&gesture,1000,2,0)==PEDAL_GESTURE_MENU);
    pedal_gesture_start(&gesture,0,0,0,104,210);
    assert(pedal_gesture_update(&gesture,400,105,0)==PEDAL_GESTURE_NONE && gesture.cancelled);
    assert(pedal_gesture_update(&gesture,3000,0,0)==PEDAL_GESTURE_NONE);
    const uint32_t wrapped_start=UINT32_MAX-200;
    pedal_gesture_start(&gesture,wrapped_start,0,0,104,210);
    assert(pedal_gesture_update(&gesture,wrapped_start+499,50,100)==PEDAL_GESTURE_NONE);
    assert(pedal_gesture_update(&gesture,wrapped_start+500,105,100)==PEDAL_GESTURE_DRAG);
    pedal_gesture_start(&gesture,wrapped_start,0,0,104,210);
    assert(pedal_gesture_update(&gesture,wrapped_start+999,50,100)==PEDAL_GESTURE_NONE);
    assert(pedal_gesture_update(&gesture,wrapped_start+1000,50,100)==PEDAL_GESTURE_MENU);
    assert(pedal_category(999)==PEDAL_CATEGORY_COUNT-1);
    SeedFxGraphDefinition grid_graph={.node_count=2,.nodes={{.id=1},{.id=2}}};
    pedal_grid_t grid={0};pedal_grid_sync(&grid,&grid_graph);
    assert(pedal_grid_move(&grid,1,31) && !grid.cells[0] && grid.cells[31]==1);
    pedal_grid_sync(&grid,&grid_graph);assert(!grid.cells[0] && grid.cells[31]==1);
    assert(pedal_grid_move(&grid,2,31) && grid.cells[1]==1 && grid.cells[31]==2);
    assert(!pedal_grid_move(&grid,1,-1) && !pedal_grid_move(&grid,1,PEDAL_GRID_CELLS));
    grid_graph.node_count=1;pedal_grid_sync(&grid,&grid_graph);
    assert(grid.cells[1]==1 && !grid.cells[31]);
    for(unsigned cell=0;cell<PEDAL_GRID_CELLS;++cell)
        assert(pedal_grid_hit(pedal_grid_x(cell)+56,pedal_grid_y(cell)+99)==(int)cell);
    SeedFxGraphDefinition wide_graph={.node_count=3,.nodes={
        {.id=1,.effect_type=SEEDFX_EFFECT_CABINET_SIM},
        {.id=2,.effect_type=SEEDFX_EFFECT_GAIN},{.id=3,.effect_type=SEEDFX_EFFECT_GAIN}}};
    pedal_grid_t wide={0};pedal_grid_sync(&wide,&wide_graph);
    assert(wide.cells[0]==1 && wide.cells[1]==1 && wide.cells[2]==2 && wide.cells[3]==3);
    assert(!pedal_grid_can_move(&wide,99,-1) && !pedal_grid_can_move(&wide,1,-1));
    assert(!pedal_grid_move(&wide,1,4) && !pedal_grid_move(&wide,1,2));
    assert(pedal_grid_move(&wide,1,5) && wide.cells[5]==1 && wide.cells[6]==1);
    assert(!pedal_grid_move(&wide,2,6)); // wide swap would collide with the third pedal
    assert(pedal_grid_move(&wide,3,9) && pedal_grid_move(&wide,2,6));
    assert(wide.cells[5]==2 && wide.cells[2]==1 && wide.cells[3]==1);
    assert(pedal_grid_used_rows(&wide)==2);
    assert(pedal_grid_canvas_height(&wide,true)==16+3*PEDAL_GRID_PITCH_Y);
    wide_graph.node_count=1;pedal_grid_sync(&wide,&wide_graph);
    assert(wide.cells[2]==1 && wide.cells[3]==1 && !wide.cells[5] && !wide.cells[9]);
    assert(pedal_grid_used_rows(&wide)==1 && pedal_grid_canvas_height(&wide,false)==490);
    assert(pedal_grid_move(&wide,1,PEDAL_GRID_CELLS-2));
    assert(pedal_grid_used_rows(&wide)==PEDAL_GRID_ROWS);
    assert(pedal_grid_canvas_height(&wide,true)==PEDAL_GRID_HEIGHT);
    assert(!pedal_grid_move(&wide,1,PEDAL_GRID_CELLS-1));
    SeedFxGraphDefinition fanout={.node_count=3,.edge_count=3,
        .nodes={{.id=1,.effect_type=SEEDFX_EFFECT_GAIN},{.id=2,.effect_type=SEEDFX_EFFECT_GAIN},
                {.id=3,.effect_type=SEEDFX_EFFECT_GAIN}},
        .edges={{.source_id=0,.destination_id=1,.gain=1},
                {.source_id=1,.destination_id=2,.gain=1},{.source_id=1,.destination_id=3,.gain=1}}};
    pedal_layout_t fanout_layout;pedal_layout_build(&fanout_layout,&fanout,3,3);
    for(unsigned e=0;e<fanout.edge_count;++e) assert(fanout_layout.routes[e].count>=2);
    test_layout();
    lv_init(); lv_display_t *display=lv_display_create(1024,600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    s_graph_commands=xQueueCreateStatic(32,sizeof(audio_dashboard_command_t),s_graph_queue_storage,&s_graph_queue);
    create_dashboard();
    set_active_page(DASHBOARD_PAGE_SETTINGS);
    wifi_view_open();assert(mock_scans==1);
    strcpy(mock_wifi.ssid,"Test network");strcpy(mock_wifi.ip,"192.0.2.1");
    strcpy(mock_wifi.message,"Connected");mock_wifi.connected=true;++mock_wifi.revision;
    wifi_view_close();wifi_view_open();assert(mock_scans==1);
    assert(strstr(lv_label_get_text(s_ip),"192.0.2.1"));
    snapshot("wifi-connected.bmp");
    s_shown.ap_count=1;strcpy(s_shown.aps[0].ssid,"Test network");
    lv_obj_t *network=lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(network,network_event,LV_EVENT_CLICKED,(void *)0);
    lv_obj_send_event(network,LV_EVENT_CLICKED,NULL);lv_obj_delete(network);
    lv_obj_send_event(s_password,LV_EVENT_CLICKED,NULL);lv_obj_update_layout(lv_screen_active());
    lv_area_t keyboard;lv_obj_get_coords(s_keyboard,&keyboard);
    assert(keyboard.x1>=0 && keyboard.y1>=0 && keyboard.x2<1024 && keyboard.y2<600);
    assert(lv_keyboard_get_textarea(s_keyboard)==s_password);
    unsigned q=0;while(strcmp(lv_keyboard_get_button_text(s_keyboard,q),"q")) ++q;
    lv_buttonmatrix_set_selected_button(s_keyboard,q);
    lv_obj_send_event(s_keyboard,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!strcmp(lv_textarea_get_text(s_password),"q"));lv_textarea_set_text(s_password,"");
    snapshot("wifi-password.bmp");
    lv_textarea_set_text(s_password,"not-a-real-password");wifi_view_close();
    assert(!lv_textarea_get_text(s_password)[0]);
    s_status_flags=STATUS_RATE_VALID|STATUS_PEDALBOARD_ENABLED|STATUS_PEDALBOARD_FORCED;
    s_sample_rate_hz=48000; s_seed_cpu_percent=13;
    update_status_text(); set_active_page(DASHBOARD_PAGE_PEDALBOARD);
    snapshot("empty.bmp");
    /* Short click must not open Audio Settings; hold must. */
    lv_obj_t *settings=lv_obj_get_child(s_pedalboard_page,2);
    lv_obj_send_event(settings,LV_EVENT_SHORT_CLICKED,NULL);
    assert(lv_obj_has_flag(s_io_settings_overlay,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(settings,LV_EVENT_LONG_PRESSED,NULL);
    assert(!lv_obj_has_flag(s_io_settings_overlay,LV_OBJ_FLAG_HIDDEN));
    snapshot("audio-settings.bmp"); close_io_settings();
    for(unsigned i=0;i<5;++i) seedfx_extend_catalog(&s_effect_catalog[i]);
    s_effect_catalog[5]=(SeedFxCatalogEntry){.effect_type=SEEDFX_EFFECT_COMPRESSOR,
        .name="Compressor",.category="dynamics",.color_rgb=0x5be37d,.parameter_count=4,
        .parameters={{"Threshold","dB",-60,0,.5,-18},{"Ratio","x",1,20,.5,4},
                     {"Attack","ms",1,100,1,10},{"Release","ms",10,1000,5,120}}};
    s_effect_catalog_count=6;
    /* Exercise the actual add/delete callbacks, including the sixth pedal
     * and rejected thirteenth addition. Every step renders a complete frame. */
    for(unsigned cycle=0;cycle<20;++cycle) {
        for(unsigned i=0;i<12;++i) { add_effect(i%6); assert(s_pedal_node_count==i+1); }
        add_effect(0); assert(s_pedal_node_count==12);
        for(unsigned i=12;i>0;--i) { delete_effect(i/2); assert(s_pedal_node_count==i-1); }
    }
    for(unsigned i=0;i<12;++i) add_effect(i%6);
    rebuild_ui_edges(); render_pedal_nodes(); snapshot("board-12.bmp");
    lv_obj_scroll_to_y(s_graph_workspace,40,LV_ANIM_OFF);
    assert(lv_obj_has_flag(s_graph_node_layer,LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    assert(lv_obj_has_flag(s_node_cards[0],LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_scroll_to_y(s_graph_workspace,0,LV_ANIM_OFF);
    s_pedal_io[0].channel_enabled[1]=false; render_pedal_nodes(); snapshot("channel-off.bmp");
    assert(lv_obj_has_flag(s_pedal_io[0].channel_dot[1], LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_pedal_io[0].channel_dot[0], LV_OBJ_FLAG_HIDDEN));
    for(unsigned mask=0;mask<16;++mask) {
        for(unsigned side=0;side<2;++side)
            for(unsigned ch=0;ch<2;++ch) s_pedal_io[side].channel_enabled[ch]=(mask&(1<<(side*2+ch)))!=0;
        render_pedal_nodes();
        for(unsigned side=0;side<2;++side) {
            assert(lv_obj_has_flag(s_pedal_io[side].card,LV_OBJ_FLAG_HIDDEN)==!(mask&(3<<(side*2))));
            for(unsigned ch=0;ch<2;++ch)
                assert(lv_obj_has_flag(s_pedal_io[side].channel_dot[ch],LV_OBJ_FLAG_HIDDEN)==!(mask&(1<<(side*2+ch))));
        }
        render_frame();
    }
    s_graph_target_index=5; refresh_node_edit();
    lv_obj_clear_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN); snapshot("knobs.bmp");
    assert(s_node_knob_count==4);
    /* Independent commands survive edits of different knobs. */
    lv_arc_set_value(s_node_knobs[1].arc,600);
    lv_obj_send_event(s_node_knobs[1].arc,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_send_event(s_node_knobs[1].arc,LV_EVENT_RELEASED,NULL);
    lv_arc_set_value(s_node_knobs[2].arc,800);
    lv_obj_send_event(s_node_knobs[2].arc,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_send_event(s_node_knobs[2].arc,LV_EVENT_RELEASED,NULL);
    audio_dashboard_command_t c;
    assert(audio_dashboard_take_command(&c) && c.graph_parameter_index==1 && c.graph_parameter_value>10);
    assert(audio_dashboard_take_command(&c) && c.graph_parameter_index==2 && c.graph_parameter_value>30);
    close_graph_overlays(); show_node_info(); snapshot("information.bmp");
    close_graph_overlays();
    /* Repeated editor open/close must not leave stale knob pointers. */
    for(unsigned i=0;i<100;++i) { s_graph_target_index=i%12; refresh_node_edit(); close_graph_overlays(); }
    /* Direct two-tap patching: no popup/list. Start at either endpoint. */
    s_pedal_node_count=0; s_ui_edge_count=0;
    seedfx_add_routing_catalog(s_effect_catalog,&s_effect_catalog_count);
    add_effect(5); add_effect(6); add_effect(1); add_effect(4); add_effect(7);
    patch_pair(0,0,true,1,0,false);
    patch_pair(2,0,false,1,0,true); // reverse direction
    patch_pair(2,0,true,3,0,false);
    patch_pair(2,1,true,4,0,false);
    patch_pair(5,0,false,3,0,true);
    patch_pair(5,1,false,4,0,true);
    patch_pair(0,0,false,5,0,true);
    patch_pair(0,1,false,0,1,true);
    assert(s_ui_edge_count==8);
    audio_dashboard_submit_local_peaks(0x20000000,0x10000000,0x60000000,0x18000000);
    update_pedal_meters(); snapshot("parallel-routing.bmp");
    tap_jack(2,0,false); assert(s_patch.active); snapshot("patch-selected.bmp");
    tap_jack(5,0,true); assert(!s_patch.active && s_ui_edge_count==8); // occupied/cycle
    tap_jack(1,0,true); tap_jack(3,1,false); assert(s_ui_edge_count==8); // occupied source
    tap_jack(3,1,false); tap_jack(3,1,false); assert(!s_patch.active); // cancel
    tap_jack(4,0,false); blank_tap(); assert(s_ui_edge_count==7); // remove selected input
    patch_pair(4,0,false,2,1,true); assert(s_ui_edge_count==8);
    tap_jack(2,1,true); blank_tap(); assert(s_ui_edge_count==7); // remove selected output
    patch_pair(2,1,true,4,0,false);
    tap_jack(3,1,false); blank_tap(); assert(s_ui_edge_count==8); // free port -> cancel only
    for(unsigned e=0;e<s_ui_edge_count;++e) for(unsigned f=0;f<e;++f)
        if(s_ui_edges[e].source_id && s_ui_edges[e].destination_id
           && s_ui_edges[f].source_id && s_ui_edges[f].destination_id)
            assert(pedal_color_for(&s_route_colors,&s_ui_edges[e])!=pedal_color_for(&s_route_colors,&s_ui_edges[f]));
    assert(jack_color((pedal_jack_t){0,0,true})==pedal_physical_color(0));
    assert(jack_color((pedal_jack_t){0,0,false})==pedal_physical_color(0));
    assert(jack_color((pedal_jack_t){0,1,true})==pedal_physical_color(1));
    assert(jack_color((pedal_jack_t){0,1,false})==pedal_physical_color(1));
    const uint32_t retained=pedal_color_for(&s_route_colors,&s_ui_edges[7]);
    delete_effect(2); assert(s_ui_edge_count==6);
    assert(s_pedal_nodes[2].id==4 && s_pedal_nodes[3].id==5);
    assert(pedal_color_for(&s_route_colors,&s_ui_edges[5])==retained);
    add_effect(1); assert(s_pedal_nodes[4].id==3 && s_ui_edge_count==6);
    /* Card bypass switch must not open the parameter editor. */
    lv_obj_t *toggle=lv_obj_get_child(s_node_cards[0],0);
    lv_obj_send_event(toggle,LV_EVENT_SHORT_CLICKED,NULL);
    assert(!s_pedal_nodes[0].enabled && lv_obj_has_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN));
    drain_commands();
    /* Modal closes only on its own backdrop, not a knob/body click. */
    s_graph_target_index=0; refresh_node_edit(); lv_obj_clear_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN);
    lv_obj_send_event(s_node_edit_body,LV_EVENT_SHORT_CLICKED,NULL);
    assert(!lv_obj_has_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(s_node_edit,LV_EVENT_SHORT_CLICKED,NULL);
    assert(lv_obj_has_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN));
    /* Physical meters use separate ADC/DAC telemetry and stop off-page. */
    audio_dashboard_submit_local_peaks(0x20000000,0x10000000,0x60000000,0);
    update_pedal_meters();
    assert(lv_bar_get_value(s_pedal_io[0].level[0])>70);
    assert(lv_bar_get_value(s_pedal_io[1].level[0])>90);
    snapshot("physical-meters.bmp");
    const float level=s_pedal_io[0].displayed[0];
    set_active_page(DASHBOARD_PAGE_SETTINGS);
    audio_dashboard_submit_local_peaks(0x7fffffff,0,0,0);
    assert(s_pending_local_peak[0]==0 && s_pedal_io[0].displayed[0]==level);
    set_active_page(DASHBOARD_PAGE_PEDALBOARD);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(s_pedal_io[0].channel_dot[0])==42);
    assert(lv_obj_get_height(s_node_jacks[0][0][0])==42);
    /* Filled queue cannot locally mutate routes or nodes. */
    drain_commands();
    c=(audio_dashboard_command_t){.edit_graph=true};
    for(unsigned i=0;i<32;++i) assert(xQueueSend(s_graph_commands,&c,0)==pdTRUE);
    tap_jack(0,1,false); blank_tap(); assert(s_ui_edge_count==6);
    const unsigned old_count=s_pedal_node_count;
    delete_effect(0); assert(s_pedal_node_count==old_count);
    drain_commands();
    /* Category navigation must retain the replacement target. */
    open_effect_selector(2);assert(s_effect_category==-1 && s_graph_target_index==2);
    snapshot("categories.bmp");s_effect_category=13;refresh_effect_selector();
    assert(s_graph_target_index==2);snapshot("category-other.bmp");close_graph_overlays();
    /* Visual order never changes DSP indices, stable IDs or cables. */
    SeedFxGraphDefinition before,after;ui_graph(&before);
    assert(pedal_grid_move(&s_pedal_grid,s_pedal_grid.cells[0],1));
    render_pedal_nodes();ui_graph(&after);assert(!memcmp(&before,&after,sizeof(before)));
    s_effect_catalog[s_effect_catalog_count]=(SeedFxCatalogEntry){.effect_type=SEEDFX_EFFECT_CABINET_SIM,
        .name="Cabinet Sim",.category="tone",.color_rgb=0xd4ad72,.parameter_count=3,
        .parameters={{"Body","%",0,1,.01,.5},{"Presence","%",0,1,.01,.5},{"Mix","%",0,1,.01,1}}};
    unsigned cabinet_index=s_effect_catalog_count++;
    add_effect(cabinet_index);unsigned cabinet=s_pedal_node_count-1;
    assert(lv_obj_get_width(s_node_cards[cabinet])==pedal_grid_width(2));
    assert(lv_obj_get_width(s_node_cards[0])==PEDAL_CARD_WIDTH);
    for(unsigned n=0;n<s_pedal_node_count;++n) {
        assert(lv_obj_get_height(s_node_cards[n])==PEDAL_CARD_HEIGHT);
        assert(lv_obj_get_style_radius(s_node_cards[n],0)==PEDAL_CARD_RADIUS);
        assert(lv_color_to_u32(lv_obj_get_style_bg_color(s_node_cards[n],0))
            !=lv_color_to_u32(lv_obj_get_style_bg_color(s_graph_node_layer,0)));
    }
    assert(!s_grid_hint_layer);
    snapshot("cabinet-board.bmp");
    s_graph_target_index=cabinet;refresh_node_edit();lv_obj_clear_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN);
    assert(s_node_knob_count==3);snapshot("cabinet-editor.bmp");
    close_graph_overlays();lv_obj_update_layout(lv_screen_active());
    lv_timer_pause(lv_display_get_refr_timer(display));
    lv_indev_t *touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch,touch_read);lv_indev_set_display(touch,display);
    lv_area_t card;lv_obj_get_coords(s_node_cards[0],&card);
    test_touch=(lv_point_t){(card.x1+card.x2)/2,card.y1+98};
    test_pressed=true;touch_advance(touch,100);
    test_pressed=false;touch_advance(touch,30);
    assert(!lv_obj_has_flag(s_node_edit,LV_OBJ_FLAG_HIDDEN));close_graph_overlays();
    test_pressed=true;touch_advance(touch,400);
    assert(!s_drag_ready && lv_obj_get_style_outline_width(s_node_cards[0],0)==0);
    touch_advance(touch,200);
    assert(s_drag_ready && lv_obj_get_style_outline_width(s_node_cards[0],0)==2);
    lv_area_t ready_card;lv_obj_get_coords(s_node_cards[0],&ready_card);
    assert(ready_card.x1==card.x1 && ready_card.y1==card.y1
        && ready_card.x2==card.x2 && ready_card.y2==card.y2);
    snapshot("gesture-ready.bmp");
    test_touch.x+=20;test_touch.y+=14;touch_advance(touch,200);
    assert(lv_obj_has_flag(s_node_context,LV_OBJ_FLAG_HIDDEN));
    touch_advance(touch,300);
    assert(!lv_obj_has_flag(s_node_context,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_outline_width(s_node_cards[0],0)==0);
    test_pressed=false;touch_advance(touch,30);close_graph_overlays();
    test_pressed=true;touch_advance(touch,600);
    lv_obj_get_coords(s_node_cards[1],&card);
    test_touch=(lv_point_t){(card.x1+card.x2)/2,card.y1+98};touch_advance(touch,100);
    assert(s_node_gesture.dragging && s_drag_preview);
    assert(s_grid_hint_layer && lv_obj_get_child_count(s_grid_hint_layer)==(pedal_grid_used_rows(&s_pedal_grid)+1)*PEDAL_GRID_COLUMNS);
    assert(lv_obj_get_height(s_graph_node_layer)==pedal_grid_canvas_height(&s_pedal_grid,true));
    snapshot("grid-drag.bmp");
    int previous_slot=pedal_grid_find(&s_pedal_grid,s_pedal_nodes[0].id);ui_graph(&before);
    test_pressed=false;touch_advance(touch,30);ui_graph(&after);
    assert(!s_grid_hint_layer && lv_obj_get_height(s_graph_node_layer)==pedal_grid_canvas_height(&s_pedal_grid,false));
    assert(pedal_grid_find(&s_pedal_grid,s_pedal_nodes[0].id)!=previous_slot && !memcmp(&before,&after,sizeof(before)));
    assert(lv_obj_has_flag(s_node_context,LV_OBJ_FLAG_HIDDEN));
    /* Move to an empty cell below/right, not just another existing card. */
    lv_obj_update_layout(s_graph_workspace);lv_obj_get_coords(s_node_cards[0],&card);
    test_touch=(lv_point_t){(card.x1+card.x2)/2,card.y1+88};
    test_pressed=true;touch_advance(touch,600);
    lv_area_t canvas;lv_obj_get_coords(s_graph_node_layer,&canvas);
    test_touch=(lv_point_t){canvas.x1+pedal_grid_x(7)+56,canvas.y1+pedal_grid_y(7)+99};
    touch_advance(touch,80);assert(s_drop_preview && !lv_obj_has_flag(s_drop_preview,LV_OBJ_FLAG_HIDDEN));
    test_pressed=false;touch_advance(touch,30);ui_graph(&after);
    assert(pedal_grid_find(&s_pedal_grid,s_pedal_nodes[0].id)==7 && !memcmp(&before,&after,sizeof(before)));
    snapshot("grid-placement.bmp");
    /* Fixed sockets/meters and reusable cable objects while the field scrolls. */
    lv_area_t rail_before[4],rail_after;lv_obj_t *wire=s_cables[0].body;
    for(unsigned i=0;i<4;++i) lv_obj_get_coords(s_pedal_io[i/2].channel_dot[i%2],&rail_before[i]);
    lv_obj_scroll_to_y(s_graph_workspace,325,LV_ANIM_OFF);lv_obj_update_layout(s_graph_workspace);
    assert(s_cables[0].body==wire);
    for(unsigned i=0;i<4;++i) {
        lv_obj_get_coords(s_pedal_io[i/2].channel_dot[i%2],&rail_after);
        assert(!memcmp(&rail_before[i],&rail_after,sizeof(rail_after)));
    }
    lv_obj_get_coords(s_graph_node_layer,&canvas);
    for(unsigned e=0;e<s_ui_edge_count;++e) {
        const pedal_route_t *route=&s_cable_layout.routes[e];assert(route->count);
        if(!s_ui_edges[e].source_id) assert(route->points[0].y+canvas.y1
            ==(rail_before[s_ui_edges[e].source_port].y1+rail_before[s_ui_edges[e].source_port].y2+1)/2);
        if(!s_ui_edges[e].destination_id) assert(route->points[route->count-1].y+canvas.y1
            ==(rail_before[2+s_ui_edges[e].destination_port].y1+rail_before[2+s_ui_edges[e].destination_port].y2+1)/2);
    }
    audio_dashboard_submit_local_peaks(0x70000000,0x60000000,0x50000000,0x40000000);update_pedal_meters();
    assert(lv_bar_get_value(s_pedal_io[0].level[0])>90);
    snapshot("grid-scrolled.bmp");
    lv_obj_scroll_to_y(s_graph_workspace,0,LV_ANIM_OFF);lv_obj_update_layout(s_graph_workspace);
    /* Edge auto-scroll reaches rows that were not visible at drag start. */
    lv_obj_get_coords(s_node_cards[1],&card);
    test_touch=(lv_point_t){(card.x1+card.x2)/2,card.y1+88};test_pressed=true;touch_advance(touch,600);
    test_touch=(lv_point_t){pedal_grid_x(3)+56,582};touch_advance(touch,900);
    assert(s_node_gesture.dragging && lv_obj_get_scroll_y(s_graph_workspace)>100);
    test_pressed=false;touch_advance(touch,30);
    assert(pedal_grid_find(&s_pedal_grid,s_pedal_nodes[1].id)>=PEDAL_GRID_COLUMNS*2);
    lv_obj_scroll_to_y(s_graph_workspace,0,LV_ANIM_OFF);lv_obj_update_layout(s_graph_workspace);
    /* Dropping onto the header cancels; positions and audio remain intact. */
    lv_obj_get_coords(s_node_cards[2],&card);
    test_touch=(lv_point_t){(card.x1+card.x2)/2,card.y1+88};test_pressed=true;touch_advance(touch,600);
    previous_slot=pedal_grid_find(&s_pedal_grid,s_pedal_nodes[2].id);
    test_touch=(lv_point_t){600,50};touch_advance(touch,80);
    test_pressed=false;touch_advance(touch,30);
    assert(pedal_grid_find(&s_pedal_grid,s_pedal_nodes[2].id)==previous_slot);
    /* Holding an empty cell adds at that cell, not the first list position. */
    int free_cell=-1;for(int cell=0;cell<8;++cell) if(!s_pedal_grid.cells[cell]) {free_cell=cell;break;}
    assert(free_cell>=0);lv_obj_get_coords(s_graph_node_layer,&canvas);
    test_touch=(lv_point_t){canvas.x1+pedal_grid_x(free_cell)+56,canvas.y1+pedal_grid_y(free_cell)+99};
    test_pressed=true;touch_advance(touch,650);
    assert(!lv_obj_has_flag(s_effect_selector,LV_OBJ_FLAG_HIDDEN) && s_add_cell==free_cell);
    test_pressed=false;touch_advance(touch,30);
    lv_obj_t *add_trigger=lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(add_trigger,effect_selector_event,LV_EVENT_CLICKED,(void *)0);
    lv_obj_send_event(add_trigger,LV_EVENT_CLICKED,NULL);lv_obj_delete(add_trigger);drain_commands();
    assert(pedal_grid_find(&s_pedal_grid,s_pedal_nodes[s_pedal_node_count-1].id)==free_cell);
    lv_indev_delete(touch);
    assert(!s_node_gesture.active && !s_drag_timer && !s_drag_preview);
    assert(lv_obj_get_style_outline_width(s_node_cards[0],0)==0);
    puts("PASS: direct patching both directions, occupied cancellation, blank unplug, fixed physical and unique internal colors");
    puts("PASS: card switch, outside dismissal, physical meters and hidden-page gating");
    puts("PASS: 240 add/delete cycles, 16 channel masks, 4 knobs, 100 editor cycles, full-queue rejection");
    puts("PASS: categories/Other, 5 columns, hidden idle grid, used rows + 1 drag row, wide-card spans, 500/1000 ms gestures, swap, edge auto-scroll, fixed live I/O, Wi-Fi password clearing");
    return 0;
}

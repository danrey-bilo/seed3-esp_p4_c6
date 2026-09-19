#include "p4_wifi_settings.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

enum { CMD_SCAN=1,CMD_CONNECT,CMD_DISCONNECT,CMD_RECONNECT };
enum { EVENT_IP=1,EVENT_LOST=2,EVENT_SCAN=4 };
typedef struct { uint8_t kind;bool enabled;char ssid[33],password[65]; } command_t;
static QueueHandle_t s_commands;
static StaticQueue_t s_queue;
static uint8_t s_queue_bytes[4*sizeof(command_t)];
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static p4_wifi_status_t s_status={.reconnect=true,.message="Wi-Fi is off"};
static volatile uint32_t s_events;
static esp_netif_t *s_netif;
static bool s_initialized,s_started,s_desired,s_scanning,s_connecting;
static unsigned s_retry_seconds=2;
static TickType_t s_retry_at;
static TickType_t s_scan_deadline;
static nvs_handle_t s_nvs;
static bool s_nvs_open;
static char s_saved_ssid[33],s_saved_password[65];
static char s_requested_ssid[33],s_requested_password[65];
static void message(const char *text,bool busy)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.message,sizeof(s_status.message),"%s",text);
    s_status.busy=busy;++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}
void p4_wifi_get_status(p4_wifi_status_t *out)
{ if(out){portENTER_CRITICAL(&s_lock);*out=s_status;portEXIT_CRITICAL(&s_lock);} }
static void event_handler(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;(void)data;
    uint32_t bit=0;
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) bit=EVENT_IP;
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) bit=EVENT_LOST;
    else if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE) bit=EVENT_SCAN;
    if(bit) __atomic_fetch_or(&s_events,bit,__ATOMIC_RELEASE);
}
static esp_err_t radio_start(void)
{
    if(!s_initialized) {
        message("Starting ESP32-C6 Wi-Fi...",true);
        esp_err_t err=esp_netif_init();if(err!=ESP_OK) return err;
        err=esp_event_loop_create_default();
        if(err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) return err;
        if(!s_netif) s_netif=esp_netif_create_default_wifi_sta();
        if(!s_netif) return ESP_ERR_NO_MEM;
        wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
        err=esp_wifi_init(&cfg);if(err!=ESP_OK) return err;
        err=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event_handler,NULL);
        if(err!=ESP_OK) return err;
        err=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event_handler,NULL);
        if(err!=ESP_OK) return err;
        err=esp_wifi_set_storage(WIFI_STORAGE_RAM);if(err!=ESP_OK) return err;
        err=esp_wifi_set_mode(WIFI_MODE_STA);if(err!=ESP_OK) return err;
        s_initialized=true;
    }
    if(!s_started) {
        esp_err_t err=esp_wifi_start();if(err!=ESP_OK) return err;
        s_started=true;
    }
    return ESP_OK;
}
static void clear_link(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.connected=false;s_status.ip[0]=0;++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}
static bool reconnect_enabled(void)
{
    portENTER_CRITICAL(&s_lock);bool value=s_status.reconnect;portEXIT_CRITICAL(&s_lock);
    return value;
}
static void retry_later(void)
{
    s_connecting=false;
    s_retry_at=xTaskGetTickCount()+pdMS_TO_TICKS(s_retry_seconds*1000);
    if(s_retry_seconds<30) s_retry_seconds=s_retry_seconds*2>30?30:s_retry_seconds*2;
}
static void connect_now(void)
{
    esp_err_t err=radio_start();
    if(err==ESP_OK) {
        wifi_config_t cfg={0};
        memcpy(cfg.sta.ssid,s_requested_ssid,strlen(s_requested_ssid));
        memcpy(cfg.sta.password,s_requested_password,strlen(s_requested_password));
        cfg.sta.pmf_cfg.capable=true;
        err=esp_wifi_set_config(WIFI_IF_STA,&cfg);
        memset(&cfg,0,sizeof(cfg));
        if(err==ESP_OK) err=esp_wifi_connect();
    }
    if(err!=ESP_OK) {
        char text[96];snprintf(text,sizeof(text),"Wi-Fi: %s. Check C6 firmware.",esp_err_to_name(err));
        message(text,false);retry_later();return;
    }
    s_connecting=true;s_retry_at=xTaskGetTickCount()+pdMS_TO_TICKS(20000);
    message("Connecting...",true);
}
static void save_connection(void)
{
    if(!strcmp(s_saved_ssid,s_requested_ssid) && !strcmp(s_saved_password,s_requested_password)) return;
    if(!s_nvs_open) return;
    esp_err_t err=nvs_set_str(s_nvs,"ssid",s_requested_ssid);
    if(err==ESP_OK) err=nvs_set_str(s_nvs,"pass",s_requested_password);
    if(err==ESP_OK) err=nvs_commit(s_nvs);
    if(err==ESP_OK) {
        memcpy(s_saved_ssid,s_requested_ssid,sizeof(s_saved_ssid));
        memcpy(s_saved_password,s_requested_password,sizeof(s_saved_password));
    } else message("Connected; could not save network",false);
}
static void read_scan(void)
{
    wifi_ap_record_t aps[P4_WIFI_MAX_APS];uint16_t count=P4_WIFI_MAX_APS;
    esp_err_t err=esp_wifi_scan_get_ap_records(&count,aps);
    s_scanning=false;
    if(err!=ESP_OK){message("Scan failed; try again",false);return;}
    portENTER_CRITICAL(&s_lock);s_status.ap_count=0;
    for(unsigned i=0;i<count;++i) {
        if(!aps[i].ssid[0]) continue;
        bool duplicate=false;
        for(unsigned j=0;j<s_status.ap_count;++j)
            if(!strcmp(s_status.aps[j].ssid,(char *)aps[i].ssid)) duplicate=true;
        if(duplicate) continue;
        p4_wifi_ap_t *a=&s_status.aps[s_status.ap_count++];
        memcpy(a->ssid,aps[i].ssid,32);a->ssid[32]=0;
        a->rssi=aps[i].rssi;a->secured=aps[i].authmode!=WIFI_AUTH_OPEN;
    }
    ++s_status.revision;portEXIT_CRITICAL(&s_lock);
    message(count?"Choose a 2.4 GHz network":"No networks found; try again",false);
}
static void worker(void *arg)
{
    (void)arg;
    s_nvs_open=nvs_open("p4_wifi",NVS_READWRITE,&s_nvs)==ESP_OK;
    if(s_nvs_open) {
        size_t n=sizeof(s_saved_ssid);(void)nvs_get_str(s_nvs,"ssid",s_saved_ssid,&n);
        n=sizeof(s_saved_password);(void)nvs_get_str(s_nvs,"pass",s_saved_password,&n);
        uint8_t enabled=1;(void)nvs_get_u8(s_nvs,"reconnect",&enabled);
        portENTER_CRITICAL(&s_lock);s_status.reconnect=enabled!=0;++s_status.revision;portEXIT_CRITICAL(&s_lock);
        if(enabled && s_saved_ssid[0]) {
            memcpy(s_requested_ssid,s_saved_ssid,sizeof(s_requested_ssid));
            memcpy(s_requested_password,s_saved_password,sizeof(s_requested_password));
            s_desired=true;connect_now();
        }
    }
    for(;;) {
        command_t cmd={0};
        if(xQueueReceive(s_commands,&cmd,pdMS_TO_TICKS(250))==pdTRUE) {
            if(cmd.kind==CMD_RECONNECT) {
                portENTER_CRITICAL(&s_lock);s_status.reconnect=cmd.enabled;++s_status.revision;portEXIT_CRITICAL(&s_lock);
                if(s_nvs_open) {
                    if(nvs_set_u8(s_nvs,"reconnect",cmd.enabled)==ESP_OK) (void)nvs_commit(s_nvs);
                }
            } else if(cmd.kind==CMD_DISCONNECT) {
                s_desired=false;s_connecting=false;
                if(s_scanning) (void)esp_wifi_scan_stop();
                s_scanning=false;
                if(s_started) { (void)esp_wifi_stop();s_started=false; }
                clear_link();message("Disconnected",false);
            } else if(cmd.kind==CMD_SCAN) {
                if(s_connecting) message("Connection in progress; scan later",true);
                else if(!s_scanning) {
                    esp_err_t err=radio_start();
                    if(err==ESP_OK) {wifi_scan_config_t scan={0};err=esp_wifi_scan_start(&scan,false);}
                    s_scanning=err==ESP_OK;
                    s_scan_deadline=xTaskGetTickCount()+pdMS_TO_TICKS(12000);
                    message(s_scanning?"Scanning 2.4 GHz networks...":"Scan unavailable; check C6 firmware",s_scanning);
                }
            } else if(cmd.kind==CMD_CONNECT) {
                s_desired=false;
                if(s_scanning) (void)esp_wifi_scan_stop();
                s_scanning=false;
                if(s_started) (void)esp_wifi_disconnect();
                clear_link();
                memcpy(s_requested_ssid,cmd.ssid,sizeof(s_requested_ssid));
                memcpy(s_requested_password,cmd.password,sizeof(s_requested_password));
                portENTER_CRITICAL(&s_lock);memcpy(s_status.ssid,cmd.ssid,sizeof(s_status.ssid));portEXIT_CRITICAL(&s_lock);
                s_desired=true;s_retry_seconds=2;connect_now();
            }
            memset(&cmd,0,sizeof(cmd));
        }
        const uint32_t events=__atomic_exchange_n(&s_events,0,__ATOMIC_ACQ_REL);
        if(events&EVENT_LOST) {
            clear_link();retry_later();
            message(s_desired && reconnect_enabled()?"Connection lost; retrying...":"Disconnected",false);
        }
        if(events&EVENT_IP) {
            esp_netif_ip_info_t ip;
            wifi_ap_record_t ap;
            /* Ignore a delayed DHCP event from the previous network. */
            if(s_desired && esp_wifi_sta_get_ap_info(&ap)==ESP_OK
                && !strncmp((const char *)ap.ssid,s_requested_ssid,32)
                && esp_netif_get_ip_info(s_netif,&ip)==ESP_OK && ip.ip.addr) {
                portENTER_CRITICAL(&s_lock);
                s_status.connected=true;memcpy(s_status.ssid,s_requested_ssid,sizeof(s_status.ssid));
                snprintf(s_status.ip,sizeof(s_status.ip),IPSTR,IP2STR(&ip.ip));++s_status.revision;
                portEXIT_CRITICAL(&s_lock);
                s_connecting=false;s_retry_seconds=2;message("Connected",false);save_connection();
            }
        }
        if((events&EVENT_SCAN) && s_scanning) read_scan();
        if(s_scanning && (int32_t)(xTaskGetTickCount()-s_scan_deadline)>=0) {
            (void)esp_wifi_scan_stop();s_scanning=false;message("Scan timed out; try again",false);
        }
        if(s_desired && !s_scanning && (int32_t)(xTaskGetTickCount()-s_retry_at)>=0) {
            portENTER_CRITICAL(&s_lock);bool connected=s_status.connected;portEXIT_CRITICAL(&s_lock);
            if(!connected) {
                if(s_connecting) {
                    (void)esp_wifi_disconnect();retry_later();message("Connection timed out; check password",false);
                } else if(reconnect_enabled()) connect_now();
            }
        }
    }
}
esp_err_t p4_wifi_settings_init(void)
{
    if(s_commands) return ESP_OK;
    s_commands=xQueueCreateStatic(4,sizeof(command_t),s_queue_bytes,&s_queue);
    if(xTaskCreatePinnedToCore(worker,"wifi_settings",6144,NULL,2,NULL,1)!=pdPASS) {
        s_commands=NULL;return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static bool submit(command_t *c)
{
    bool ok=s_commands && xQueueSend(s_commands,c,0)==pdTRUE;
    memset(c,0,sizeof(*c));return ok;
}
bool p4_wifi_scan(void) { command_t c={.kind=CMD_SCAN};return submit(&c); }
bool p4_wifi_disconnect(void) { command_t c={.kind=CMD_DISCONNECT};return submit(&c); }
bool p4_wifi_set_reconnect(bool enabled)
{ command_t c={.kind=CMD_RECONNECT,.enabled=enabled};return submit(&c); }
bool p4_wifi_connect(const char *ssid,const char *password)
{
    if(!ssid || !password || !ssid[0] || strlen(ssid)>32 || strlen(password)>63
        || (password[0] && strlen(password)<8)) return false;
    command_t c={.kind=CMD_CONNECT};
    strcpy(c.ssid,ssid);strcpy(c.password,password);return submit(&c);
}

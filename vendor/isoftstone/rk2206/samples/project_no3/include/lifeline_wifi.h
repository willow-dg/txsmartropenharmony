#ifndef __LIFELINE_WIFI_H__
#define __LIFELINE_WIFI_H__

int  lifeline_wifi_has_cred(void);
int  lifeline_wifi_load_sta(char *ssid, char *pwd);
void lifeline_wifi_run_ap(void);
int  lifeline_wifi_is_pairing(void);
void lifeline_wifi_clear_and_reboot(void);

#endif

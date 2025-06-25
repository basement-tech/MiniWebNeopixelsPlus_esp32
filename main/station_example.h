#ifndef __STATION_EXAMPLE_H__
#define __STATION_EXAMPLE_H__

esp_err_t wifi_init_sta(char *ssid, char *passwd, bool dhcp_enable);
void set_static_ip_address_data(esp_netif_ip_info_t ip);

#endif
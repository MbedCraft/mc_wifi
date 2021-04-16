#if !defined __MC_WIFI_H__
#define __MC_WIFI_H__

typedef enum {
    mc_wifi_ok = 0,
    mc_wifi_err,
    mc_wifi_credentials_not_found,
    mc_wifi_join_failed,
} mc_wifi_err_t;

void mc_wifi_init(void);

mc_wifi_err_t mc_wifi_join(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int timeout);

mc_wifi_err_t mc_wifi_start_soft_ap(
        const uint8_t * const ssid, size_t ssid_size,
        const uint8_t * const password, size_t password_size,
        int channel);

#endif // __MC_WIFI_H__


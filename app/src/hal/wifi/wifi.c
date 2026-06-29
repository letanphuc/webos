#include "hal/wifi/wifi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(webos_wifi, LOG_LEVEL_INF);

#define WIFI_RETRY_MAX 5
#define WIFI_RETRY_DELAY_MS 3000
#define WIFI_STARTUP_DELAY_MS 5000

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(ipv4_ready, 0, 1);
static bool wifi_connect_ok;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void wifi_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event, struct net_if* iface) {
  ARG_UNUSED(iface);

  if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
    const struct wifi_status* status = cb->info;

    if (status != NULL && status->status != 0) {
      LOG_ERR("Wi-Fi connect failed: %d", status->status);
      k_sem_give(&wifi_connected);
      return;
    }

    LOG_INF("Wi-Fi connected to %s", CONFIG_WEBOS_WIFI_SSID);
    wifi_connect_ok = true;
    k_sem_give(&wifi_connected);
  } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
    LOG_WRN("Wi-Fi disconnected from %s", CONFIG_WEBOS_WIFI_SSID);
  }
}

static void ipv4_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event, struct net_if* iface) {
  ARG_UNUSED(cb);

  if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
    char addr[NET_IPV4_ADDR_LEN];

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
      if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP) {
        continue;
      }

      LOG_INF("IPv4 address: %s",
              net_addr_ntop(NET_AF_INET, &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr, addr, sizeof(addr)));
      k_sem_give(&ipv4_ready);
      return;
    }
  }
}

int connect_wifi(void) {
  struct net_if* iface = net_if_get_wifi_sta();
  static struct wifi_connect_req_params sta_config;
  int ret;

  if (strlen(CONFIG_WEBOS_WIFI_SSID) == 0) {
    LOG_WRN("Wi-Fi SSID is not configured; skipping auto-connect");
    return -ENOTSUP;
  }

  if (iface == NULL) {
    iface = net_if_get_default();
  }
  if (iface == NULL) {
    LOG_ERR("No network interface available for Wi-Fi STA");
    return -ENODEV;
  }

  net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
                               NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
  net_mgmt_add_event_callback(&wifi_cb);
  net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
  net_mgmt_add_event_callback(&ipv4_cb);

  LOG_INF("Waiting for Wi-Fi stack startup");
  k_sleep(K_MSEC(WIFI_STARTUP_DELAY_MS));

  sta_config.ssid = (const uint8_t*)CONFIG_WEBOS_WIFI_SSID;
  sta_config.ssid_length = strlen(CONFIG_WEBOS_WIFI_SSID);
  sta_config.psk = (const uint8_t*)CONFIG_WEBOS_WIFI_PSK;
  sta_config.psk_length = strlen(CONFIG_WEBOS_WIFI_PSK);
  sta_config.security = WIFI_SECURITY_TYPE_PSK;
  sta_config.channel = WIFI_CHANNEL_ANY;
  sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

  for (int attempt = 1; attempt <= WIFI_RETRY_MAX; attempt++) {
    while (k_sem_take(&wifi_connected, K_NO_WAIT) == 0) {
    }
    wifi_connect_ok = false;

    LOG_INF("Connecting to Wi-Fi SSID: %s (attempt %d/%d)", CONFIG_WEBOS_WIFI_SSID, attempt, WIFI_RETRY_MAX);
    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &sta_config, sizeof(struct wifi_connect_req_params));
    if (ret != 0) {
      LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
      if (attempt < WIFI_RETRY_MAX) {
        k_sleep(K_MSEC(WIFI_RETRY_DELAY_MS));
      }
      continue;
    }

    ret = k_sem_take(&wifi_connected, K_SECONDS(30));
    if (ret == 0 && wifi_connect_ok) {
      break;
    }
    if (ret == 0) {
      ret = -ECONNREFUSED;
      LOG_WRN("Wi-Fi connect attempt %d/%d: failed", attempt, WIFI_RETRY_MAX);
    } else {
      LOG_WRN("Wi-Fi connect attempt %d/%d: timed out", attempt, WIFI_RETRY_MAX);
    }
    if (attempt < WIFI_RETRY_MAX) {
      k_sleep(K_MSEC(WIFI_RETRY_DELAY_MS));
    }
  }

  if (ret != 0) {
    LOG_ERR("Wi-Fi connection failed after %d attempts", WIFI_RETRY_MAX);
    return ret;
  }

  net_dhcpv4_start(iface);
  ret = k_sem_take(&ipv4_ready, K_SECONDS(30));
  if (ret != 0) {
    LOG_WRN("Timed out waiting for DHCP IPv4 address");
  }

  return 0;
}

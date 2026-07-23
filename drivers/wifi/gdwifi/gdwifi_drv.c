/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32VW55x Wi-Fi network driver on the native Zephyr network stack.
 *
 * Replaces the SDK lwIP: it implements the binary seam that the MAC
 * firmware (libwifi.a) imports -- macsw/import/lwip_import.h -- on top of
 * the Zephyr net_if, plus the wifi_mgmt ops on top of the SDK
 * wifi_management API (compiled from source).
 *
 * Contracts imposed by the prebuilt library (do not change without
 * re-reading lwip_import.h):
 *   - "struct pbuf" has a FIXED 16-byte layout (the MAC writes the fields);
 *   - every TX buffer needs NET_AL_TX_HEADROOM (348 B) free before the
 *     payload, and net_buf_tx_info() moves the payload back into that
 *     headroom, returning a 4-byte aligned pointer;
 *   - net_if_input() delivers RX with a free_fn that returns the buffer
 *     to the hardware and must be called exactly once;
 *   - on TX the net_if handed to macif_tx_start() MUST be the VIF one
 *     (vif_idx_to_net_if): anything else is silently dropped.
 *
 * Validated vendor HAL: SDK V1.0.3g (2026-04-23, commit 945c6e2).
 */

#define DT_DRV_COMPAT gd_gd32vw55x_wifi

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>

#include <string.h>

/* Four names declared by the SDK seam headers collide with the Zephyr net
 * stack API already included above.  Divert only the SDK declarations to
 * the gdal_ alias (the blobs are objcopy-renamed to match, and the SDK
 * sources get the same map force-included -- see gdwifi_rename.h).
 */

#define net_if_up gdal_net_if_up
#define net_if_down gdal_net_if_down
#include "mac_types.h"
#include "macif_api.h"
#undef net_if_up
#undef net_if_down

LOG_MODULE_REGISTER(gdwifi_drv, CONFIG_WIFI_LOG_LEVEL);

/* Headroom required by the MAC before the TX payload (lwip_import.h) */

#define NET_AL_TX_HEADROOM 348

#define PBUF_TYPE_RAM 0x80 /* Payload contiguous in the heap */
#define PBUF_TYPE_REF 0x41 /* Struct only; payload is external */

#define ALIGN4_HI(x) (((uintptr_t)(x) + 3) & ~3ul)

#define GDWIFI_VIF_STA 0

/* SDK externs (wifi_manager sources + prebuilt library) */

int macif_tx_start(void *net_if, struct pbuf *buf,
		   void (*cfm_cb)(uint32_t, bool, void *), void *cfm_arg);
int wifi_management_init(void);
int wifi_management_connect(char *ssid, char *password, uint8_t blocked);
int wifi_management_disconnect(void);
int wifi_management_scan(uint8_t blocked, const char *ssid);
int wifi_management_ap_start(char *ssid, char *passwd, uint32_t channel,
			     int auth_mode, uint32_t hidden);
int wifi_management_ap_stop(void);
int wifi_vif_is_sta_connected(int vif_idx);
void *vif_idx_to_net_if(uint8_t vif_idx);
void *vif_idx_to_wvif(uint8_t vif_idx);
int wifi_wpa_rx_eapol_event(void *wvif, uint16_t type, uint8_t *data,
			    uint32_t len);
int wifi_netlink_scan_results_get(int vif_idx,
				  struct macif_scan_results *results);
int gdwifi_start(void);

/* Driver state */

struct gdwifi_dev_data {
	struct net_if *iface;
	uint8_t mac[6];
	bool iface_carrier;
	enum wifi_iface_mode mode;
	uint8_t channel;
	char ssid[WIFI_SSID_MAX_LEN + 1];
};

static struct gdwifi_dev_data g_gdwifi;

/* TX buffer: the struct pbuf the prebuilt library sees comes first (the
 * pointer handed to it is this struct's), followed by our book-keeping.
 */

struct gdwifi_txbuf {
	struct pbuf pbuf; /* MUST be the first field (prebuilt lib ABI) */
	uint8_t *alloc;   /* Start of the allocation (headroom + data) */
	bool payload_shifted;
};

/*
 * Prebuilt library seam (macsw/import/lwip_import.h)
 */

net_buf_tx_t *net_buf_tx_alloc(uint32_t length)
{
	struct gdwifi_txbuf *tx;
	uint8_t *mem;

	tx = k_calloc(1, sizeof(*tx));
	if (tx == NULL) {
		return NULL;
	}

	/* +4 so the headroom pointer can be 4-byte aligned in-place */

	mem = k_malloc(NET_AL_TX_HEADROOM + length + 4);
	if (mem == NULL) {
		k_free(tx);
		return NULL;
	}

	tx->alloc = mem;
	tx->pbuf.payload = mem + NET_AL_TX_HEADROOM;
	tx->pbuf.len = length;
	tx->pbuf.tot_len = length;
	tx->pbuf.ref = 1;
	tx->pbuf.type_internal = PBUF_TYPE_RAM;

	return &tx->pbuf;
}

net_buf_tx_t *net_buf_tx_alloc_ref(uint32_t length)
{
	struct gdwifi_txbuf *tx = k_calloc(1, sizeof(*tx));

	if (tx == NULL) {
		return NULL;
	}

	tx->pbuf.len = length;
	tx->pbuf.tot_len = length;
	tx->pbuf.ref = 1;
	tx->pbuf.type_internal = PBUF_TYPE_REF;

	return &tx->pbuf;
}

static void gdwifi_txbuf_release(struct pbuf *p)
{
	while (p != NULL) {
		struct gdwifi_txbuf *tx = (struct gdwifi_txbuf *)p;
		struct pbuf *next = p->next;

		if (--p->ref == 0) {
			if (tx->alloc != NULL) {
				k_free(tx->alloc);
			}

			k_free(tx);
		}

		p = next;
	}
}

void net_buf_tx_pbuf_free(net_buf_tx_t *buf)
{
	gdwifi_txbuf_release(buf);
}

void net_buf_tx_free(net_buf_tx_t *buf)
{
	struct gdwifi_txbuf *tx = (struct gdwifi_txbuf *)buf;

	if (buf != NULL && tx->payload_shifted) {
		buf->payload = (uint8_t *)buf->payload + NET_AL_TX_HEADROOM;
		buf->len -= NET_AL_TX_HEADROOM;
		buf->tot_len -= NET_AL_TX_HEADROOM;
		tx->payload_shifted = false;
	}

	gdwifi_txbuf_release(buf);
}

void net_buf_tx_cat(net_buf_tx_t *buf1, net_buf_tx_t *buf2)
{
	struct pbuf *p;

	if (buf1 == NULL || buf2 == NULL) {
		return;
	}

	for (p = buf1; p->next != NULL; p = p->next) {
		p->tot_len += buf2->tot_len;
	}

	p->tot_len += buf2->tot_len;
	p->next = buf2;
}

void *net_buf_tx_info(net_buf_tx_t *buf, uint16_t *tot_len, int *seg_cnt,
		      uint32_t seg_addr[], uint16_t seg_len[])
{
	struct gdwifi_txbuf *tx = (struct gdwifi_txbuf *)buf;
	int seg_max = *seg_cnt;
	uint16_t length;
	void *headroom;
	int idx;

	if (buf == NULL) {
		return NULL;
	}

	length = buf->tot_len;
	*tot_len = length;
	seg_addr[0] = (uint32_t)(uintptr_t)buf->payload;
	seg_len[0] = buf->len;
	length -= buf->len;

	/* Grow the pbuf into the headroom */

	if ((uint8_t *)buf->payload - NET_AL_TX_HEADROOM < tx->alloc) {
		LOG_ERR("not enough headroom in the TX buffer");
		return NULL;
	}

	buf->payload = (uint8_t *)buf->payload - NET_AL_TX_HEADROOM;
	buf->len += NET_AL_TX_HEADROOM;
	buf->tot_len += NET_AL_TX_HEADROOM;
	tx->payload_shifted = true;

	headroom = (void *)ALIGN4_HI(buf->payload);

	buf = buf->next;
	idx = 1;

	while (length > 0 && buf != NULL && idx < seg_max) {
		seg_addr[idx] = (uint32_t)(uintptr_t)buf->payload;
		seg_len[idx] = buf->len;
		length -= buf->len;
		buf = buf->next;
		idx++;
	}

	*seg_cnt = idx;

	if (length != 0) {
		LOG_ERR("TX buffer not covered by the segments");
		return NULL;
	}

	return headroom;
}

/* RX: the MAC delivers an Ethernet frame on the MACIF-RX task (not an
 * ISR).  Copy into a net_pkt and return the DMA buffer right away.
 */

int net_if_input(net_buf_rx_t *buf, void *net_if, void *addr, uint16_t len,
		 net_buf_free_fn free_fn)
{
	struct gdwifi_dev_data *data = &g_gdwifi;
	uint8_t *frame = addr;
	uint16_t ethertype;
	struct net_pkt *pkt;

	if (len < 14 || len > NET_ETH_MAX_FRAME_SIZE) {
		free_fn(buf);
		return -1;
	}

	/* EAPOL never goes to the IP stack: the consumer is the SDK
	 * supplicant (4-way handshake).
	 */

	ethertype = (uint16_t)((frame[12] << 8) | frame[13]);

	if (ethertype == 0x888e) {
		wifi_wpa_rx_eapol_event(vif_idx_to_wvif(GDWIFI_VIF_STA),
					ethertype, frame, len);
		free_fn(buf);
		return 0;
	}

	if (data->iface == NULL || !net_if_is_up(data->iface)) {
		free_fn(buf);
		return -1;
	}

	pkt = net_pkt_rx_alloc_with_buffer(data->iface, len, AF_UNSPEC, 0,
					   K_MSEC(50));
	if (pkt == NULL) {
		free_fn(buf);
		return -1;
	}

	if (net_pkt_write(pkt, addr, len) < 0) {
		net_pkt_unref(pkt);
		free_fn(buf);
		return -1;
	}

	free_fn(buf); /* Return the DMA buffer to the MAC */

	if (net_recv_data(data->iface, pkt) < 0) {
		net_pkt_unref(pkt);
		return -1;
	}

	return 0;
}

/* Carrier: called by the SDK/blob when the association goes up/down
 * (renamed seam: net_if_up/net_if_down on the SDK side).
 */

void gdal_net_if_up(void *net_if)
{
	struct gdwifi_dev_data *data = &g_gdwifi;

	data->iface_carrier = true;
	if (data->iface != NULL) {
		net_eth_carrier_on(data->iface);
#if defined(CONFIG_NET_DHCPV4)
		if (data->mode != WIFI_MODE_AP) {
			net_dhcpv4_start(data->iface);
		}
#endif
	}
}

void gdal_net_if_down(void *net_if)
{
	struct gdwifi_dev_data *data = &g_gdwifi;

	data->iface_carrier = false;
	if (data->iface != NULL) {
#if defined(CONFIG_NET_DHCPV4)
		net_dhcpv4_stop(data->iface);
#endif
		net_eth_carrier_off(data->iface);
	}
}

/* Prebuilt library control link: loopback UDP sockets (127.0.0.1).  The
 * blob (macif_cntrl.o) creates them through these functions and moves data
 * with lwip_send/lwip_recv.
 */

int net_lpbk_socket_create(int protocol)
{
	return zsock_socket(AF_INET, SOCK_DGRAM, protocol);
}

int net_lpbk_socket_bind(int sock_recv, uint32_t port)
{
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001UL);
	addr.sin_port = htons(port);

	return zsock_bind(sock_recv, (struct sockaddr *)&addr,
			  sizeof(addr)) < 0 ? -1 : 0;
}

int net_lpbk_socket_connect(int sock_send, uint32_t port)
{
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001UL);
	addr.sin_port = htons(port);

	return zsock_connect(sock_send, (struct sockaddr *)&addr,
			     sizeof(addr)) < 0 ? -1 : 0;
}

int lwip_socket(int domain, int type, int protocol)
{
	return zsock_socket(domain, type, protocol);
}

int lwip_send(int s, const void *data, size_t size, int flags)
{
	return zsock_send(s, data, size, flags);
}

int lwip_recv(int s, void *mem, size_t len, int flags)
{
	return zsock_recv(s, mem, len, flags);
}

int lwip_close(int s)
{
	return zsock_close(s);
}

/* Only meaningful with the SDK DHCP server, which we do not use */

void dhcpd_delete_ipaddr_by_macaddr(uint8_t *mac_addr)
{
}

/*
 * Hooks used by gdwifi_netif_compat.c
 */

struct net_if *gdwifi_iface_get(void)
{
	return g_gdwifi.iface;
}

const uint8_t *gdwifi_mac_get(void)
{
	return g_gdwifi.mac;
}

bool gdwifi_iface_is_up(void)
{
	return g_gdwifi.iface != NULL && net_if_is_up(g_gdwifi.iface);
}

uint32_t gdwifi_iface_ipv4_addr(void)
{
	struct net_if *iface = g_gdwifi.iface;

	if (iface == NULL || iface->config.ip.ipv4 == NULL) {
		return 0;
	}

	return iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr.s_addr;
}

uint8_t *gdwifi_txbuf_payload(struct pbuf *buf)
{
	return ((struct gdwifi_txbuf *)buf)->pbuf.payload;
}

/* Called (through the netif compat net_if_add) when the SDK creates the
 * VIF with the MAC address read from the efuse: the right moment to give
 * the Zephyr iface its link address.
 */

int gdwifi_iface_ready(const uint8_t *mac_addr)
{
	struct gdwifi_dev_data *data = &g_gdwifi;

	memcpy(data->mac, mac_addr, 6);

	if (data->iface != NULL) {
		net_if_set_link_addr(data->iface, data->mac, 6,
				     NET_LINK_ETHERNET);
		net_eth_carrier_off(data->iface);
	}

	LOG_INF("wlan0 ready (%02x:%02x:%02x:%02x:%02x:%02x)",
		mac_addr[0], mac_addr[1], mac_addr[2],
		mac_addr[3], mac_addr[4], mac_addr[5]);
	return 0;
}

/*
 * Zephyr net driver
 */

static void gdwifi_iface_init(struct net_if *iface)
{
	struct gdwifi_dev_data *data = &g_gdwifi;

	data->iface = iface;
	data->mode = WIFI_MODE_INFRA;

	ethernet_init(iface);
	net_eth_set_if_type_wifi(iface);

	/* No carrier until the SDK reports the association */

	net_if_carrier_off(iface);
}

static int gdwifi_send(const struct device *dev, struct net_pkt *pkt)
{
	size_t len = net_pkt_get_len(pkt);
	struct pbuf *p;

	if (len == 0 || len > NET_ETH_MAX_FRAME_SIZE) {
		return -EINVAL;
	}

	/* Only transmit once associated: handing a frame to the MAC while
	 * it is still scanning/associating trips the firmware HW_IDLE
	 * assert.
	 */

	if (!g_gdwifi.iface_carrier) {
		return 0;
	}

	p = net_buf_tx_alloc(len);
	if (p == NULL) {
		return -ENOMEM;
	}

	if (net_pkt_read(pkt, p->payload, len) < 0) {
		net_buf_tx_pbuf_free(p);
		return -EIO;
	}

	/* The net_if MUST be the VIF one: the MAC uses that pointer to know
	 * which interface transmits.  Anything else = silent drop.
	 */

	if (macif_tx_start(vif_idx_to_net_if(GDWIFI_VIF_STA), p,
			   NULL, NULL) != 0) {
		net_buf_tx_pbuf_free(p);
		return -EIO;
	}

	return 0;
}

/*
 * wifi_mgmt ops on top of the SDK wifi_management API
 */

static int gdwifi_mgmt_scan(const struct device *dev, struct net_if *iface,
			    struct wifi_scan_params *params,
			    scan_result_cb_t cb)
{
	struct macif_scan_results *results;
	int ret;

	ret = wifi_management_scan(1 /* blocked */, NULL);
	if (ret != 0) {
		LOG_ERR("scan failed: %d", ret);
		return -EIO;
	}

	results = k_malloc(sizeof(*results));
	if (results == NULL) {
		return -ENOMEM;
	}

	if (wifi_netlink_scan_results_get(GDWIFI_VIF_STA, results) != 0) {
		k_free(results);
		return -EIO;
	}

	for (uint32_t i = 0; i < results->result_cnt; i++) {
		struct mac_scan_result *ap = &results->result[i];
		struct wifi_scan_result entry;

		if (!ap->valid_flag) {
			continue;
		}

		memset(&entry, 0, sizeof(entry));
		entry.ssid_length = MIN(ap->ssid.length, WIFI_SSID_MAX_LEN);
		memcpy(entry.ssid, ap->ssid.array, entry.ssid_length);
		memcpy(entry.mac, ap->bssid.array, 6);
		entry.mac_length = 6;
		entry.rssi = ap->rssi;
		if (ap->chan != NULL) {
			uint16_t freq = ap->chan->freq;

			entry.channel = (freq == 2484) ? 14 :
				(freq > 2407 && freq < 2484) ?
					(freq - 2407) / 5 : 0;
		}

		entry.band = WIFI_FREQ_BAND_2_4_GHZ;
		entry.security = (ap->akm & CO_BIT(MAC_AKM_PRE_RSN)) ?
			((ap->akm & CO_BIT(MAC_AKM_NONE)) ?
				WIFI_SECURITY_TYPE_NONE :
				WIFI_SECURITY_TYPE_WEP) :
			((ap->akm & CO_BIT(MAC_AKM_SAE)) ?
				WIFI_SECURITY_TYPE_SAE :
			 ((ap->akm & CO_BIT(MAC_AKM_PSK)) ?
				WIFI_SECURITY_TYPE_PSK :
				WIFI_SECURITY_TYPE_NONE));

		cb(iface, 0, &entry);
	}

	k_free(results);

	/* End of scan */

	cb(iface, 0, NULL);
	return 0;
}

/* The port is WPA2-only (the SAE handshake faults inside the prebuilt
 * supplicant -- CONFIG_WPA3_SAE is undefined in config/build_config.h), so
 * refuse a network that offers no AKM we can complete -- WPA3(SAE)-only
 * being the common case -- with a clear error instead of letting the
 * association fail obscurely.
 *
 * The AKM bitmap comes from the CACHED scan results only (a previous "wifi
 * scan", or none): issuing a fresh scan here and connecting right after
 * races the LMAC teardown -- the MM task asserts HW_IDLE (mm_task.c:2411,
 * ~1 in 5 connects) because wifi_management_connect starts its own scan
 * while the previous one is still winding down.  If the network is not in
 * the cache the decision is left to the vendor connect path, which is
 * crash-safe: with CONFIG_WPA3_SAE undefined the SAE/OWE AKMs are stripped
 * and an SAE-only AP rejects the WPA2 association cleanly.
 */
static int gdwifi_target_supported(const char *ssid)
{
	struct macif_scan_results *results;
	size_t ssid_len = strlen(ssid);
	uint32_t supported = CO_BIT(MAC_AKM_NONE) | CO_BIT(MAC_AKM_PRE_RSN) |
			     CO_BIT(MAC_AKM_PSK) | CO_BIT(MAC_AKM_PSK_SHA256);
	int ret = 0;

	results = k_malloc(sizeof(*results));
	if (results == NULL) {
		return -ENOMEM;
	}

	if (wifi_netlink_scan_results_get(GDWIFI_VIF_STA, results) != 0) {
		k_free(results);
		return 0;
	}

	for (uint32_t i = 0; i < results->result_cnt; i++) {
		struct mac_scan_result *ap = &results->result[i];

		if (!ap->valid_flag || ap->ssid.length != ssid_len ||
		    memcmp(ap->ssid.array, ssid, ssid_len) != 0) {
			continue;
		}

		if ((ap->akm & supported) == 0) {
			LOG_ERR("'%s' offers no supported AKM (bitmap 0x%x): "
				"WPA3/SAE, OWE and 802.1X are not supported, "
				"WPA2-PSK only", ssid, ap->akm);
			ret = -ENOTSUP;
		}

		break;
	}

	k_free(results);
	return ret;
}

static int gdwifi_mgmt_connect(const struct device *dev, struct net_if *iface,
			       struct wifi_connect_req_params *params)
{
	struct gdwifi_dev_data *data = &g_gdwifi;
	char ssid[WIFI_SSID_MAX_LEN + 1];
	char psk[WIFI_PSK_MAX_LEN + 1];
	int ret;

	if (params->ssid_length == 0 ||
	    params->ssid_length > WIFI_SSID_MAX_LEN) {
		return -EINVAL;
	}

	memcpy(ssid, params->ssid, params->ssid_length);
	ssid[params->ssid_length] = '\0';

	memset(psk, 0, sizeof(psk));
	if (params->psk_length > 0 &&
	    params->psk_length <= WIFI_PSK_MAX_LEN) {
		memcpy(psk, params->psk, params->psk_length);
	}

	if (params->security == WIFI_SECURITY_TYPE_SAE_HNP ||
	    params->security == WIFI_SECURITY_TYPE_SAE_H2E ||
	    params->security == WIFI_SECURITY_TYPE_SAE_AUTO) {
		LOG_ERR("WPA3/SAE is not supported (WPA2-PSK only)");
		return -ENOTSUP;
	}

	ret = gdwifi_target_supported(ssid);
	if (ret < 0) {
		return ret;
	}

	strncpy(data->ssid, ssid, sizeof(data->ssid) - 1);
	data->mode = WIFI_MODE_INFRA;

	LOG_INF("connecting to '%s'", ssid);
	ret = wifi_management_connect(ssid, psk[0] ? psk : NULL,
				      1 /* blocked */);

#if defined(CONFIG_NET_L2_WIFI_MGMT)
	wifi_mgmt_raise_connect_result_event(iface, ret == 0 ? 0 : -1);
#endif
	return ret == 0 ? 0 : -EAGAIN;
}

static int gdwifi_mgmt_disconnect(const struct device *dev,
				  struct net_if *iface)
{
	int ret;

	if (g_gdwifi.mode == WIFI_MODE_AP) {
		ret = wifi_management_ap_stop();
	} else {
		ret = wifi_management_disconnect();
	}

#if defined(CONFIG_NET_L2_WIFI_MGMT)
	wifi_mgmt_raise_disconnect_result_event(iface, ret == 0 ? 0 : -1);
#endif
	return ret == 0 ? 0 : -EIO;
}

/* wifi_management.h: AUTH_MODE_OPEN = 0, ..., AUTH_MODE_WPA2 = 3.
 * WPA3/SAE on the softAP side overruns the crypto: WPA2 like the
 * validated reference port.
 */

#define GDWIFI_AUTH_OPEN 0
#define GDWIFI_AUTH_WPA2 3

static int gdwifi_mgmt_ap_enable(const struct device *dev,
				 struct net_if *iface,
				 struct wifi_connect_req_params *params)
{
	struct gdwifi_dev_data *data = &g_gdwifi;
	char ssid[WIFI_SSID_MAX_LEN + 1];
	char psk[WIFI_PSK_MAX_LEN + 1];
	uint8_t channel;
	int auth;
	int ret;

	if (params->ssid_length == 0 ||
	    params->ssid_length > WIFI_SSID_MAX_LEN) {
		return -EINVAL;
	}

	memcpy(ssid, params->ssid, params->ssid_length);
	ssid[params->ssid_length] = '\0';

	memset(psk, 0, sizeof(psk));
	if (params->psk_length > 0 &&
	    params->psk_length <= WIFI_PSK_MAX_LEN) {
		memcpy(psk, params->psk, params->psk_length);
	}

	channel = (params->channel == WIFI_CHANNEL_ANY) ? 11 : params->channel;
	auth = psk[0] ? GDWIFI_AUTH_WPA2 : GDWIFI_AUTH_OPEN;

	LOG_INF("softAP start '%s' ch=%u", ssid, channel);
	ret = wifi_management_ap_start(ssid, psk[0] ? psk : NULL, channel,
				       auth, 0);
	if (ret == 0) {
		data->mode = WIFI_MODE_AP;
		data->channel = channel;
		strncpy(data->ssid, ssid, sizeof(data->ssid) - 1);
		net_eth_carrier_on(iface);

#if defined(CONFIG_NET_DHCPV4_SERVER)
		/* Serve addresses to the AP clients (the SDK DHCP server is
		 * not compiled).  Same subnet the SDK announces (x.y.z.1).
		 */
		{
			struct in_addr base = { .s_addr = htonl(0xc0a8ed02) };
			struct in_addr gw = { .s_addr = htonl(0xc0a8ed01) };
			struct in_addr mask = { .s_addr = htonl(0xffffff00) };

			net_if_ipv4_addr_add(iface, &gw, NET_ADDR_MANUAL, 0);
			net_if_ipv4_set_netmask_by_addr(iface, &gw, &mask);
			net_dhcpv4_server_start(iface, &base);
		}
#endif
	}

#if defined(CONFIG_NET_L2_WIFI_MGMT)
	wifi_mgmt_raise_ap_enable_result_event(iface, ret == 0 ?
			WIFI_STATUS_AP_SUCCESS : WIFI_STATUS_AP_FAIL);
#endif
	return ret == 0 ? 0 : -EIO;
}

static int gdwifi_mgmt_ap_disable(const struct device *dev,
				  struct net_if *iface)
{
	int ret = wifi_management_ap_stop();

	if (ret == 0) {
		g_gdwifi.mode = WIFI_MODE_INFRA;
		net_eth_carrier_off(iface);
	}

#if defined(CONFIG_NET_L2_WIFI_MGMT)
	wifi_mgmt_raise_ap_disable_result_event(iface, ret == 0 ?
			WIFI_STATUS_AP_SUCCESS : WIFI_STATUS_AP_FAIL);
#endif
	return ret == 0 ? 0 : -EIO;
}

static int gdwifi_mgmt_iface_status(const struct device *dev,
				    struct net_if *iface,
				    struct wifi_iface_status *status)
{
	struct gdwifi_dev_data *data = &g_gdwifi;
	bool connected = data->mode == WIFI_MODE_AP ?
		data->iface_carrier :
		(wifi_vif_is_sta_connected(GDWIFI_VIF_STA) != 0);

	memset(status, 0, sizeof(*status));

	status->state = connected ? WIFI_STATE_COMPLETED :
		WIFI_STATE_DISCONNECTED;
	status->iface_mode = data->mode;
	status->band = WIFI_FREQ_BAND_2_4_GHZ;
	status->channel = data->channel;
	status->ssid_len = strlen(data->ssid);
	memcpy(status->ssid, data->ssid, status->ssid_len);
	memcpy(status->bssid, data->mac, 6);
	status->link_mode = WIFI_6;
	status->security = WIFI_SECURITY_TYPE_PSK;
	status->mfp = WIFI_MFP_OPTIONAL;

	return 0;
}

static int gdwifi_dev_init(const struct device *dev)
{
	return 0;
}

/* Radio bring-up thread: runs once the kernel is up.  wifi_init() spawns
 * the SDK service tasks; the net_if_add callback then publishes the MAC.
 */

static void gdwifi_bringup(void *a, void *b, void *c)
{
	/* wifi_init() (inside gdwifi_start) already runs
	 * wifi_management_init(); calling it again spawns a duplicate
	 * wifi_mgmt task.
	 */
	if (gdwifi_start() != 0) {
		return;
	}

#if defined(CONFIG_GD32VW55X_BLE_VENDOR)
	{
		extern int gdble_vendor_start(void);

		gdble_vendor_start();
	}
#endif

#if defined(CONFIG_WIFI_GDWIFI_TEST_AUTOCONNECT)
	/* Validation helper: exercise the STA path without a console */
	k_sleep(K_SECONDS(3));
	LOG_INF("TEST autoconnect to '%s'", CONFIG_WIFI_GDWIFI_TEST_SSID);
	wifi_management_connect(CONFIG_WIFI_GDWIFI_TEST_SSID,
				CONFIG_WIFI_GDWIFI_TEST_PSK, 1);
#endif
}

K_THREAD_DEFINE(gdwifi_boot, 4096, gdwifi_bringup, NULL, NULL, NULL,
		K_PRIO_PREEMPT(8), 0, 500);

static const struct wifi_mgmt_ops gdwifi_mgmt_ops = {
	.scan = gdwifi_mgmt_scan,
	.connect = gdwifi_mgmt_connect,
	.disconnect = gdwifi_mgmt_disconnect,
	.ap_enable = gdwifi_mgmt_ap_enable,
	.ap_disable = gdwifi_mgmt_ap_disable,
	.iface_status = gdwifi_mgmt_iface_status,
};

static const struct net_wifi_mgmt_offload gdwifi_api = {
	.wifi_iface.iface_api.init = gdwifi_iface_init,
	.wifi_iface.send = gdwifi_send,
	.wifi_mgmt_api = &gdwifi_mgmt_ops,
};

NET_DEVICE_DT_INST_DEFINE(0, gdwifi_dev_init, NULL, &g_gdwifi, NULL,
			  CONFIG_WIFI_INIT_PRIORITY, &gdwifi_api, ETHERNET_L2,
			  NET_L2_GET_CTX_TYPE(ETHERNET_L2), NET_ETH_MTU);

#if defined(CONFIG_NET_CONNECTION_MANAGER_CONNECTIVITY_WIFI_MGMT)
CONNECTIVITY_WIFI_MGMT_BIND(Z_DEVICE_DT_DEV_ID(DT_DRV_INST(0)));
#endif

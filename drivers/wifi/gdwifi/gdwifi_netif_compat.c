/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements the network functions that the SDK wifi_manager expects from
 * the lwIP port (wifi_netif.c), on top of the Zephyr network stack.  Only
 * what the SDK actually calls -- the binary seam with the prebuilt library
 * lives in gdwifi_drv.c.
 *
 * This file deliberately includes NO Zephyr networking header: four seam
 * names collide with the Zephyr net stack API (see gdwifi_rename.h) and
 * the SDK sources call them under the gdal_ alias.  Interface state is
 * reached through the small helpers exported by gdwifi_drv.c.
 *
 * The IP address is not configured by the SDK: Zephyr is in charge (DHCPv4
 * client / net shell).  The DHCP entry points report the iface state only.
 */

#include <zephyr/kernel.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* SDK externals (blob + wrapper) */

struct pbuf;

int macif_tx_start(void *net_if, struct pbuf *buf,
		   void (*cfm_cb)(uint32_t, bool, void *), void *cfm_arg);
int sys_sema_init_ext(void **sema, int max_count, int init_count);
int32_t sys_sema_down(void **sema, uint32_t timeout_ms);
void sys_sema_up(void **sema);
int sys_mutex_init(void **mutex);
int32_t sys_mutex_get(void **mutex);
void sys_mutex_put(void **mutex);

struct pbuf *net_buf_tx_alloc(uint32_t length);
void net_buf_tx_pbuf_free(struct pbuf *buf);

/* From gdwifi_drv.c */

const uint8_t *gdwifi_mac_get(void);
int gdwifi_iface_ready(const uint8_t *mac_addr);
bool gdwifi_iface_is_up(void);
uint32_t gdwifi_iface_ipv4_addr(void);
void *gdwifi_iface_get(void);
uint8_t *gdwifi_txbuf_payload(struct pbuf *buf);

/* EAPOL has to be synchronous: the 4-way handshake installs the keys right
 * after the frame goes out, so wait for the MAC confirm.
 */

static void *g_l2_sema;
static void *g_l2_mutex;
static bool g_l2_acked;

static bool g_static_ip;

static void net_l2_send_cfm(uint32_t frame_id, bool acknowledged, void *arg)
{
	g_l2_acked = acknowledged;
	sys_sema_up(&g_l2_sema);
}

int net_init(void)
{
	sys_sema_init_ext(&g_l2_sema, 1, 0);
	sys_mutex_init(&g_l2_mutex);

	/* IP configuration belongs to Zephyr: report "static IP" so the
	 * wifi_manager connect state machine never waits for its own DHCP
	 * (the Zephyr DHCPv4 client is kicked on carrier-up instead).
	 */

	g_static_ip = true;
	return 0;
}

void net_deinit(void)
{
}

int gdal_net_l2_send(void *net_if, const uint8_t *data, int data_len,
		uint16_t ethertype, const uint8_t *dst_addr, bool *ack)
{
	struct pbuf *buf;
	uint8_t *p;
	int len = data_len;
	int ret;

	if (dst_addr != NULL) {
		len += 14; /* Ethernet header */
	}

	buf = net_buf_tx_alloc(len);
	if (buf == NULL) {
		return -1;
	}

	p = gdwifi_txbuf_payload(buf);

	if (dst_addr != NULL) {
		memcpy(p, dst_addr, 6);
		memcpy(p + 6, gdwifi_mac_get(), 6);
		p[12] = (uint8_t)(ethertype >> 8);
		p[13] = (uint8_t)(ethertype & 0xff);
		p += 14;
	}

	memcpy(p, data, data_len);

	sys_mutex_get(&g_l2_mutex);

	ret = macif_tx_start(net_if, buf, net_l2_send_cfm, NULL);
	if (ret == 0) {
		sys_sema_down(&g_l2_sema, 0); /* Wait for the confirm */

		if (ack != NULL) {
			*ack = g_l2_acked;
		}
	} else {
		net_buf_tx_pbuf_free(buf);
	}

	sys_mutex_put(&g_l2_mutex);
	return ret;
}

int gdal_net_if_get_name(void *net_if, char *name, int len)
{
	if (name == NULL || len <= 0) {
		return -1;
	}

	strncpy(name, "wlan0", len - 1);
	name[len - 1] = '\0';
	return 0;
}

const uint8_t *net_if_get_mac_addr(void *net_if)
{
	return gdwifi_mac_get();
}

void *net_if_find_from_name(const char *name)
{
	return gdwifi_iface_get();
}

int net_if_is_static_ip(void)
{
	return g_static_ip;
}

void net_if_use_static_ip(bool static_ip)
{
	g_static_ip = static_ip;
}

/* IP configuration belongs to Zephyr (DHCPv4 / net shell): keep the entry
 * points so the wifi_manager compiles and report the real iface state.
 */

void net_if_set_ip(void *net_if, uint32_t ip, uint32_t mask, uint32_t gw)
{
}

int net_if_get_ip(void *net_if, uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
	if (ip != NULL) {
		*ip = gdwifi_iface_ipv4_addr();
	}

	if (mask != NULL) {
		*mask = 0;
	}

	if (gw != NULL) {
		*gw = 0;
	}

	return 0;
}

void gdal_net_if_set_default(void *net_if)
{
}

int net_dhcp_start(void *net_if)
{
	return 0;
}

void net_dhcp_stop(void *net_if)
{
}

int net_dhcp_release(void *net_if)
{
	return 0;
}

bool net_dhcp_address_obtained(void *net_if)
{
	return gdwifi_iface_ipv4_addr() != 0;
}

void net_if_send_gratuitous_arp(void *net_if)
{
}

int net_set_dns(uint32_t dns_server)
{
	return 0;
}

int net_get_dns(uint32_t *dns_server)
{
	if (dns_server != NULL) {
		*dns_server = 0;
	}

	return 0;
}

uint16_t net_ip_chksum(const void *dataptr, int len)
{
	const uint8_t *p = dataptr;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}

	if (len > 0) {
		sum += (uint32_t)(p[0] << 8);
	}

	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}

	return (uint16_t)(~sum);
}

int netif_is_up(void *net_if)
{
	return gdwifi_iface_is_up() ? 1 : 0;
}

/* The SDK calls this when it creates the VIF, with the MAC address read
 * from the efuse -- the moment to publish the link address.
 */

int net_if_add(void *net_if, const uint8_t *mac_addr, const uint32_t *ipaddr,
	       const uint32_t *netmask, const uint32_t *gw, void *vif)
{
	static bool registered;

	if (registered) {
		return 0;
	}

	if (gdwifi_iface_ready(mac_addr) < 0) {
		return -1;
	}

	registered = true;
	return 0;
}

void net_if_remove(void *net_if)
{
}

/* SDK DHCP server (softAP): Zephyr's DHCPv4 server is used instead */

int net_dhcpd_start(void *net_if)
{
	return -1;
}

void net_dhcpd_stop(void *net_if)
{
}

uint32_t dhcpd_find_ipaddr_by_macaddr(uint8_t *mac_addr)
{
	return 0;
}

void dhcpd_set_dns_server(uint32_t dns)
{
}

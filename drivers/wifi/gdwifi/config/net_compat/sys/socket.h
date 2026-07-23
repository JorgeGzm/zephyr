/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BSD-socket shim for the SDK sources (wifi_wpa.c control interface over
 * loopback UDP), mapped onto the Zephyr zsock API.  This header shadows
 * <sys/socket.h>: the driver config include dir comes first.
 *
 * The seam renames (gdwifi_rename.h) are lifted around the Zephyr socket
 * header: it drags zephyr/net/net_if.h in, whose net_if_up/net_if_down
 * declarations must keep their real (Zephyr) names in this translation
 * unit -- the SDK calls still go to the gdal_ aliases re-defined below.
 */

#ifndef _GDWIFI_COMPAT_SYS_SOCKET_H_
#define _GDWIFI_COMPAT_SYS_SOCKET_H_

#undef net_if_up
#undef net_if_down
#undef net_if_get_name
#undef net_if_set_default
#undef net_l2_send

#include <zephyr/net/socket.h>

#define net_if_up          gdal_net_if_up
#define net_if_down        gdal_net_if_down
#define net_if_get_name    gdal_net_if_get_name
#define net_if_set_default gdal_net_if_set_default
#define net_l2_send        gdal_net_l2_send

/* Plain BSD names for the SDK sources */

#ifndef socket
#define socket   zsock_socket
#define bind     zsock_bind
#define connect  zsock_connect
#define send     zsock_send
#define recv     zsock_recv
#define sendto   zsock_sendto
#define recvfrom zsock_recvfrom
#define close    zsock_close
#define select   zsock_select
#define fd_set   zsock_fd_set
#define timeval  zsock_timeval
#undef FD_ZERO
#undef FD_SET
#undef FD_ISSET
#undef FD_CLR
#define FD_ZERO  ZSOCK_FD_ZERO
#define FD_SET   ZSOCK_FD_SET
#define FD_ISSET ZSOCK_FD_ISSET
#define FD_CLR   ZSOCK_FD_CLR
#endif

#endif /* _GDWIFI_COMPAT_SYS_SOCKET_H_ */

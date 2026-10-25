/* WS2_32 on host sockets (ws2.c). */
#pragma once

#include <stdint.h>

/* Starts host networking and registers the shims (before the images are mapped). */
void ws2_init(void);
/* A name the host answers itself (before DNS), e.g. the lobby -> the chosen server. */
void ws2_map_host(const char* name, uint32_t ipv4_host_order);

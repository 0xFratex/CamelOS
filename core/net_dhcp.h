// core/net_dhcp.h
#ifndef NET_DHCP_H
#define NET_DHCP_H

int dhcp_discover(void);
int dhcp_request(uint32_t offered_ip);
void dhcp_process_packet(uint8_t* payload, uint32_t len);

#endif
/*
 * xdmcp.c
 *
 * Copyright (C) 2009-11 - ipoque GmbH
 * Copyright (C) 2011-25 - ntop.org
 *
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the OpenDPI and PACE technology by ipoque GmbH
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "ndpi_protocol_ids.h"

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_XDMCP

#include "ndpi_api.h"
#include "ndpi_private.h"


static void ndpi_int_xdmcp_add_connection(struct ndpi_detection_module_struct
					  *ndpi_struct, struct ndpi_flow_struct *flow)
{
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_XDMCP, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
}

static void ndpi_search_xdmcp(struct ndpi_detection_module_struct *ndpi_struct,
			      struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);
	
  NDPI_LOG_DBG(ndpi_struct, "search xdmcp\n");

  if (packet->tcp != NULL && (ntohs(packet->tcp->dest) >= 6000 && ntohs(packet->tcp->dest) <= 6005)
      && packet->payload_packet_len == 48
      && packet->payload[0] == 0x6c && packet->payload[1] == 0x00
      && ntohs(get_u_int16_t(packet->payload, 6)) == 0x1200 && ntohs(get_u_int16_t(packet->payload, 8)) == 0x1000) {

    NDPI_LOG_INFO(ndpi_struct, "found xdmcp over tcp\n");
    ndpi_int_xdmcp_add_connection(ndpi_struct, flow);
    return;
  }
  if (packet->udp != NULL && ntohs(packet->udp->dest) == 177
      && packet->payload_packet_len >= 6 && packet->payload_packet_len == 6 + ntohs(get_u_int16_t(packet->payload, 4))
      && ntohs(get_u_int16_t(packet->payload, 0)) == 0x0001 && ntohs(get_u_int16_t(packet->payload, 2)) == 0x0002) {

    NDPI_LOG_INFO(ndpi_struct, "found xdmcp over udp\n");
    ndpi_int_xdmcp_add_connection(ndpi_struct, flow);
    return;
  }

  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}


void init_xdmcp_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("XDMCP", ndpi_struct,
                     ndpi_search_xdmcp,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_OR_UDP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
                     1, NDPI_PROTOCOL_XDMCP);
}

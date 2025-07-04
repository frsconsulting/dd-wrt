/*
 * noe.c (Alcatel new office environment)
 *
 * Copyright (C) 2013 Remy Mudingay <mudingay@ill.fr>
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_NOE

#include "ndpi_api.h"
#include "ndpi_private.h"


static void ndpi_int_noe_add_connection(struct ndpi_detection_module_struct
					*ndpi_struct, struct ndpi_flow_struct *flow)
{
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_NOE, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
  NDPI_LOG_INFO(ndpi_struct, "found noe\n");
}

static void ndpi_search_noe(struct ndpi_detection_module_struct *ndpi_struct,
			    struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);
  
  NDPI_LOG_DBG(ndpi_struct, "search NOE\n");
  
  if(packet->udp != NULL) {
    NDPI_LOG_DBG2(ndpi_struct, "calculating dport over udp\n");

    if (packet->payload_packet_len == 1 && ( packet->payload[0] == 0x05 || packet->payload[0] == 0x04 )) {
      ndpi_int_noe_add_connection(ndpi_struct, flow);
      return;
    } else if((packet->payload_packet_len == 5 || packet->payload_packet_len == 12) &&
	      (packet->payload[0] == 0x07 ) && 
	      (packet->payload[1] == 0x00 ) &&
	      (packet->payload[2] != 0x00 ) &&
	      (packet->payload[3] == 0x00 )) {
      ndpi_int_noe_add_connection(ndpi_struct, flow);
      return;
    } else if((packet->payload_packet_len >= 25) &&
	      (packet->payload[0] == 0x00 &&
	       packet->payload[1] == 0x06 &&
	       packet->payload[2] == 0x62 &&
	       packet->payload[3] == 0x6c)) {
      ndpi_int_noe_add_connection(ndpi_struct, flow);
      return;
    }
  }
  
  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}


void init_noe_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("NOE", ndpi_struct,
                     ndpi_search_noe,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
                     1, NDPI_PROTOCOL_NOE);
}


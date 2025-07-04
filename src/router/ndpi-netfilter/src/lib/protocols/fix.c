/*
 * fix.c
 *
 * Copyright (C) 2017-22 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_FIX

#include "ndpi_api.h"
#include "ndpi_private.h"


static void ndpi_search_fix(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);

  NDPI_LOG_DBG(ndpi_struct, "search FIX\n");
  if(packet->tcp && packet->payload_packet_len > 5) {
    // 8=
    if(packet->payload[0] == 0x38 && packet->payload[1] == 0x3d) {
      // FIX
      if(packet->payload[2] == 0x46 &&
	 packet->payload[3] == 0x49 &&
	 packet->payload[4] == 0x58) {
	
	NDPI_LOG_INFO(ndpi_struct, "found FIX\n");
	ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_FIX, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
	return;
      }
      // 0. 9=
      if(packet->payload[2] == 0x4f &&
	 packet->payload[3] == 0x01 &&
	 packet->payload[4] == 0x39 &&
	 packet->payload[5] == 0x3d) {

	NDPI_LOG_INFO(ndpi_struct, "found FIX\n");
	ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_FIX, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
	return;
      }
    }
  }

  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}


void init_fix_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("FIX", ndpi_struct,
                     ndpi_search_fix,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
                     1, NDPI_PROTOCOL_FIX);
}

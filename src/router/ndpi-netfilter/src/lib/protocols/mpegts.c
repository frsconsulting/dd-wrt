/*
 * mpegts.c (MPEG Transport Stream)
 *          https://en.wikipedia.org/wiki/MPEG_transport_stream
 *
 * Copyright (C) 2015-22 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_MPEGTS

#include "ndpi_api.h"
#include "ndpi_private.h"

static void ndpi_search_mpegts(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);

  NDPI_LOG_DBG(ndpi_struct, "search MPEGTS\n");

  if((packet->udp != NULL) && ((packet->payload_packet_len % 188) == 0)) {
    u_int i, num_chunks = packet->payload_packet_len / 188;
    
    for(i=0; i<num_chunks; i++) {
      u_int offset = 188 * i;

      if(packet->payload[offset] != 0x47) goto no_mpegts;
    }

    /* This looks MPEG TS */
    NDPI_LOG_INFO(ndpi_struct, "found MPEGTS\n");
    ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_MPEGTS, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
    return;
  }    

 no_mpegts:
  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}


void init_mpegts_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("MPEG_TS", ndpi_struct,
                     ndpi_search_mpegts,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
                      1, NDPI_PROTOCOL_MPEGTS);
}


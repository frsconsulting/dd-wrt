/*
 * vmware.c
 *
 * Copyright (C) 2016-22 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_VMWARE

#include "ndpi_api.h"
#include "ndpi_private.h"

static void ndpi_search_vmware(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);

  NDPI_LOG_DBG(ndpi_struct, "search vmware\n");
  /* Check whether this is an VMWARE flow */
  if(packet->udp != NULL){
    if((packet->payload_packet_len == 66) &&
       (ntohs(packet->udp->dest) == 902) &&
       ((packet->payload[0] & 0xFF) == 0xA4)){
      
      NDPI_LOG_INFO(ndpi_struct, "found vmware\n");
      ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_VMWARE, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
      return;
    }
  }
  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}

void init_vmware_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("VMWARE", ndpi_struct,
                     ndpi_search_vmware,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
                     1, NDPI_PROTOCOL_VMWARE);
}

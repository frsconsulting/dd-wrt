/*
 * citrix.c
 *
 * Copyright (C) 2012-22 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_CITRIX

#include "ndpi_api.h"
#include "ndpi_private.h"


/* ************************************ */

static void ndpi_check_citrix(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);
  u_int32_t payload_len = packet->payload_packet_len;

  if(payload_len == 6) {
    char citrix_header[] = { 0x7F, 0x7F, 0x49, 0x43, 0x41, 0x00 };

    if(memcmp(packet->payload, citrix_header, sizeof(citrix_header)) == 0) {
      NDPI_LOG_INFO(ndpi_struct, "found citrix\n");
      ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_CITRIX, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
      return;
    }
  } else if(payload_len > 22) {
    char citrix_header[] = { 0x1a, 0x43, 0x47, 0x50, 0x2f, 0x30, 0x31 };

    if((memcmp(packet->payload, citrix_header, sizeof(citrix_header)) == 0)
       || (ndpi_strnstr((const char *)packet->payload, "Citrix.TcpProxyService", payload_len) != NULL)) {
      NDPI_LOG_INFO(ndpi_struct, "found citrix\n");
      ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_CITRIX, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
      return;
    }
  }

  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}

static void ndpi_search_citrix(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  NDPI_LOG_DBG(ndpi_struct, "search citrix\n");

  ndpi_check_citrix(ndpi_struct, flow);
}


void init_citrix_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("Citrix", ndpi_struct,
                     ndpi_search_citrix,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
                     1, NDPI_PROTOCOL_CITRIX);
}

/*
 * http2.c
 *
 * Copyright (C) 2023 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_HTTP2

#include "ndpi_api.h"
#include "ndpi_private.h"

static void ndpi_int_http2_add_connection(struct ndpi_detection_module_struct * const ndpi_struct,
                                          struct ndpi_flow_struct * const flow)
{
  NDPI_LOG_INFO(ndpi_struct, "found HTTP/2\n");
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_HTTP2,
                             NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
}

void ndpi_search_http2(struct ndpi_detection_module_struct *ndpi_struct,
                       struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct const * const packet = ndpi_get_packet_struct(ndpi_struct);
  const char magic[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

  NDPI_LOG_DBG(ndpi_struct, "search http2\n");

  if(packet->payload_packet_len < NDPI_STATICSTRING_LEN(magic)) {
    NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
    return;
  }

  if(strncmp((char const *)packet->payload, magic, NDPI_STATICSTRING_LEN(magic)) == 0) {
    ndpi_int_http2_add_connection(ndpi_struct, flow);
    return;
  }

  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}

void init_http2_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("HTTP2", ndpi_struct,
                     ndpi_search_http2,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
                     1, NDPI_PROTOCOL_HTTP2);
}

/*
 * avast_securedns.c
 *
 * Copyright (C) 2012-22 - ntop.org
 *
 * This module is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "ndpi_protocol_ids.h"

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_AVAST_SECUREDNS

#include "ndpi_api.h"
#include "ndpi_private.h"

static void ndpi_int_avast_securedns_add_connection(struct ndpi_detection_module_struct *ndpi_struct,
                                                    struct ndpi_flow_struct *flow)
{
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_AVAST_SECUREDNS, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
}

static void ndpi_search_avast_securedns(struct ndpi_detection_module_struct *ndpi_struct,
                                        struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct * packet = ndpi_get_packet_struct(ndpi_struct);

  if (packet->payload_packet_len < 34 ||
      ntohl(get_u_int32_t(packet->payload, 11)) != 0x00013209 ||
      flow->packet_counter > 1)
  {
    NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
    return;
  }

  if (strncasecmp((char *)&packet->payload[15], "securedns", NDPI_STATICSTRING_LEN("securedns")) == 0)
  {
    ndpi_int_avast_securedns_add_connection(ndpi_struct, flow);
    return;
  }

  NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
}

void init_avast_securedns_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("AVAST SecureDNS", ndpi_struct,
                     ndpi_search_avast_securedns,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
                     1, NDPI_PROTOCOL_AVAST_SECUREDNS);
}

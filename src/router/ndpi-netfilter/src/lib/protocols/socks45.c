/*
 * socks4.c
 *
 * Copyright (C) 2016-22 - ntop.org
 * Copyright (C) 2014 Tomasz Bujlow <tomasz@skatnet.dk>
 *
 * The signature is based on the Libprotoident library.
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_SOCKS

#include "ndpi_api.h"
#include "ndpi_private.h"

static void ndpi_int_socks_add_connection(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_SOCKS, NDPI_PROTOCOL_UNKNOWN, NDPI_CONFIDENCE_DPI);
}

static void ndpi_check_socks4(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);
  u_int32_t payload_len = packet->payload_packet_len;

  /* Check if we so far detected the protocol in the request or not. */
  if(flow->l4.tcp.socks4_stage == 0) {
    NDPI_LOG_DBG2(ndpi_struct, "SOCKS4 stage 0: \n");
    
    if(payload_len >= 9 && packet->payload[0] == 0x04 && 
      (packet->payload[1] == 0x01 || packet->payload[1] == 0x02) &&
      packet->payload[payload_len - 1] == 0x00)  {
      NDPI_LOG_DBG2(ndpi_struct, "Possible SOCKS4 request detected, we will look further for the response\n");
      /* TODO: check port and ip address is valid */
      /* Encode the direction of the packet in the stage, so we will know when we need to look for the response packet. */
      flow->l4.tcp.socks4_stage = packet->packet_direction + 1;
    }
  } else {
    NDPI_LOG_DBG2(ndpi_struct, "SOCKS4 stage %u: \n", flow->l4.tcp.socks4_stage);

    /* At first check, if this is for sure a response packet (in another direction. If not, do nothing now and return. */
    if((flow->l4.tcp.socks4_stage - packet->packet_direction) == 1) {
      return;
    }
    /* This is a packet in another direction. Check if we find the proper response. */
    if(payload_len == 8 && packet->payload[0] == 0x00 && packet->payload[1] >= 0x5a && packet->payload[1] <= 0x5d) {
      NDPI_LOG_INFO(ndpi_struct, "found SOCKS4\n");
      ndpi_int_socks_add_connection(ndpi_struct, flow);
    } else {
      NDPI_LOG_DBG2(ndpi_struct, "The reply did not seem to belong to SOCKS4, resetting the stage to 0\n");
      flow->l4.tcp.socks4_stage = 0;
    }
  }
}

static void ndpi_check_socks5(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = ndpi_get_packet_struct(ndpi_struct);
  u_int32_t payload_len = packet->payload_packet_len;

  /* Check if we so far detected the protocol in the request or not. */
  if(flow->l4.tcp.socks5_stage == 0) {
    NDPI_LOG_DBG2(ndpi_struct, "SOCKS5 stage 0: \n");

    if(((payload_len == 3) && (packet->payload[0] == 0x05) && (packet->payload[1] == 0x01) && (packet->payload[2] == 0x00)) ||
       ((payload_len == 4) && (packet->payload[0] == 0x05) && (packet->payload[1] == 0x02) && (packet->payload[2] == 0x00) && (packet->payload[3] == 0x01))) {
      NDPI_LOG_DBG2(ndpi_struct, "Possible SOCKS5 request detected, we will look further for the response\n");

      /* Encode the direction of the packet in the stage, so we will know when we need to look for the response packet. */
      flow->l4.tcp.socks5_stage = packet->packet_direction + 1;
    }

  } else {
    NDPI_LOG_DBG2(ndpi_struct, "SOCKS5 stage %u: \n", flow->l4.tcp.socks5_stage);

    /* At first check, if this is for sure a response packet (in another direction. If not, do nothing now and return. */
    if((flow->l4.tcp.socks5_stage - packet->packet_direction) == 1) {
      return;
    }

    /* This is a packet in another direction. Check if we find the proper response. */
    if((payload_len == 0) || ((payload_len == 2) && (packet->payload[0] == 0x05) && (packet->payload[1] == 0x00))) {
      NDPI_LOG_INFO(ndpi_struct, "found SOCKS5\n");
      ndpi_int_socks_add_connection(ndpi_struct, flow);
    } else {
      NDPI_LOG_DBG2(ndpi_struct, "The reply did not seem to belong to SOCKS5, resetting the stage to 0\n");
      flow->l4.tcp.socks5_stage = 0;
    }

  }
}

static void ndpi_search_socks(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  NDPI_LOG_DBG(ndpi_struct, "search SOCKS\n");

  if(flow->packet_counter >= 10) {
    NDPI_EXCLUDE_DISSECTOR(ndpi_struct, flow);
    return;
  }

  ndpi_check_socks4(ndpi_struct, flow);

  if(flow->detected_protocol_stack[0] != NDPI_PROTOCOL_SOCKS)
    ndpi_check_socks5(ndpi_struct, flow);
}

void init_socks_dissector(struct ndpi_detection_module_struct *ndpi_struct)
{
  register_dissector("SOCKS", ndpi_struct,
                     ndpi_search_socks,
                     NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
                     1, NDPI_PROTOCOL_SOCKS);
}


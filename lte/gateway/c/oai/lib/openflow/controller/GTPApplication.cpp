/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string>

#include "GTPApplication.h"
#include "IMSIEncoder.h"
#include "gtpv1u.h"

extern "C" {
#include "log.h"
#include "bstrlib.h"
}

using namespace fluid_msg;

namespace openflow {

const std::string GTPApplication::GTP_PORT_MAC = "02:00:00:00:00:01";

GTPApplication::GTPApplication(
  const std::string &uplink_mac,
  uint32_t gtp_port_num):
  uplink_mac_(uplink_mac),
  gtp_port_num_(gtp_port_num)
{
}

void GTPApplication::event_callback(
  const ControllerEvent &ev,
  const OpenflowMessenger &messenger)
{
  if (ev.get_type() == EVENT_ADD_GTP_TUNNEL) {
    auto add_tunnel_event = static_cast<const AddGTPTunnelEvent &>(ev);
    add_uplink_tunnel_flow(add_tunnel_event, messenger);
    add_downlink_tunnel_flow(add_tunnel_event, messenger);
  } else if (ev.get_type() == EVENT_DELETE_GTP_TUNNEL) {
    auto del_tunnel_event = static_cast<const DeleteGTPTunnelEvent &>(ev);
    delete_uplink_tunnel_flow(del_tunnel_event, messenger);
    delete_downlink_tunnel_flow(del_tunnel_event, messenger);
  } else if (ev.get_type() == EVENT_DISCARD_DATA_ON_GTP_TUNNEL) {
    auto discard_tunnel_flow =
      static_cast<const HandleDataOnGTPTunnelEvent &>(ev);
    discard_uplink_tunnel_flow(discard_tunnel_flow, messenger);
    discard_downlink_tunnel_flow(discard_tunnel_flow, messenger);
  } else if (ev.get_type() == EVENT_FORWARD_DATA_ON_GTP_TUNNEL) {
    auto forward_tunnel_flow =
      static_cast<const HandleDataOnGTPTunnelEvent &>(ev);
    forward_uplink_tunnel_flow(forward_tunnel_flow, messenger);
    forward_downlink_tunnel_flow(forward_tunnel_flow, messenger);
  }
}

/*
 * Helper method to add matching for adding/deleting the uplink flow
 */
void add_uplink_match(
  of13::FlowMod &uplink_fm,
  uint32_t gtp_port,
  uint32_t i_tei)
{
  // Match on tunnel id and gtp in port
  of13::InPort gtp_port_match(gtp_port);
  uplink_fm.add_oxm_field(gtp_port_match);
  of13::TUNNELId in_tunnel_id(i_tei);
  uplink_fm.add_oxm_field(in_tunnel_id);
}

/*
 * Helper method to add imsi as metadata to the packet
 */
void add_imsi_metadata(
  of13::ApplyActions &apply_actions,
  const std::string &imsi)
{
  auto metadata_field = new of13::Metadata(IMSIEncoder::compact_imsi(imsi));
  of13::SetFieldAction set_metadata(metadata_field);
  apply_actions.add_action(set_metadata);
}

void GTPApplication::add_uplink_tunnel_flow(
  const AddGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod uplink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_ADD, DEFAULT_PRIORITY);
  add_uplink_match(uplink_fm, gtp_port_num_, ev.get_in_tei());

  // Set eth src and dst
  of13::ApplyActions apply_ul_inst;
  EthAddress gtp_port(GTP_PORT_MAC);
  // libfluid handles memory freeing of fields
  of13::SetFieldAction set_eth_src(new of13::EthSrc(gtp_port));
  apply_ul_inst.add_action(set_eth_src);

  EthAddress uplink_port(uplink_mac_);
  of13::SetFieldAction set_eth_dst(new of13::EthDst(uplink_port));
  apply_ul_inst.add_action(set_eth_dst);

  // add imsi to packet metadata to pass to other tables
  add_imsi_metadata(apply_ul_inst, ev.get_imsi());

  uplink_fm.add_instruction(apply_ul_inst);

  // Output to inout table
  of13::GoToTable goto_inst(NEXT_TABLE);
  uplink_fm.add_instruction(goto_inst);

  // Finally, send flow mod
  messenger.send_of_msg(uplink_fm, ev.get_connection());
  OAILOG_DEBUG(LOG_GTPV1U, "Uplink flow added\n");
}

void GTPApplication::delete_uplink_tunnel_flow(
  const DeleteGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod uplink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_DELETE, 0);
  // match all ports and groups
  uplink_fm.out_port(of13::OFPP_ANY);
  uplink_fm.out_group(of13::OFPG_ANY);

  add_uplink_match(uplink_fm, gtp_port_num_, ev.get_in_tei());

  messenger.send_of_msg(uplink_fm, ev.get_connection());
}

/*
 * Helper method to add matching for adding/deleting the downlink flow
 */
static void add_downlink_match(of13::FlowMod &downlink_fm,
    const struct in_addr &ue_ip)
{
  // Set match on uplink port and IP eth type
  of13::InPort uplink_port_match(of13::OFPP_LOCAL);
  downlink_fm.add_oxm_field(uplink_port_match);
  of13::EthType ip_type(0x0800);
  downlink_fm.add_oxm_field(ip_type);

  // Match UE IP destination
  of13::IPv4Dst ip_match(ue_ip.s_addr);
  downlink_fm.add_oxm_field(ip_match);
}

static void add_ded_brr_dl_match(of13::FlowMod &downlink_fm,
    const struct ipv4flow_dl &flow)
{
  // Set match on uplink port and IP eth type
  of13::InPort uplink_port_match(of13::OFPP_LOCAL);
  downlink_fm.add_oxm_field(uplink_port_match);
  of13::EthType ip_type(0x0800);
  downlink_fm.add_oxm_field(ip_type);

  // Match UE IP destination
  if (flow.set_params & DST_IPV4) {
    of13::IPv4Dst ipv4_dst(flow.dst_ip.s_addr);
    downlink_fm.add_oxm_field(ipv4_dst);
  }

  // Match IP source
  if (flow.set_params & SRC_IPV4) {
    of13::IPv4Src ipv4_src(flow.src_ip.s_addr);
    downlink_fm.add_oxm_field(ipv4_src);
  }

  if (flow.set_params & IP_PROTO) {
    of13::IPProto proto(flow.ip_proto);
    downlink_fm.add_oxm_field(proto);
  }

  if (flow.set_params & TCP_SRC_PORT) {
    of13::TCPSrc tcp_src_port(flow.tcp_src_port);
    downlink_fm.add_oxm_field(tcp_src_port);
  }

  if (flow.set_params & TCP_DST_PORT) {
    of13::TCPDst tcp_dst_port(flow.tcp_dst_port);
    downlink_fm.add_oxm_field(tcp_dst_port);
  }

  if (flow.set_params & UDP_SRC_PORT) {
    of13::UDPSrc udp_src_port(flow.udp_src_port);
    downlink_fm.add_oxm_field(udp_src_port);
  }

  if (flow.set_params & UDP_DST_PORT) {
    of13::UDPDst udp_dst_port(flow.udp_dst_port);
    downlink_fm.add_oxm_field(udp_dst_port);
  }
}

void GTPApplication::add_downlink_tunnel_flow(
  const AddGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod downlink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_ADD, DEFAULT_PRIORITY);

  if (ev.is_dl_flow_valid()) {
    add_ded_brr_dl_match(downlink_fm, ev.get_dl_flow());
  } else {
    add_downlink_match(downlink_fm, ev.get_ue_ip());
  }

  of13::ApplyActions apply_dl_inst;

  // Set outgoing tunnel id and tunnel destination ip
  of13::SetFieldAction set_out_tunnel(new of13::TUNNELId(ev.get_out_tei()));
  apply_dl_inst.add_action(set_out_tunnel);
  of13::SetFieldAction set_tunnel_dst(
    new of13::TunnelIPv4Dst(ev.get_enb_ip().s_addr));
  apply_dl_inst.add_action(set_tunnel_dst);

  // add imsi to packet metadata to pass to other tables
  add_imsi_metadata(apply_dl_inst, ev.get_imsi());

  // Output to inout table
  of13::GoToTable goto_inst(NEXT_TABLE);

  downlink_fm.add_instruction(apply_dl_inst);
  downlink_fm.add_instruction(goto_inst);

  // Finally, send flow mod
  messenger.send_of_msg(downlink_fm, ev.get_connection());
  OAILOG_DEBUG(LOG_GTPV1U, "Downlink flow added\n");
}

void GTPApplication::delete_downlink_tunnel_flow(
  const DeleteGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod downlink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_DELETE, 0);
  // match all ports and groups
  downlink_fm.out_port(of13::OFPP_ANY);
  downlink_fm.out_group(of13::OFPG_ANY);

  if (ev.is_dl_flow_valid()) {
    add_ded_brr_dl_match(downlink_fm, ev.get_dl_flow());
  } else {
    add_downlink_match(downlink_fm, ev.get_ue_ip());
  }


  messenger.send_of_msg(downlink_fm, ev.get_connection());
}

void GTPApplication::discard_uplink_tunnel_flow(
  const HandleDataOnGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod uplink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_ADD, DEFAULT_PRIORITY + 1);
  // match all ports and groups
  uplink_fm.out_port(of13::OFPP_ANY);
  uplink_fm.out_group(of13::OFPG_ANY);
  uplink_fm.cookie(cookie);
  uplink_fm.cookie_mask(cookie);

  add_uplink_match(uplink_fm, gtp_port_num_, ev.get_in_tei());

  messenger.send_of_msg(uplink_fm, ev.get_connection());
}

void GTPApplication::discard_downlink_tunnel_flow(
  const HandleDataOnGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod downlink_fm =
    messenger.create_default_flow_mod(0, of13::OFPFC_ADD, DEFAULT_PRIORITY + 1);
  // match all ports and groups
  downlink_fm.out_port(of13::OFPP_ANY);
  downlink_fm.out_group(of13::OFPG_ANY);
  downlink_fm.cookie(cookie + 1);
  downlink_fm.cookie_mask(cookie + 1);

  if (ev.is_dl_flow_valid()) {
    add_ded_brr_dl_match(downlink_fm, ev.get_dl_flow());
  } else {
    add_downlink_match(downlink_fm, ev.get_ue_ip());
  }


  messenger.send_of_msg(downlink_fm, ev.get_connection());
}

void GTPApplication::forward_uplink_tunnel_flow(
  const HandleDataOnGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod uplink_fm = messenger.create_default_flow_mod(
    0, of13::OFPFC_DELETE, DEFAULT_PRIORITY + 1);
  // match all ports and groups
  uplink_fm.out_port(of13::OFPP_ANY);
  uplink_fm.out_group(of13::OFPG_ANY);
  uplink_fm.cookie(cookie);
  uplink_fm.cookie_mask(cookie);

  add_uplink_match(uplink_fm, gtp_port_num_, ev.get_in_tei());

  messenger.send_of_msg(uplink_fm, ev.get_connection());
}

void GTPApplication::forward_downlink_tunnel_flow(
  const HandleDataOnGTPTunnelEvent &ev,
  const OpenflowMessenger &messenger)
{
  of13::FlowMod downlink_fm = messenger.create_default_flow_mod(
    0, of13::OFPFC_DELETE, DEFAULT_PRIORITY + 1);
  // match all ports and groups
  downlink_fm.out_port(of13::OFPP_ANY);
  downlink_fm.out_group(of13::OFPG_ANY);
  downlink_fm.cookie(cookie + 1);
  downlink_fm.cookie_mask(cookie + 1);

  if (ev.is_dl_flow_valid()) {
    add_ded_brr_dl_match(downlink_fm, ev.get_dl_flow());
  } else {
    add_downlink_match(downlink_fm, ev.get_ue_ip());
  }


  messenger.send_of_msg(downlink_fm, ev.get_connection());
}

} // namespace openflow

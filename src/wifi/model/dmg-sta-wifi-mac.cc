/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2015, 2016 IMDEA Networks Institute
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Hany Assasa <Hany.assasa@gmail.com>
 */
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/trace-source-accessor.h"

#include "amsdu-subframe-header.h"
#include "dcf-manager.h"
#include "dmg-capabilities.h"
#include "dmg-sta-wifi-mac.h"
#include "ext-headers.h"
#include "mgt-headers.h"
#include "mac-low.h"
#include "msdu-aggregator.h"
#include "qos-tag.h"
#include "wifi-mac-header.h"
#include "random-stream.h"
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("DmgStaWifiMac");

NS_OBJECT_ENSURE_REGISTERED (DmgStaWifiMac);

TypeId
DmgStaWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::DmgStaWifiMac")
    .SetParent<DmgWifiMac> ()
    .AddConstructor<DmgStaWifiMac> ()
    .AddAttribute ("ProbeRequestTimeout", "The interval between two consecutive probe request attempts.",
                    TimeValue (Seconds (0.05)),
                    MakeTimeAccessor (&DmgStaWifiMac::m_probeRequestTimeout),
                    MakeTimeChecker ())
    .AddAttribute ("AssocRequestTimeout", "The interval between two consecutive assoc request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&DmgStaWifiMac::m_assocRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("MaxMissedBeacons",
                   "Number of beacons which much be consecutively missed before "
                   "we attempt to restart association.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&DmgStaWifiMac::m_maxMissedBeacons),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ActiveProbing",
                   "If true, we send probe requests. If false, we don't."
                   "NOTE: if more than one STA in your simulation is using active probing, "
                   "you should enable it at a different simulation time for each STA, "
                   "otherwise all the STAs will start sending probes at the same time resulting in collisions. "
                   "See bug 1060 for more info.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&DmgStaWifiMac::SetActiveProbing, &DmgStaWifiMac::GetActiveProbing),
                   MakeBooleanChecker ())
    .AddTraceSource ("Assoc", "Associated with an access point.",
                     MakeTraceSourceAccessor (&DmgStaWifiMac::m_assocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("DeAssoc", "Association with an access point lost.",
                     MakeTraceSourceAccessor (&DmgStaWifiMac::m_deAssocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("ChannelReportReceived", "The DMG STA has received a channel report",
                     MakeTraceSourceAccessor (&DmgStaWifiMac::m_channelReportReceived),
                     "ns3::Mac48Address::TracedCallback")
  ;
  return tid;
}

DmgStaWifiMac::DmgStaWifiMac ()
  : m_state (BEACON_MISSED),
    m_probeRequestEvent (),
    m_assocRequestEvent (),
    m_beaconWatchdogEnd (Seconds (0.0)),
    m_abftEvent (),
    m_atiPresent (false)
{
  NS_LOG_FUNCTION (this);

  a_bftSlot = CreateObject<UniformRandomVariable> ();

  // Let the lower layers know that we are acting as a non-AP DMG STA in an infrastructure BSS.
  SetTypeOfStation (DMG_STA);
}

DmgStaWifiMac::~DmgStaWifiMac ()
{
  NS_LOG_FUNCTION(this);
}

void
DmgStaWifiMac::DoInitialize (void)
{
  NS_LOG_FUNCTION (this);
  DmgWifiMac::DoInitialize ();
  /* Initialize DMG STA */
  StartBeaconTransmissionInterval ();
}

void
DmgStaWifiMac::DoDispose ()
{
  NS_LOG_FUNCTION (this);
  DmgWifiMac::DoDispose ();
}

void
DmgStaWifiMac::SetWifiRemoteStationManager (Ptr<WifiRemoteStationManager> stationManager)
{
  NS_LOG_FUNCTION (this << stationManager);
  DmgWifiMac::SetWifiRemoteStationManager (stationManager);
}

void
DmgStaWifiMac::SetMaxMissedBeacons(uint32_t missed)
{
  NS_LOG_FUNCTION (this << missed);
  m_maxMissedBeacons = missed;
}

void
DmgStaWifiMac::SetProbeRequestTimeout (Time timeout)
{
    NS_LOG_FUNCTION (this << timeout);
    m_probeRequestTimeout = timeout;
}

void
DmgStaWifiMac::SetAssocRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_assocRequestTimeout = timeout;
}

void
DmgStaWifiMac::StartActiveAssociation (void)
{
  NS_LOG_FUNCTION (this);
  TryToEnsureAssociated ();
}

void
DmgStaWifiMac::SetActiveProbing (bool enable)
{
  NS_LOG_FUNCTION(this << enable);
  if (enable)
    {
      Simulator::ScheduleNow (&DmgStaWifiMac::TryToEnsureAssociated, this);
    }
  else
    {
      m_probeRequestEvent.Cancel ();
    }
  m_activeProbing = enable;
}
  
bool
DmgStaWifiMac::GetActiveProbing (void) const
{
  return m_activeProbing;
}

void
DmgStaWifiMac::SendProbeRequest (void)
{
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetProbeReq ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeRequestHeader probe;
  probe.SetSsid (GetSsid ());

  packet->AddHeader (probe);

  // The standard is not clear on the correct queue for management
  // frames if we are a QoS AP. The approach taken here is to always
  // use the DCF for these regardless of whether we have a QoS
  // association or not.
  m_dca->Queue(packet, hdr);

  if (m_probeRequestEvent.IsRunning())
    {
      m_probeRequestEvent.Cancel();
    }
  m_probeRequestEvent = Simulator::Schedule(m_probeRequestTimeout, &DmgStaWifiMac::ProbeRequestTimeout, this);
}

void
DmgStaWifiMac::SendAssociationRequest (void)
{
  NS_LOG_FUNCTION (this << GetBssid ());
  WifiMacHeader hdr;
  hdr.SetAssocReq ();
  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetBssid ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  hdr.SetNoOrder ();

  Ptr<Packet> packet = Create<Packet> ();
  MgtAssocRequestHeader assoc;
  assoc.SetSsid (GetSsid ());
  assoc.AddWifiInformationElement (GetDmgCapabilities ());
  assoc.AddWifiInformationElement (GetMultiBandElement ());
  assoc.AddWifiInformationElement (GetRelayCapabilities ());
  packet->AddHeader (assoc);

  // The standard is not clear on the correct queue for management
  // frames if we are a QoS AP. The approach taken here is to always
  // use the DCF for these regardless of whether we have a QoS
  // association or not.
  m_dca->Queue (packet, hdr);

  if (m_assocRequestEvent.IsRunning ())
    {
      m_assocRequestEvent.Cancel ();
    }

  /* For now, we assume station talks to the DMG AP only */
  ANTENNA_CONFIGURATION_TX antennaConfigTx = m_bestAntennaConfig[GetBssid ()].first;
  ANTENNA_CONFIGURATION_RX antennaConfigRx = m_bestAntennaConfig[GetBssid ()].second;
  m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (antennaConfigTx.first);
  m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaConfigTx.second);
  if (antennaConfigRx.first != 0)
    {
      /* Make sure we have trained Rx Antenna sector */
      m_phy->GetDirectionalAntenna ()->SetCurrentRxSectorID (antennaConfigRx.first);
      m_phy->GetDirectionalAntenna ()->SetCurrentRxAntennaID (antennaConfigRx.second);
    }

  m_assocRequestEvent = Simulator::Schedule (m_assocRequestTimeout, &DmgStaWifiMac::AssocRequestTimeout, this);
}

void 
DmgStaWifiMac::TryToEnsureAssociated (void)
{
  NS_LOG_FUNCTION (this);
  switch (m_state)
  {
  case ASSOCIATED:
    return;
    break;

  case WAIT_PROBE_RESP:
    /* we have sent a probe request earlier so we
     do not need to re-send a probe request immediately.
     We just need to wait until probe-request-timeout
     or until we get a probe response
    */
    break;

  case BEACON_MISSED:
    /* we were associated but we missed a bunch of beacons
    * so we should assume we are not associated anymore.
    * We try to initiate a probe request now.
    */
    m_linkDown ();
    if (m_activeProbing)
      {
        SetState (WAIT_PROBE_RESP);
        SendProbeRequest ();
      }
    break;

  case WAIT_ASSOC_RESP:
    /* we have sent an assoc request so we do not need to
     re-send an assoc request right now. We just need to
     wait until either assoc-request-timeout or until
     we get an assoc response.
    */
    break;

  case REFUSED:
    /* we have sent an assoc request and received a negative
     assoc resp. We wait until someone restarts an
     association with a given ssid.
    */
    break;
  }
}

void
DmgStaWifiMac::AssocRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_ASSOC_RESP);
  SendAssociationRequest ();
}

uint16_t
DmgStaWifiMac::GetAssociationID (void)
{
  NS_LOG_FUNCTION (this);
  if (m_state == ASSOCIATED)
    {
      return m_aid;
    }
  else
    {
      return 0;
    }
}

void
DmgStaWifiMac::MapAidToMacAddress (uint16_t aid, Mac48Address address)
{
  NS_LOG_FUNCTION (this << aid << address);
  m_aidMap[aid] = address;
  m_macMap[address] = aid;
}

void
DmgStaWifiMac::ProbeRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_PROBE_RESP);
  SendProbeRequest ();
}

void
DmgStaWifiMac::MissedBeacons (void)
{
  NS_LOG_FUNCTION (this);
  if (m_beaconWatchdogEnd > Simulator::Now ())
  {
    if (m_beaconWatchdog.IsRunning ())
      {
        m_beaconWatchdog.Cancel ();
      }
    m_beaconWatchdog = Simulator::Schedule (m_beaconWatchdogEnd - Simulator::Now (), &DmgStaWifiMac::MissedBeacons, this);
    return;
  }
  NS_LOG_DEBUG ("beacon missed");
  SetState (BEACON_MISSED);
  TryToEnsureAssociated ();
}

void
DmgStaWifiMac::RestartBeaconWatchdog (Time delay)
{
  NS_LOG_FUNCTION (this << delay);
  m_beaconWatchdogEnd = std::max (Simulator::Now () + delay, m_beaconWatchdogEnd);
  if (Simulator::GetDelayLeft (m_beaconWatchdog) < delay && m_beaconWatchdog.IsExpired ())
    {
      NS_LOG_DEBUG ("Restart watchdog.");
      m_beaconWatchdog = Simulator::Schedule (delay, &DmgStaWifiMac::MissedBeacons, this);
    }
}

bool
DmgStaWifiMac::IsAssociated (void) const
{
  return m_state == ASSOCIATED;
}

bool
DmgStaWifiMac::IsWaitAssocResp (void) const
{
  return m_state == WAIT_ASSOC_RESP;
}

void
DmgStaWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NS_LOG_FUNCTION (this << packet << to);
  if (!IsAssociated ())
    {
      NotifyTxDrop (packet);
      TryToEnsureAssociated ();
      return;
    }
  WifiMacHeader hdr;

  // If we are not a QoS AP then we definitely want to use AC_BE to
  // transmit the packet. A TID of zero will map to AC_BE (through \c
  // QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

  // For now, an AP that supports QoS does not support non-QoS
  // associations, and vice versa. In future the AP model should
  // support simultaneously associated QoS and non-QoS STAs, at which
  // point there will need to be per-association QoS state maintained
  // by the association state machine, and consulted here.

  /* The QoS Data and QoS Null subtypes are the only Data subtypes transmitted by a DMG STA. */
  hdr.SetType (WIFI_MAC_QOSDATA);
  hdr.SetQosAckPolicy (WifiMacHeader::NORMAL_ACK);
  hdr.SetQosNoEosp ();
  hdr.SetQosNoAmsdu ();
  // Transmission of multiple frames in the same TXOP is not
  // supported for now.
  hdr.SetQosTxopLimit (0);
  // Fill in the QoS control field in the MAC header
  tid = QosUtilsGetTidForPacket (packet);
  // Any value greater than 7 is invalid and likely indicates that
  // the packet had no QoS tag, so we revert to zero, which'll
  // mean that AC_BE is used.
  if (tid > 7)
    {
      tid = 0;
    }
  hdr.SetQosTid (tid);
  /* DMG QoS Control*/
  hdr.SetQosRdGrant (m_supportRdp);
  /* The HT Control field is not present in frames transmitted by a DMG STA.
   * The presence of the HT Control field is determined by the Order
   * subfield of the Frame Control field, as specified in 8.2.4.1.10.*/
  hdr.SetNoOrder ();

  // Sanity check that the TID is valid
  NS_ASSERT (tid < 8);

  SetHeaderAddresses (to, hdr);

  /* Check whether we should transmit in CBAP or SP */
  bool found = false;
  for (std::list<Mac48Address>::iterator iter = m_spStations.begin (); iter != m_spStations.end (); iter++)
    {
      if ((*iter) == to)
        {
          m_sp->Queue (packet, hdr);
          found = true;
          return;
        }
    }
  if (!found)
    {
      m_edca[QosUtilsMapTidToAc (tid)]->Queue (packet, hdr);
    }
}

void
DmgStaWifiMac::SetHeaderAddresses (Mac48Address destAddress, WifiMacHeader &hdr)
{
  NS_LOG_FUNCTION (this << destAddress);
  DataForwardingMapIterator it;
  bool found = false;
  for (it = m_dataForwardingMap.begin (); it != m_dataForwardingMap.end (); it++)
    {
      if ((*it) == destAddress)
        {
          found = true;
          break;
        }
    }
  if (found)
    {
      /* We are in Ad-Hoc Mode  (STA-STA)*/
      hdr.SetAddr1 (destAddress);
      hdr.SetAddr2 (GetAddress ());
      hdr.SetAddr3 (GetBssid ());
      hdr.SetDsNotFrom ();
      hdr.SetDsNotTo ();
    }
  else
    {
      /* The AP is our receiver */
      hdr.SetAddr1 (GetBssid ());
      hdr.SetAddr2 (GetAddress ());
      hdr.SetAddr3 (destAddress);
      hdr.SetDsNotFrom ();
      hdr.SetDsTo ();
    }
}

void
DmgStaWifiMac::SendSprFrame (Mac48Address to)
{
  NS_LOG_FUNCTION (this << to);
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_DMG_SPR);
  hdr.SetAddr1 (to);            //RA Field (MAC Address of the STA being polled)
  hdr.SetAddr2 (GetAddress ()); //TA Field (MAC Address of the PCP or AP)
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  CtrlDMG_SPR spr;

  Dynamic_Allocation_Info_Field dynamicInfo;
  BF_Control_Field bfField;

  dynamicInfo.SetSourceAID (m_aid);
  dynamicInfo.SetAllocationDuration (32000);

  spr.SetDynamicAllocationInfo (dynamicInfo);
  spr.SetBFControl (bfField);

  packet->AddHeader (spr);
  m_dmgAtiDca->Queue (packet, hdr);
}

void
DmgStaWifiMac::StartBeaconTransmissionInterval (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_INFO ("DMG STA Starting BTI at " << Simulator::Now ());
  m_accessPeriod = CHANNEL_ACCESS_BTI;

  /* Re-initialize variables */
  m_scheduledPeriodAfterAbft = false;
  m_sectorFeedbackSent.clear ();

  /* Disable Channel Access by CBAPs and SPs */
  m_dcfManager->DisableChannelAccess ();
  m_sp->DisableChannelAccess ();
  if (m_rdsOperational)
    {
      m_phy->SuspendRdsOperation ();
    }

  /* At BTI period, a DMG STA should be in Omni receiving mode */
  m_phy->GetDirectionalAntenna ()->SetInOmniReceivingMode ();
}

void
DmgStaWifiMac::StartAssociationBeamformTraining (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_INFO ("DMG STA Starting A-BFT at " << Simulator::Now ());
  m_accessPeriod = CHANNEL_ACCESS_A_BFT;

  /* Choose a random SSW Slot to transmit SSW Frames in it */
  a_bftSlot->SetAttribute ("Min", DoubleValue (0));
  a_bftSlot->SetAttribute ("Max", DoubleValue (m_remainingSlotsPerABFT - 1));
  m_slotIndex = a_bftSlot->GetInteger ();

  Time rssTime = m_slotIndex * m_low->GetSectorSweepSlotTime (m_ssFramesPerSlot);
  Simulator::Schedule (rssTime, &DmgStaWifiMac::StartResponderSectorSweep,
                       this, GetBssid (), m_isResponderTXSS, m_low->GetSectorSweepDuration (m_ssFramesPerSlot));
  NS_LOG_DEBUG ("Choosing Sector Slot Index=" << uint (m_slotIndex) << " Start RSS at " << Simulator::Now () + rssTime);

  if (!m_scheduledPeriodAfterAbft)
    {
      if (m_atiPresent)
        {
          Simulator::Schedule (m_abftDuration, &DmgStaWifiMac::StartAnnouncementTransmissionInterval, this);
          NS_LOG_DEBUG ("ATI for Station:" << GetAddress () << " is scheduled at " << Simulator::Now () + m_abftDuration);
        }
      else
        {
          Simulator::Schedule (m_abftDuration, &DmgStaWifiMac::StartDataTransmissionInterval, this);
          NS_LOG_DEBUG ("DTI for Station:" << GetAddress () << " is scheduled at " << Simulator::Now () + m_abftDuration);
        }
      m_scheduledPeriodAfterAbft = true;
    }

  if (m_remainingSlotsPerABFT > 0)
    {
      /* Schedule SSW FBCK Timeout to detect a collision i.e. missing SSW-FBCK */
      Time timeout = (m_slotIndex + 1) * m_low->GetSectorSweepSlotTime (m_ssFramesPerSlot);
      NS_LOG_DEBUG ("Scheduled SSW-FBCK Timeout Event at " << Simulator::Now () + timeout);
      m_sswFbckTimeout = Simulator::Schedule (timeout, &DmgStaWifiMac::StartAssociationBeamformTraining, this);
      /* Update upper bound of slots */
      m_remainingSlotsPerABFT -= (m_slotIndex + 1);
    }
}

void
DmgStaWifiMac::StartAnnouncementTransmissionInterval (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_INFO ("DMG STA Starting ATI at " << Simulator::Now ());
  m_accessPeriod = CHANNEL_ACCESS_ATI;
  m_scheduledPeriodAfterAbft = false;
  /* We started ATI Period we should stay in Omni Drectional waiting for packets */
  m_phy->GetDirectionalAntenna ()->SetInOmniReceivingMode ();
  Simulator::Schedule (m_atiDuration, &DmgStaWifiMac::StartDataTransmissionInterval, this);
  m_dmgAtiDca->InitiateAtiAccessPeriod (m_atiDuration);
}

void
DmgStaWifiMac::StartDataTransmissionInterval (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_INFO ("DMG STA Starting DTI at " << Simulator::Now ());
  m_accessPeriod = CHANNEL_ACCESS_DTI;

  /* Initialize DMG Reception */
  m_receivedDmgBeacon = false;

  /* Schedule the beginning of the next BI interval */
  Time nextBeaconInterval = m_beaconInterval - (Simulator::Now () - m_btiStarted);
  Simulator::Schedule (nextBeaconInterval, &DmgStaWifiMac::StartBeaconTransmissionInterval, this);
  NS_LOG_DEBUG ("Next Beacon Interval will start at " << Simulator::Now () + nextBeaconInterval);

  /* Check whether we work in RDS mode or not*/
  if (m_rdsOperational)
    {
      m_phy->ResumeRdsOperation ();
    }
  else
    {
      /* Send Association Request if we are not assoicated */
      if (!IsAssociated ())
        {
          /* We allow normal DCA for access */

          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }

      /**
        * A STA shall not transmit within a CBAP unless at least one of the following conditions is met:
        * — The value of the CBAP Only field is equal to 1 and the value of the CBAP Source field is equal to 0
        *   within the DMG Parameters field of the DMG Beacon that allocates the CBAP
        * — The STA is a PCP/AP and the value of the CBAP Only field is equal to 1 and the value of the CBAP
        *   Source field is equal to 1 within the DMG Parameters field of the DMG Beacon that allocates the CBAP
        * — The value of the Source AID field of the CBAP is equal to the broadcast AID
        * — The STA’s AID is equal to the value of the Source AID field of the CBAP
        * — The STA’s AID is equal to the value of the Destination AID field of the CBAP
        */
      if (m_isCbapOnly && !m_isCbapSource)
        {
          NS_LOG_INFO ("CBAP allocation only in DTI");
          Simulator::ScheduleNow (&DmgStaWifiMac::StartContentionPeriod, this, nextBeaconInterval);
        }
      else
        {
          AllocationField field;
          for (AllocationFieldList::iterator iter = m_allocationList.begin (); iter != m_allocationList.end (); iter++)
            {
              field = (*iter);
              if (field.GetAllocationType () == SERVICE_PERIOD_ALLOCATION)
                {
                  Time spStart = MicroSeconds (field.GetAllocationStart ());
                  if (field.GetSourceAid () == m_aid)
                    {
                      Mac48Address destAddress = m_aidMap[field.GetDestinationAid ()];
                      if (field.GetBfControl ().IsBeamformTraining ())
                        {
                          Simulator::Schedule (spStart, &DmgStaWifiMac::InitiateBeamforming, this, destAddress,
                                               field.GetBfControl ().IsInitiatorTxss (),
                                               MicroSeconds (field.GetAllocationBlockDuration ()));
                        }
                      else
                        {
                          /* Add station to the list of stations */
                          m_spStations.push_back (destAddress);
                          /* Schedule two events: one for the beginning of the SP and another for the end of SP */
                          Time spEnd = spStart + MicroSeconds (field.GetAllocationBlockDuration ());
                          Simulator::Schedule (spStart, &DmgStaWifiMac::StartServicePeriod,
                                               this, MicroSeconds (field.GetAllocationBlockDuration ()), destAddress, true);
                          Simulator::Schedule (spEnd, &DmgStaWifiMac::EndServicePeriod, this);
                        }
                    }
                  else if ((field.GetAllocationType () == SERVICE_PERIOD_ALLOCATION) &&
                           (field.GetSourceAid () == 255) && (field.GetDestinationAid () == 0xFF))
                    {
                      /**
                       * The PCP/AP may create SPs in its beacon interval with the source and destination AID
                       * subfields within an Allocation field set to 255 to prevent transmissions during
                       * specific periods in the beacon interval.
                       */
                      NS_LOG_INFO ("No transmission is allowed from " << field.GetAllocationStart () <<
                                   " till " << field.GetAllocationBlockDuration ());
                    }
                  else if ((field.GetAllocationType () == SERVICE_PERIOD_ALLOCATION) &&
                          ((field.GetDestinationAid () == m_aid) || (field.GetDestinationAid () == 0xFF)))
                    {
                      /**
                       * The STA identified by the Destination AID field in the Extended Schedule element
                       * should be in the receive state for the duration of the SP in order to receive
                       * transmissions from the source DMG STA.
                       */

                      /* Change Rx antenna sector to the source AID */
                      Mac48Address sourceAddress = m_aidMap[field.GetSourceAid ()];
                      Time spEnd = spStart + MicroSeconds (field.GetAllocationBlockDuration ());
                      /* Schedule two events: one for the beginning of the SP and another for the end of SP */
                      Simulator::Schedule (spStart, &DmgStaWifiMac::StartServicePeriod,
                                           this, MicroSeconds (field.GetAllocationBlockDuration ()), sourceAddress, false);
                      Simulator::Schedule (spEnd, &DmgStaWifiMac::EndServicePeriod, this);

                    }
                }
              else if ((field.GetAllocationType () == CBAP_ALLOCATION) &&
                      ((field.GetSourceAid () == 0xFF) || (field.GetSourceAid () == m_aid) || (field.GetDestinationAid () == m_aid)))

                {
                  Simulator::Schedule (MicroSeconds (field.GetAllocationStart ()), &DmgStaWifiMac::StartContentionPeriod, this
                                       , MicroSeconds (field.GetAllocationBlockDuration ()));
                }
            }
        }
    }
}

void
DmgStaWifiMac::InitiateBeamforming (Mac48Address address, bool isTxss, Time duration)
{
  NS_LOG_FUNCTION (this << address << isTxss << duration);
  NS_LOG_INFO ("DMG STA Initiating Beamforming with " << address << " at " << Simulator::Now ());
  StartInitiatorSectorSweep (address, isTxss, duration);
}

void
DmgStaWifiMac::StartInitiatorSectorSweep (Mac48Address address, bool isTxss, Time duration)
{
  NS_LOG_FUNCTION (this << address << isTxss << duration);
  NS_LOG_INFO ("DMG STA Starting ISS at " << Simulator::Now ());
  m_isIssInitiator = true;
  m_allocationStarted = Simulator::Now ();
  m_currentAllocationLength = duration;
  if (isTxss)
    {
      StartTransmitSectorSweep (address, BeamformingInitiator);
    }
  else
    {
      StartReceiveSectorSweep (address, BeamformingInitiator);
    }
}

void
DmgStaWifiMac::StartResponderSectorSweep (Mac48Address address, bool isTxss, Time duration)
{
  NS_LOG_FUNCTION (this << address << isTxss << duration);
  NS_LOG_INFO ("DMG STA Starting RSS at " << Simulator::Now ());
  m_isIssInitiator = false;
  m_allocationStarted = Simulator::Now ();
  m_currentAllocationLength = duration;
  /* Obtain antenna configuration for the highest received SNR from the DMG AP to feed it back */
  m_feedbackAntennaConfig = GetBestAntennaConfiguration (address, true);

  if (isTxss)
    {
      StartTransmitSectorSweep (address, BeamformingResponder);
    }
  else
    {
      /* The initiator is switching receive antennas at the same time. */
      m_phy->GetDirectionalAntenna ()->SetInOmniReceivingMode ();
      StartReceiveSectorSweep (address, BeamformingResponder);
    }
}

void
DmgStaWifiMac::StartTransmitSectorSweep (Mac48Address address, BeamformingDirection direction)
{
  NS_LOG_FUNCTION (this << address << direction);
  NS_LOG_INFO ("DMG STA Starting TxSS at " << Simulator::Now ());

  m_sectorId = 1;
  m_antennaId = 1;
  m_totalSectors = m_phy->GetDirectionalAntenna ()->GetNumberOfSectors () *
                   m_phy->GetDirectionalAntenna ()->GetNumberOfAntennas () - 1;

  if (direction == BeamformingInitiator)
    {
      Simulator::ScheduleNow (&DmgStaWifiMac::SendIssSectorSweepFrame, this, address,
                              direction, m_sectorId, m_antennaId, m_totalSectors);
    }
  else
    {
      Simulator::ScheduleNow (&DmgStaWifiMac::SendSectorSweepFrame, this, address,
                              direction, m_sectorId, m_antennaId, m_totalSectors);
    }
}

void
DmgStaWifiMac::StartReceiveSectorSweep (Mac48Address address, BeamformingDirection direction)
{
  NS_LOG_FUNCTION (this << address << direction);
  NS_LOG_INFO ("DMG STA Starting RxSS at " << Simulator::Now ());
}

Time
DmgStaWifiMac::GetRemainingAllocationTime (void) const
{
  return m_currentAllocationLength - (Simulator::Now () - m_allocationStarted);
}

void
DmgStaWifiMac::SendIssSectorSweepFrame (Mac48Address address, BeamformingDirection direction,
                                        uint8_t sectorID, uint8_t antennaID,  uint16_t count)
{
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_DMG_SSW);

  /* Header Duration*/
  hdr.SetDuration (GetRemainingAllocationTime ());

  /* Other Fields */
  hdr.SetAddr1 (address);           // MAC address of the STA that is the intended receiver of the sector sweep.
  hdr.SetAddr2 (GetAddress ());     // MAC address of the transmitter STA of the sector sweep frame.
  hdr.SetNoMoreFragments ();
  hdr.SetNoRetry ();

  Ptr<Packet> packet = Create<Packet> ();
  CtrlDMG_SSW sswFrame;

  DMG_SSW_Field ssw;
  ssw.SetDirection (direction);
  ssw.SetCountDown (count);
  ssw.SetSectorID (sectorID);
  ssw.SetDMGAntennaID (antennaID);

  DMG_SSW_FBCK_Field sswFeedback;
  sswFeedback.IsPartOfISS (true);
  sswFeedback.SetSector (m_totalSectors);
  sswFeedback.SetDMGAntenna (m_phy->GetDirectionalAntenna ()->GetNumberOfAntennas ());
  sswFeedback.SetPollRequired (false);

  /* Set the fields in SSW Frame */
  sswFrame.SetSswField (ssw);
  sswFrame.SetSswFeedbackField (sswFeedback);
  packet->AddHeader (sswFrame);

  /* Set Antenna Direction */
  m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (sectorID);
  m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaID);

  NS_LOG_INFO ("Sending SSW Frame " << Simulator::Now () << " with "
               << uint32_t (m_sectorId) << " " << uint32_t (m_antennaId));

  /* Send Control Frames directly without DCA + DCF Manager */
  MacLowTransmissionParameters params;
  params.EnableOverrideDurationId (hdr.GetDuration ());
  params.DisableRts ();
  params.DisableAck ();
  params.DisableNextData ();
  m_low->StartTransmission (packet,
                            &hdr,
                            params,
                            MakeCallback (&DmgStaWifiMac::FrameTxOk, this));
}

void
DmgStaWifiMac::SendRssSectorSweepFrame (Mac48Address address, BeamformingDirection direction,
                                        uint8_t sectorID, uint8_t antennaID,  uint16_t count)
{
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_DMG_SSW);

  /* Header Duration*/
  hdr.SetDuration (GetRemainingAllocationTime ());

  /* Other Fields */
  hdr.SetAddr1 (address);           // MAC address of the STA that is the intended receiver of the sector sweep.
  hdr.SetAddr2 (GetAddress ());     // MAC address of the transmitter STA of the sector sweep frame.
  hdr.SetNoMoreFragments ();
  hdr.SetNoRetry ();

  Ptr<Packet> packet = Create<Packet> ();
  CtrlDMG_SSW sswFrame;

  DMG_SSW_Field ssw;
  ssw.SetDirection (direction);
  ssw.SetCountDown (count);
  ssw.SetSectorID (sectorID);
  ssw.SetDMGAntennaID (antennaID);

  DMG_SSW_FBCK_Field sswFeedback;
  sswFeedback.IsPartOfISS (false);
  sswFeedback.SetSector (m_feedbackAntennaConfig.first);
  sswFeedback.SetDMGAntenna (m_feedbackAntennaConfig.second);
  sswFeedback.SetPollRequired (false);

  /* Set the fields in SSW Frame */
  sswFrame.SetSswField (ssw);
  sswFrame.SetSswFeedbackField (sswFeedback);
  packet->AddHeader (sswFrame);

  if (m_isResponderTXSS)
    {
      /* Set Antenna Direction */
      m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (sectorID);
      m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaID);

      NS_LOG_INFO ("Sending SSW Frame " << Simulator::Now () << " with "
                   << uint32_t (m_sectorId) << " " << uint32_t (m_antennaId));
    }

  /* Send Control Frames directly without DCA + DCF Manager */
  MacLowTransmissionParameters params;
  params.EnableOverrideDurationId (hdr.GetDuration ());
  params.DisableRts ();
  params.DisableAck ();
  params.DisableNextData ();
  m_low->StartTransmission (packet,
                            &hdr,
                            params,
                            MakeCallback (&DmgStaWifiMac::FrameTxOk, this));
}

void
DmgStaWifiMac::SendSectorSweepFrame (Mac48Address address, BeamformingDirection direction,
                                     uint8_t sectorID, uint8_t antennaID,  uint16_t count)
{ 
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_DMG_SSW);

  /* Header Duration*/
  hdr.SetDuration (GetRemainingAllocationTime ());

  /* Other Fields */
  hdr.SetAddr1 (address);           // MAC address of the STA that is the intended receiver of the sector sweep.
  hdr.SetAddr2 (GetAddress ());     // MAC address of the transmitter STA of the sector sweep frame.
  hdr.SetNoMoreFragments ();
  hdr.SetNoRetry ();

  Ptr<Packet> packet = Create<Packet> ();
  CtrlDMG_SSW sswFrame;

  DMG_SSW_Field ssw;
  ssw.SetDirection (direction);
  ssw.SetCountDown (count);
  ssw.SetSectorID (sectorID);
  ssw.SetDMGAntennaID (antennaID);

  DMG_SSW_FBCK_Field sswFeedback;
  sswFeedback.IsPartOfISS (false);
  sswFeedback.SetSector (m_feedbackAntennaConfig.first);
  sswFeedback.SetDMGAntenna (m_feedbackAntennaConfig.second);
  sswFeedback.SetPollRequired (false);

  /* Set the fields in SSW Frame */
  sswFrame.SetSswField (ssw);
  sswFrame.SetSswFeedbackField (sswFeedback);
  packet->AddHeader (sswFrame);

  if (m_isResponderTXSS)
    {
      /* Set Antenna Direction */
      m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (sectorID);
      m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaID);

      NS_LOG_INFO ("Sending SSW Frame " << Simulator::Now () << " with "
                   << uint32_t (m_sectorId) << " " << uint32_t (m_antennaId));
    }

  /* Send Control Frames directly without DCA + DCF Manager */
  MacLowTransmissionParameters params;
  params.EnableOverrideDurationId (hdr.GetDuration ());
  params.DisableRts ();
  params.DisableAck ();
  params.DisableNextData ();
  m_low->StartTransmission (packet,
                            &hdr,
                            params,
                            MakeCallback (&DmgStaWifiMac::FrameTxOk, this));
}

void
DmgStaWifiMac::SendSswFbckFrame (Mac48Address receiver)
{
  NS_LOG_FUNCTION (this);

  WifiMacHeader hdr;
  /* The Duration field is set until the end of the current allocation */
  hdr.SetDuration (GetRemainingAllocationTime ());
  hdr.SetType (WIFI_MAC_CTL_DMG_SSW_FBCK);
  hdr.SetAddr1 (receiver);        // Receiver.
  hdr.SetAddr2 (GetAddress ());   // Transmiter.

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (hdr);

  CtrlDMG_SSW_FBCK fbck;          // SSW-FBCK Frame.
  DMG_SSW_FBCK_Field feedback;    // SSW-FBCK Field.
  feedback.IsPartOfISS (false);
  /* Obtain antenna configuration for the highest received SNR from DMG STA to feed it back */
  m_feedbackAntennaConfig = GetBestAntennaConfiguration (receiver, true);
  feedback.SetSector (m_feedbackAntennaConfig.first);
  feedback.SetDMGAntenna (m_feedbackAntennaConfig.second);

  BRP_Request_Field request;
  request.SetMID_REQ (false);
  request.SetBC_REQ (false);

  BF_Link_Maintenance_Field maintenance;
  maintenance.SetIsMaster (true);

  fbck.SetSswFeedbackField (feedback);
  fbck.SetBrpRequestField (request);
  fbck.SetBfLinkMaintenanceField (maintenance);

  packet->AddHeader (fbck);
  NS_LOG_INFO ("Sending SSW-FBCK Frame to " << receiver << " at " << Simulator::Now ());

  /* Set the best sector for transmission */
  ANTENNA_CONFIGURATION_TX antennaConfigTx = m_bestAntennaConfig[receiver].first;
  m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (antennaConfigTx.first);
  m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaConfigTx.second);

  /* Send Control Frames directly without DCA + DCF Manager */
  MacLowTransmissionParameters params;
  params.EnableOverrideDurationId (hdr.GetDuration ());
  params.DisableRts ();
  params.DisableAck ();
  params.DisableNextData ();
  m_low->StartTransmission (packet,
                            &hdr,
                            params,
                            MakeCallback (&DmgStaWifiMac::FrameTxOk, this));
}

void
DmgStaWifiMac::SendSswAckFrame (Mac48Address receiver)
{
  NS_LOG_FUNCTION (this);
  /* send a SSW Feedback when you receive a SSW Slot after MBIFS. */
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_DMG_SSW_ACK);
  hdr.SetAddr1 (receiver);        // Receiver.
  hdr.SetAddr2 (GetAddress ());   // Transmiter.
  /* The Duration field is set until the end of the current allocation */
  Time duration = m_sswFbckDuration - (GetSifs () + NanoSeconds (sswAckTxTime));
  NS_ASSERT (duration > Seconds (0));
  hdr.SetDuration (m_currentAllocationLength);

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (hdr);

  CtrlDMG_SSW_FBCK ackFrame;      // SSW-ACK Frame.
  DMG_SSW_FBCK_Field feedback;    // SSW-FBCK Field.

  /* Obtain antenna configuration for the highest received SNR from DMG STA to feed it back */
  m_feedbackAntennaConfig = GetBestAntennaConfiguration (receiver, true);

  feedback.IsPartOfISS (false);
  feedback.SetSector (m_feedbackAntennaConfig.first);
  feedback.SetDMGAntenna (m_feedbackAntennaConfig.second);

  BRP_Request_Field request;
  request.SetMID_REQ (false);
  request.SetBC_REQ (false);

  BF_Link_Maintenance_Field maintenance;
  maintenance.SetIsMaster (true); /* Master of data transfer */

  ackFrame.SetSswFeedbackField (feedback);
  ackFrame.SetBrpRequestField (request);
  ackFrame.SetBfLinkMaintenanceField (maintenance);

  packet->AddHeader (ackFrame);
  NS_LOG_INFO ("Sending SSW-ACK Frame to " << receiver << " at " << Simulator::Now ());

  /* Set the best sector for transmission */
  ANTENNA_CONFIGURATION_TX antennaConfigTx = m_bestAntennaConfig[receiver].first;
  m_phy->GetDirectionalAntenna ()->SetCurrentTxSectorID (antennaConfigTx.first);
  m_phy->GetDirectionalAntenna ()->SetCurrentTxAntennaID (antennaConfigTx.second);

  /* Send Control Frames directly without DCA + DCF Manager */
  MacLowTransmissionParameters params;
  params.EnableOverrideDurationId (hdr.GetDuration ());
  params.DisableRts ();
  params.DisableAck ();
  params.DisableNextData ();
  m_low->StartTransmission (packet,
                            &hdr,
                            params,
                            MakeCallback (&DmgStaWifiMac::FrameTxOk, this));
}

void
DmgStaWifiMac::FrameTxOk (const WifiMacHeader &hdr)
{
  NS_LOG_FUNCTION (this);
  if (hdr.IsSSW ())
    {
      if (m_totalSectors > 0)
        {
          if (m_sectorId < m_phy->GetDirectionalAntenna ()->GetNumberOfSectors ())
            {
              m_sectorId++;
            }
          else if ((m_sectorId == m_phy->GetDirectionalAntenna ()->GetNumberOfSectors ()) &&
                   (m_antennaId < m_phy->GetDirectionalAntenna ()->GetNumberOfAntennas ()))
            {
              m_sectorId = 1;
              m_antennaId++;
            }

          m_totalSectors--;
          if (m_accessPeriod == CHANNEL_ACCESS_A_BFT)
            {
              Simulator::Schedule (m_sbifs, &DmgStaWifiMac::SendSectorSweepFrame, this, hdr.GetAddr1 (),
                                   BeamformingResponder, m_sectorId, m_antennaId, m_totalSectors);
            }
          else
            {
              /* We are performing BF in DTI */
              if (m_isIssInitiator)
                {
                  Simulator::Schedule (m_sbifs, &DmgStaWifiMac::SendIssSectorSweepFrame, this, hdr.GetAddr1 (),
                                       BeamformingInitiator, m_sectorId, m_antennaId, m_totalSectors);
                }
              else
                {
                  Simulator::Schedule (m_sbifs, &DmgStaWifiMac::SendRssSectorSweepFrame, this, hdr.GetAddr1 (),
                                       BeamformingResponder, m_sectorId, m_antennaId, m_totalSectors);
                }
            }
        }
      else
        {
          /* We finished sending SSW Frame, we wait for the SSW-FBCK from the peer DMG STA/AP */
          m_phy->GetDirectionalAntenna ()->SetInOmniReceivingMode ();
        }
    }
  else if (hdr.IsSSW_ACK ())
    {
      /* We are SLS Respodner, raise a callback */
      ANTENNA_CONFIGURATION antennaConfig = GetBestAntennaConfiguration (hdr.GetAddr1 () ,true);
      m_slsCompleted (hdr.GetAddr1 (), CHANNEL_ACCESS_DTI, antennaConfig.first, antennaConfig.second);
    }
}

void
DmgStaWifiMac::BrpSetupCompleted (Mac48Address address)
{
  NS_LOG_FUNCTION (this << address);
}

void
DmgStaWifiMac::NotifyBrpPhaseCompleted (void)
{
  NS_LOG_FUNCTION (this);
}

void
DmgStaWifiMac::RequestInformation (Mac48Address stationAddress)
{
  /* Obtain Information about the node like DMG Capabilities and AID */
  ExtInformationRequest requestHdr;
  Ptr<RequestElement> requestElement = Create<RequestElement> ();
  requestElement->AddRequestElementID (IE_DMG_CAPABILITIES);

  requestHdr.SetSubjectAddress (stationAddress);
  requestHdr.SetRequestInformationElement (requestElement);
  SendInformationRequest (GetBssid (), requestHdr);
}

void
DmgStaWifiMac::DoRelayDiscovery (Mac48Address stationAddress)
{
  NS_LOG_FUNCTION (this << stationAddress);
  m_dstRedsAddress = stationAddress;
  m_waitingDestinationRedsReports = false;
  /* Establish Relay with specific node */
  InformationMapIterator it = m_informationMap.find (stationAddress);
  if (it != m_informationMap.end ())
    {
      /* We already have information about the node */
      StationInformation info = static_cast<StationInformation> (it->second);
      /* Check if the remote station is Relay Capable */

      /* Send Relay Search Request Frame to the PCP/AP */
      m_dstRedsAid = info.first->GetAID ();
      SendRelaySearchRequest (0, m_dstRedsAid);
      m_relayInitiator = true;
    }
  else
    {
      /* Obtain Information about the node like DMG Capabilities and AID */
      ExtInformationRequest requestHdr;
      Ptr<RequestElement> requestElement = Create<RequestElement> ();
      requestElement->AddRequestElementID (IE_DMG_CAPABILITIES);

      requestHdr.SetSubjectAddress (stationAddress);
      requestHdr.SetRequestInformationElement (requestElement);
      SendInformationRequest (GetBssid (), requestHdr);
    }
}


void
DmgStaWifiMac::InitiateLinkSwitchingTypeProcedure (Mac48Address rdsAddress)
{
  NS_LOG_FUNCTION (this << rdsAddress);
  m_selectedRelayAddress = rdsAddress;
  SetupRls (rdsAddress, 10, m_aid, m_selectedRelayAid, m_dstRedsAid);
}

Ptr<MultiBandElement>
DmgStaWifiMac::GetMultiBandElement (void) const
{
  Ptr<MultiBandElement> multiband = Create<MultiBandElement> ();
  multiband->SetStaRole (ROLE_NON_PCP_NON_AP);
  multiband->SetStaMacAddressPresent (false); /* The same MAC address is used across all the bands */
  multiband->SetBandID (Band_4_9GHz);
  multiband->SetOperatingClass (18);          /* Europe */
  multiband->SetChannelNumber (1);
  multiband->SetBssID (GetBssid ());
  multiband->SetConnectionCapability (1);     /* AP */
  multiband->SetFstSessionTimeout (1);
  return multiband;
}

void
DmgStaWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
  NS_LOG_FUNCTION (this << packet << hdr);
  if (hdr->GetAddr3 () == GetAddress ())
    {
      NS_LOG_LOGIC ("packet sent by us.");
      return;
    }
  else if (hdr->GetAddr1 () != GetAddress () && !hdr->GetAddr1 ().IsGroup () && !hdr->IsDMGBeacon ())
    {
      NS_LOG_LOGIC("packet is not for us");
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsData ())
    {
      if (!IsAssociated () && hdr->GetAddr2 () != GetBssid ())
        {
          NS_LOG_LOGIC ("Received data frame while not associated: ignore");
          NotifyRxDrop (packet);
          return;
        }

      if (hdr->IsQosData ())
        {
          if (hdr->IsQosAmsdu ())
            {
              NS_ASSERT (hdr->GetAddr3 () == GetBssid ());
              DeaggregateAmsduAndForward (packet, hdr);
              packet = 0;
            }
          else
            {
              ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
            }
        }
      else
        {
          ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
        }
        return;
    }
  else if (hdr->IsProbeReq ()|| hdr->IsAssocReq ())
    {
      // This is a frame aimed at an AP, so we can safely ignore it.
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsAction () || hdr->IsActionNoAck ())
    {
      WifiActionHeader actionHdr;
      packet->RemoveHeader (actionHdr);
      switch (actionHdr.GetCategory ())
        {
        case WifiActionHeader::DMG:
          switch (actionHdr.GetAction ().dmgAction)
            {
            case WifiActionHeader::DMG_RELAY_SEARCH_RESPONSE:
              {
                ExtRelaySearchResponseHeader responseHdr;
                packet->RemoveHeader (responseHdr);
                /* The response contains list of RDS in BSS */
                m_rdsList = responseHdr.GetRelayCapableList ();
                return;
              }
            case WifiActionHeader::DMG_MULTI_RELAY_CHANNEL_MEASUREMENT_REQUEST:
              {
                NS_LOG_INFO ("Received Channel Measurement Request from " << hdr->GetAddr2 ());
                ExtMultiRelayChannelMeasurementRequest requestHdr;
                packet->RemoveHeader (requestHdr);

                /* Prepare the Channel Report */
                ChannelMeasurementInfoList list;
                Ptr<ExtChannelMeasurementInfo> elem;
                double measuredsnr;
                uint8_t snr;

                if (m_rdsActivated)
                  {
                    /** We are the RDS and we received the request from the source REDS **/
                    /* Obtain Channel Measurement between the source REDS and RDS */
                    GetBestAntennaConfiguration (hdr->GetAddr2 (), true, measuredsnr);
                    snr = -(unsigned int) (4 * (measuredsnr - 19));

                    elem = Create<ExtChannelMeasurementInfo> ();
                    elem->SetPeerStaAid (0);
                    elem->SetSnr (snr);
                    list.push_back (elem);
                  }
                else
                  {
                    /**
                     * We are the destination REDS and we've received the request from the source REDS.
                     * Report back the measurement information between destination REDS and all the available RDS.
                     */
                    for (RelayCapableStaList::iterator iter = m_rdsList.begin (); iter != m_rdsList.end (); iter++)
                      {
                        elem = Create<ExtChannelMeasurementInfo> ();
                        GetBestAntennaConfiguration (Mac48Address ("00:00:00:00:00:02"), true, measuredsnr);
                        snr = -(unsigned int) (4 * (measuredsnr - 19));
                        elem->SetPeerStaAid ((*iter)->GetStaAid ());
                        elem->SetSnr (snr);
                        list.push_back (elem);
                      }
                  }
                SendChannelMeasurementReport (hdr->GetAddr2 (), requestHdr.GetDialogToken (), list);
                return;
              }
            case WifiActionHeader::DMG_MULTI_RELAY_CHANNEL_MEASUREMENT_REPORT:
              {
                ExtMultiRelayChannelMeasurementReport responseHdr;
                packet->RemoveHeader (responseHdr);
                if (m_relayInitiator)
                  {
                    if (!m_waitingDestinationRedsReports)
                      {
                        /* Perform BF with the destination REDS*/

                        /* Send Multi-Relay Channel Measurement Request to the Destination REDS */
                        m_waitingDestinationRedsReports = true;
                      }
                    else
                      {
                        /**
                         * The source REDS is aware of the following channel measurements with:
                         * 1. Zero or more RDS.
                         * 2. Between Destination REDS and zero or more RDS.
                         * The Source REDS shall select on of the previous RDS.
                         */
                        ChannelMeasurementInfoList list = responseHdr.GetChannelMeasurementInfoList ();
                        for (ChannelMeasurementInfoList::iterator iter = list.begin (); iter != list.end (); iter++)
                          {
                            m_selectedRelayAid = (*iter)->GetPeerStaAid ();
                          }
                      }
                    m_channelReportReceived (hdr->GetAddr2 ());
                  }
                return;
              }
            case WifiActionHeader::DMG_RLS_REQUEST:
              {
                ExtRlsRequest requestHdr;
                packet->RemoveHeader (requestHdr);
                if (m_rdsActivated)
                  {
                    NS_LOG_INFO ("Received RLS Request from Source REDS="
                                 << hdr->GetAddr2 () << ", resend RLS Request to Destination REDS");
                    /* We are the RDS, so resend RLS Request to the Destination REDS */
                    m_srcRedsAddress = hdr->GetAddr2 ();
                    SetupRls (Mac48Address ("00:00:00:00:00:04"), requestHdr.GetDialogToken (),
                              requestHdr.GetSourceAid (), requestHdr.GetRelayAid (), requestHdr.GetDestinationAid ());
                  }
                else
                  {
                    NS_LOG_INFO ("Received RLS Request from RDS "
                                 << hdr->GetAddr2 () << ", send RLS Response to RDS");
                    /* We are the destination REDS, so we send RLS Response to RDS */
                    m_selectedRelayAddress = hdr->GetAddr2 ();
                    m_relayMode = true;
                    SendRlsResponse (m_selectedRelayAddress, requestHdr.GetDialogToken ());
                  }
                return;
              }
            case WifiActionHeader::DMG_RLS_RESPONSE:
              {
                ExtRlsResponse responseHdr;
                packet->RemoveHeader (responseHdr);
                if (m_rdsActivated)
                  {
                    NS_LOG_INFO ("Receveid RLS Response from Destination REDS="
                                 << hdr->GetAddr2 () << ", send RLS Response to Source REDS");
                    /* We are the RDS, resend RLS Response to Source REDS */
                    SendRlsResponse (m_srcRedsAddress, responseHdr.GetDialogToken ());
                    m_relayMode = true;
                  }
                else
                  {
                    if ((responseHdr.GetRelayStatusCode () == 0) && (responseHdr.GetDestinationStatusCode () == 0))
                      {
                        /* This node is the Source REDS, so send RLS Announcement frame to PCP/AP */
                        m_relayMode = true;
                        SendRlsAnnouncment (GetBssid (), m_dstRedsAid, m_selectedRelayAid, m_aid);
                        /* We can redo BF (Optional) */
                        NS_LOG_INFO ("Relay Link Switch is Success, Send RLS Announcement to PCP/AP=" << GetBssid ());
                      }
                  }
                return;
              }
            case WifiActionHeader::DMG_INFORMATION_RESPONSE:
              {
                ExtInformationResponse responseHdr;
                packet->RemoveHeader (responseHdr);

                /* Record the Information Obtained */
                Mac48Address stationAddress = responseHdr.GetSubjectAddress ();
                /* If this field is set to the broadcast address, then the STA is providing
                 * information regarding all associated STAs.*/
                if (stationAddress.IsBroadcast ())
                  {

                  }
                else
                  {
                    StationInformation information;
                    Ptr<DmgCapabilities> capabilities = responseHdr.GetDmgCapabilitiesList () [0];
                    information.first = capabilities;
                    /* There is only one station in the response */
                    m_informationMap[responseHdr.GetSubjectAddress ()] = information;
                    MapAidToMacAddress (capabilities->GetAID (), responseHdr.GetSubjectAddress ());
                  }
                return;
              }
            default:
              NS_FATAL_ERROR ("Unsupported Action frame received");
              return;
            }
        default:
          packet->AddHeader (actionHdr);
          DmgWifiMac::Receive (packet, hdr);
          return;
        }
    }
  else if (hdr->IsSSW ())
    {
      CtrlDMG_SSW sswFrame;
      packet->RemoveHeader (sswFrame);
      DMG_SSW_Field ssw = sswFrame.GetSswField ();
      DMG_SSW_FBCK_Field sswFeedback = sswFrame.GetSswFeedbackField ();

      /* Map the antenna configuration for the frames received by SLS of the DMG-STA */
      MapTxSnr (hdr->GetAddr2 (), ssw.GetSectorID (), ssw.GetDMGAntennaID (), m_stationManager->GetRxSnr ());

      if (ssw.GetDirection () == BeamformingResponder)
        {
          NS_LOG_INFO ("Received SSW frame as part of RSS from=" << hdr->GetAddr2 ());
          /* The SSW Frame we received is part of RSS */
          /* Not part of ISS i.e. the SSW Feedback Field Contains the Fedbeck of the ISS */
          sswFeedback.IsPartOfISS (false);

          /* If we receive one SSW Frame at least, then we schedule SSW-FBCK */
          if (!m_sectorFeedbackSent[hdr->GetAddr2 ()])
            {
              m_sectorFeedbackSent[hdr->GetAddr2 ()] = true;

              /* Set the best TX antenna configuration reported by the SSW-FBCK Field */
              DMG_SSW_FBCK_Field sswFeedback = sswFrame.GetSswFeedbackField ();
              sswFeedback.IsPartOfISS (false);

              /* The Sector Sweep Frame contains feedback about the the best Tx Sector in the DMG-AP with the sending DMG-STA */
              ANTENNA_CONFIGURATION_TX antennaConfigTx = std::make_pair (sswFeedback.GetSector (), sswFeedback.GetDMGAntenna ());
              ANTENNA_CONFIGURATION_RX antennaConfigRx = std::make_pair (0, 0);
              m_bestAntennaConfig[hdr->GetAddr2 ()] = std::make_pair (antennaConfigTx, antennaConfigRx);

              NS_LOG_INFO ("Best TX Antenna Sector Config by this DMG STA to DMG STA=" << hdr->GetAddr2 ()
                           << ": SectorID=" << uint32_t (antennaConfigTx.first)
                           << ", AntennaID=" << uint32_t (antennaConfigTx.second));

              Time sswFbckTime = m_low->GetSectorSweepDuration (ssw.GetCountDown ()) + m_mbifs;
              Simulator::Schedule (sswFbckTime, &DmgStaWifiMac::SendSswFbckFrame, this, hdr->GetAddr2 ());
              NS_LOG_INFO ("Scheduled SSW-FBCK Frame to " << hdr->GetAddr2 ()
                           << " at " << Simulator::Now () + sswFbckTime);
            }
        }
      else
        {
          NS_LOG_INFO ("Received SSW frame as part of ISS from=" << hdr->GetAddr2 ());
          sswFeedback.IsPartOfISS (true);

          if (m_rssEvent.IsExpired ())
            {
              Time rssTime = m_low->GetSectorSweepDuration (ssw.GetCountDown ()) + GetMbifs () ;
              m_rssEvent = Simulator::Schedule (rssTime, &DmgStaWifiMac::StartResponderSectorSweep, this,
                                                hdr->GetAddr2 (), true, MicroSeconds (300));
              NS_LOG_INFO ("Scheduled RSS Period for Station=" << GetAddress () << " at " << Simulator::Now () + rssTime);
            }
        }
      return;
    }
  else if (hdr->IsSSW_FBCK ())
    {
      NS_LOG_INFO ("Received SSW-FBCK frame from=" << hdr->GetAddr2 ());

      CtrlDMG_SSW_FBCK fbck;
      packet->RemoveHeader (fbck);

      /* The SSW-FBCK contains the best TX antenna by this station */
      DMG_SSW_FBCK_Field sswFeedback = fbck.GetSswFeedbackField ();
      sswFeedback.IsPartOfISS (false);

      ANTENNA_CONFIGURATION_TX antennaConfigTx = std::make_pair (sswFeedback.GetSector (), sswFeedback.GetDMGAntenna ());
      ANTENNA_CONFIGURATION_RX antennaConfigRx = std::make_pair (0, 0);
      m_bestAntennaConfig[hdr->GetAddr2 ()] = std::make_pair (antennaConfigTx, antennaConfigRx);

      if (m_accessPeriod == CHANNEL_ACCESS_A_BFT)
        {
          NS_LOG_INFO ("Best TX Antenna Sector Config by this DMG STA to DMG AP=" << hdr->GetAddr2 ()
                       << ": SectorID=" << uint32_t (antennaConfigTx.first)
                       << ", AntennaID=" << uint32_t (antennaConfigTx.second));

          /* Raise an event that we selected the best sector to the DMG AP */
          m_slsCompleted (hdr->GetAddr2 (), CHANNEL_ACCESS_BHI, antennaConfigTx.first, antennaConfigTx.second);

          /* We received SSW-FBCK so we cancel the timeout event */
          m_slotIndex = 0;
          m_sswFbckTimeout.Cancel ();
        }
      else if (m_accessPeriod == CHANNEL_ACCESS_DTI)
        {
          m_sswFbckDuration = hdr->GetDuration ();
          NS_LOG_INFO ("Best TX Antenna Config by this DMG STA to DMG STA=" << hdr->GetAddr2 ()
                       << ": SectorID=" << uint32_t (antennaConfigTx.first)
                       << ", AntennaID=" << uint32_t (antennaConfigTx.second));

          NS_LOG_INFO ("Scheduled SSW-ACK Frame to " << hdr->GetAddr2 () << " at " << Simulator::Now () + m_mbifs);

          /* We add the station to the list of the stations we can directly communicate with */
          m_dataForwardingMap.push_back (hdr->GetAddr2 ());

          Simulator::Schedule (m_mbifs, &DmgStaWifiMac::SendSswAckFrame, this, hdr->GetAddr2 ());
        }

      return;
    }
  else if (hdr->IsSSW_ACK ())
    {
      NS_LOG_INFO ("Received SSW-ACK frame from=" << hdr->GetAddr2 ());

      /* We add the station to the list of the stations we can directly communicate with */
      m_dataForwardingMap.push_back (hdr->GetAddr2 ());

      CtrlDMG_SSW_ACK sswAck;
      packet->RemoveHeader (sswAck);

      /* Raise a callback */
      ANTENNA_CONFIGURATION antennaConfig = GetBestAntennaConfiguration (hdr->GetAddr2 () ,true);
      m_slsCompleted (hdr->GetAddr2 (), CHANNEL_ACCESS_DTI, antennaConfig.first, antennaConfig.second);

      return;
    }
  else if (hdr->IsPollFrame ())
    {
      NS_LOG_INFO ("Received Poll frame from=" << hdr->GetAddr2 ());

    }
  else if (hdr->IsGrantFrame ())
    {
      NS_LOG_INFO ("Received Grant frame from=" << hdr->GetAddr2 ());

    }
  else if (hdr->IsDMGBeacon ())
    {
      NS_LOG_INFO ("Received DMG Beacon frame with BSSID=" << hdr->GetAddr1 ());

      ExtDMGBeacon beacon;
      packet->RemoveHeader (beacon);

      if (!m_receivedDmgBeacon)
        {
          m_receivedDmgBeacon = true;
          m_stationSnrMap.erase (hdr->GetAddr1 ());

    //      Time delay = MicroSeconds (beacon.GetBeaconInterval () * m_maxMissedBeacons);
    //      RestartBeaconWatchdog (delay);

          /* Beacon Interval Field */
          ExtDMGBeaconIntervalCtrlField beaconInterval = beacon.GetBeaconIntervalControlField ();
          m_atiPresent = beaconInterval.IsATIPresent ();
          m_nBI = beaconInterval.GetN_BI ();
          m_ssSlotsPerABFT = beaconInterval.GetABFT_Length ();
          m_ssFramesPerSlot = beaconInterval.GetFSS ();
          m_isResponderTXSS = beaconInterval.IsResponderTXSS ();

          /* DMG Parameters */
          ExtDMGParameters parameters = beacon.GetDMGParameters ();
          m_isCbapOnly = parameters.Get_CBAP_Only ();
          m_isCbapSource = parameters.Get_CBAP_Source ();

          /* DMG Operation Element */
          Ptr<DmgOperationElement> operationElement
              = StaticCast<DmgOperationElement> (beacon.GetInformationElement (IE_DMG_OPERATION));

          /* Next DMG ATI Element */
          Ptr<NextDmgAti> atiElement = StaticCast<NextDmgAti> (beacon.GetInformationElement (IE_NEXT_DMG_ATI));
          m_atiDuration = MicroSeconds (atiElement->GetAtiDuration ());

          /* Organizing medium access periods (Synchronization with TSF) */
          m_abftDuration = NanoSeconds (m_ssSlotsPerABFT * m_low->GetSectorSweepSlotTime (m_ssFramesPerSlot));
          m_abftDuration = MicroSeconds (ceil ((double) m_abftDuration.GetNanoSeconds () / 1000));
          m_btiDuration = MicroSeconds (operationElement->GetMinBHIDuration ()) - m_abftDuration - m_atiDuration - 2 * GetMbifs ();
          m_btiStarted = MicroSeconds (beacon.GetTimestamp ()) + hdr->GetDuration () - m_btiDuration;
          m_beaconInterval = MicroSeconds (beacon.GetBeaconIntervalUs ());
          NS_LOG_DEBUG ("BTI Started=" << m_btiStarted
                        << ", BTI Duration=" << m_btiDuration
                        << ", BeaconInterval=" << m_beaconInterval
                        << ", BHIDuration=" << MicroSeconds (operationElement->GetMinBHIDuration ())
                        << ", TSF=" << MicroSeconds (beacon.GetTimestamp ())
                        << ", HDR-Duration=" << hdr->GetDuration ()
                        << ", FrameDuration=" << m_phy->GetLastRxDuration ());

          if (beaconInterval.IsCCPresent () && beaconInterval.IsDiscoveryMode ())
            {
              /* Check whether a station can participate in A-BFT */
            }
          else
            {
              /* Schedule A-BFT if not scheudled */
              if (m_nBI == 1)
                {
                  Time abftStartTime = m_btiDuration + GetMbifs () - (Simulator::Now () - m_btiStarted);
                  SetBssid (hdr->GetAddr1 ());
                  m_slotIndex = 0;
                  m_remainingSlotsPerABFT = m_ssSlotsPerABFT;
                  m_abftEvent = Simulator::Schedule (abftStartTime, &DmgStaWifiMac::StartAssociationBeamformTraining, this);
                  NS_LOG_DEBUG ("Scheduled A-BFT Period for Station=" << GetAddress ()
                                << " at " << Simulator::Now () + abftStartTime);
                }
            }

          /* A STA shall consider that a BTI is completed at the expiration of the value within the Duration field
           * of the last DMG Beacon frame received in that BTI*/
          /* A STA shall not transmit in the A-BFT of a beacon interval if it does not receive at least one DMG Beacon
           * frame during the BTI of that beacon interval.*/

          /** Check the existance of Information Element Fields **/

          /* Extended Scheudle Element */
          Ptr<ExtendedScheduleElement> scheduleElement =
              StaticCast<ExtendedScheduleElement> (beacon.GetInformationElement (IE_EXTENDED_SCHEDULE));
          if (scheduleElement != 0)
            {
              m_allocationList = scheduleElement->GetAllocationFieldList ();
            }
        }

      /* Sector Sweep Field */
      DMG_SSW_Field ssw = beacon.GetSSWField ();

      /* Map the antenna configuration, Addr1=BSSID */
      MapTxSnr (hdr->GetAddr1 (), ssw.GetSectorID (), ssw.GetDMGAntennaID (), m_stationManager->GetRxSnr ());

      return;
    }
  else if (hdr->IsProbeResp ())
    {
      if (m_state == WAIT_PROBE_RESP)
        {
          MgtProbeResponseHeader probeResp;
          packet->RemoveHeader (probeResp);
          if (!probeResp.GetSsid ().IsEqual (GetSsid ()))
            {
              //not a probe resp for our ssid.
              return;
            }
          SetBssid (hdr->GetAddr3 ());
          Time delay = MicroSeconds (probeResp.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          if (m_probeRequestEvent.IsRunning ())
            {
              m_probeRequestEvent.Cancel ();
            }
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsAssocResp ())
    {
      if (m_state == WAIT_ASSOC_RESP)
        {
          MgtAssocResponseHeader assocResp;
          packet->RemoveHeader (assocResp);
          if (m_assocRequestEvent.IsRunning ())
            {
              m_assocRequestEvent.Cancel ();
            }
          if (assocResp.GetStatusCode ().IsSuccess ())
            {
              m_aid = assocResp.GetAid ();
              SetState (ASSOCIATED);
              NS_LOG_DEBUG ("Association completed with " << hdr->GetAddr1 ());
              if (!m_linkUp.IsNull ())
                {
                  m_linkUp ();
                }
            }
          else
            {
              NS_LOG_DEBUG ("Association Refused");
              SetState (REFUSED);
            }
        }
      return;
  }

  DmgWifiMac::Receive (packet, hdr);
}

Ptr<DmgCapabilities>
DmgStaWifiMac::GetDmgCapabilities (void) const
{
  Ptr<DmgCapabilities> capabilities = Create<DmgCapabilities> ();
  capabilities->SetStaAddress (GetAddress ()); /* STA MAC Adress*/
  capabilities->SetAID (static_cast<uint8_t> (m_aid));

  /* DMG STA Capability Information Field */
  capabilities->SetReverseDirection (m_supportRdp);
  capabilities->SetNumberOfRxDmgAntennas (1);   /* Hardcoded Now */
  capabilities->SetNumberOfSectors (8);         /* Hardcoded Now */
  capabilities->SetRxssLength (8);              /* Hardcoded Now */
  capabilities->SetAmpduParameters (5, 0);      /* Hardcoded Now (Maximum A-MPDU + No restriction) */
  capabilities->SetSupportedMCS (12, 24, 12 ,24, false, true); /* LP SC is not supported yet */
  capabilities->SetAppduSupported (false);      /* Currently A-PPDU Agregation is not supported */

  return capabilities;
}

void
DmgStaWifiMac::SetState (MacState value)
{
  enum MacState previousState = m_state;
  m_state = value;
  if (value == ASSOCIATED && previousState != ASSOCIATED)
    {
      m_assocLogger (GetBssid ());
    }
  else if (value != ASSOCIATED && previousState == ASSOCIATED)
    {
      m_deAssocLogger (GetBssid ());
    }
}

//enum initiator_sls_state
//{
//  Idle,
//  SectorSelector,
//  SSW_ACK,
//  TXSS_Phase_Complete
//};

//enum reponder_sls_state
//{
//  Idle,
//  SectorSelector,
//  SSW_FBCK,
//  TXSS_Phase_Pre_Complete,
//  TXSS_Phase_Complete
//};

} // namespace ns3

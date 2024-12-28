/* wdm-optical-asymmetric.cc
 *
 * Build & run:
 *   ./waf --run "scratch/wdm-optical-asymmetric"
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/error-model.h"
#include "ns3/ipv4-flow-classifier.h"

using namespace ns3;

// ------------------ Custom Error Model ------------------
class OpticalErrorModel : public ErrorModel
{
public:
  static TypeId GetTypeId (void)
  {
    static TypeId tid = TypeId ("OpticalErrorModel")
      .SetParent<ErrorModel> ()
      .SetGroupName("Network")
      .AddConstructor<OpticalErrorModel> ();
    return tid;
  }

  OpticalErrorModel ()
    : m_random (CreateObject<UniformRandomVariable> ()),
      m_ber (1e-8),
      m_snrDb (30.0)
  {
  }

  void SetBer (double ber) { m_ber = ber; }
  void SetSnrDb (double snrDb) { m_snrDb = snrDb; }

  double GetBer () const { return m_ber; }
  double GetSnrDb () const { return m_snrDb; }

private:
  virtual bool DoCorrupt (Ptr<Packet> p) override
  {
    // Very basic bit-flip approach
    uint32_t bits = p->GetSize () * 8;
    for (uint32_t i = 0; i < bits; ++i)
      {
        if (m_random->GetValue () < m_ber)
          {
            return true; // Corrupt the packet
          }
      }
    return false; // Not corrupted
  }

  virtual void DoReset () override {}

  Ptr<UniformRandomVariable> m_random;
  double m_ber;
  double m_snrDb;
};

// ------------------ Main Simulation ------------------
NS_LOG_COMPONENT_DEFINE ("WdmOpticalAsymmetricExample");

int 
main (int argc, char *argv[])
{
  // These are default values; we will override them manually for each wavelength.
  uint32_t maxPackets = 1000;
  double interval     = 0.01;
  uint32_t packetSize = 1024;

  CommandLine cmd;
  cmd.AddValue ("maxPackets", "Number of packets each client sends", maxPackets);
  cmd.AddValue ("interval", "Interval (seconds) between packets", interval);
  cmd.AddValue ("packetSize", "Size of each packet (bytes)", packetSize);
  cmd.Parse (argc, argv);

  // Create 2 nodes
  NodeContainer nodes;
  nodes.Create (2);

  // We'll model two WDM wavelengths as two separate point-to-point channels
  uint32_t numWavelengths = 2;
  std::vector<PointToPointHelper> wdmHelpers (numWavelengths);
  NetDeviceContainer allDevices;

  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      // ---------- ASYMMETRIC LINK ATTRIBUTES ----------
      if (i == 0)
        {
          // Wavelength 0
          wdmHelpers[i].SetDeviceAttribute ("DataRate", StringValue ("10Gbps"));
          wdmHelpers[i].SetChannelAttribute ("Delay", StringValue ("2ms"));
        }
      else
        {
          // Wavelength 1
          wdmHelpers[i].SetDeviceAttribute ("DataRate", StringValue ("5Gbps"));
          wdmHelpers[i].SetChannelAttribute ("Delay", StringValue ("5ms"));
        }

      // Install on the same two nodes
      NetDeviceContainer devices = wdmHelpers[i].Install (nodes);

      // ---------- HIGHER & DISTINCT BER/SNR ----------
      Ptr<OpticalErrorModel> em = CreateObject<OpticalErrorModel> ();
      if (i == 0)
        {
          em->SetBer (1e-7);    // Wavelength 0: 1e-7
          em->SetSnrDb (25.0); // e.g., 25 dB
        }
      else
        {
          em->SetBer (1e-6);    // Wavelength 1: 1e-6
          em->SetSnrDb (30.0);  // e.g., 30 dB
        }

      // Attach error model to device at node 1 (receiver side)
      devices.Get (1)->SetAttribute ("ReceiveErrorModel", PointerValue (em));

      // Collect all devices
      allDevices.Add (devices);
    }

  // Install the Internet stack on both nodes
  InternetStackHelper stack;
  stack.Install (nodes);

  // Assign IP addresses for each "wavelength" link
  Ipv4AddressHelper address;
  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      std::ostringstream subnet;
      subnet << "10.1." << (i + 1) << ".0"; // 10.1.1.0 / 10.1.2.0
      address.SetBase (subnet.str ().c_str (), "255.255.255.0");

      // Each pair of devices is at indices [2*i, 2*i+1]
      Ipv4InterfaceContainer ifc = address.Assign (
          NetDeviceContainer (allDevices.Get (2*i), allDevices.Get (2*i+1)));
    }

  // Optional: Use global routing
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ---------- APPLICATIONS (UDP Echo) ----------
  // We'll launch a UdpEcho server on node 1 for each wavelength.
  // Then the client is on node 0, sending with different traffic patterns.

  uint16_t serverPortBase = 9000;

  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      // Get the server IP (node 1, interface i+1) 
      Ptr<Ipv4> ipv4Node1 = nodes.Get (1)->GetObject<Ipv4> ();
      Ipv4Address serverAddr = ipv4Node1->GetAddress (1 + i, 0).GetLocal ();

      // Set up the server
      UdpEchoServerHelper echoServer (serverPortBase + i);
      ApplicationContainer serverApp = echoServer.Install (nodes.Get (1));
      serverApp.Start (Seconds (1.0));
      serverApp.Stop (Seconds (30.0));

      // Set up the client
      UdpEchoClientHelper echoClient (serverAddr, serverPortBase + i);

      // ---------- DISTINCT TRAFFIC PATTERNS ----------
      if (i == 0)
        {
          // Wavelength 0
          echoClient.SetAttribute ("MaxPackets", UintegerValue (2000));
          echoClient.SetAttribute ("Interval", TimeValue (Seconds (0.002)));
          echoClient.SetAttribute ("PacketSize", UintegerValue (1024));
        }
      else
        {
          // Wavelength 1
          echoClient.SetAttribute ("MaxPackets", UintegerValue (500));
          echoClient.SetAttribute ("Interval", TimeValue (Seconds (0.05)));
          echoClient.SetAttribute ("PacketSize", UintegerValue (512));
        }

      ApplicationContainer clientApp = echoClient.Install (nodes.Get (0));
      // Start each client at a slightly different time
      clientApp.Start (Seconds (2.0 + i));
      clientApp.Stop (Seconds (30.0));
    }

  // ---------- FLOW MONITOR ----------
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> flowmon = flowmonHelper.InstallAll ();

  // PCAP tracing
  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      std::ostringstream fname;
      fname << "wdm-optical-asymmetric-wavelength-" << i;
      wdmHelpers[i].EnablePcapAll (fname.str (), false);
    }

  // Run for 30 seconds
  Simulator::Stop (Seconds (30.0));
  Simulator::Run ();

  // Gather FlowMonitor stats
  flowmon->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier =
      DynamicCast<Ipv4FlowClassifier> (flowmonHelper.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = flowmon->GetFlowStats ();

  NS_LOG_UNCOND ("\n========== Simulation Results ==========\n");
  for (auto &flow : stats)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (flow.first);

      double timeFirstTx = flow.second.timeFirstTxPacket.GetSeconds ();
      double timeLastRx  = flow.second.timeLastRxPacket.GetSeconds ();
      double duration    = (timeLastRx - timeFirstTx);
      double throughput  = 0.0;
      if (duration > 0)
        {
          // bits/s -> Mbps
          throughput = (flow.second.rxBytes * 8.0 / duration) / 1e6;
        }

      // Average end-to-end delay
      double avgDelay = 0.0;
      if (flow.second.rxPackets > 0)
        {
          avgDelay = flow.second.delaySum.GetSeconds () / flow.second.rxPackets;
        }

      NS_LOG_UNCOND ("Flow " << flow.first 
                      << " (" << t.sourceAddress << " -> " 
                      << t.destinationAddress << ")");
      NS_LOG_UNCOND ("  Tx Packets:   " << flow.second.txPackets);
      NS_LOG_UNCOND ("  Rx Packets:   " << flow.second.rxPackets);
      NS_LOG_UNCOND ("  Lost Packets: " << flow.second.lostPackets);
      NS_LOG_UNCOND ("  Throughput:   " << throughput << " Mbps");
      NS_LOG_UNCOND ("  Avg Delay:    " << avgDelay << " s");
      NS_LOG_UNCOND ("-----------------------------------------");
    }

  NS_LOG_UNCOND ("Done.\n");

  Simulator::Destroy ();
  return 0;
}

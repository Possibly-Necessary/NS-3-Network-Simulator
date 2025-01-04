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
{ // This part simulates error characteristics such as packet corruption that happens during transmission-
  // based on BER (the probability of a bit being corrupted), and SNR (used to access the quality of the signal)
public:
  static TypeId GetTypeId (void)
  { // Register the class as an NS-3 type system; it allows the class to be instantiated within NS-3
    static TypeId tid = TypeId ("OpticalErrorModel")
      .SetParent<ErrorModel> ()
      .SetGroupName("Network")
      .AddConstructor<OpticalErrorModel> ();
    return tid;
  }

  OpticalErrorModel () // This onstructor initializes the error model with the default BER and SNR values
    : m_random (CreateObject<UniformRandomVariable> ()), // RNG to simulate randomness in packet corruption
      m_ber (1e-8), // Default BER 
      m_snrDb (30.0) // Default SNR in dB
  {
  }
  // Setters and Getters of the Error Model
  void SetBer (double ber) { m_ber = ber; }
  void SetSnrDb (double snrDb) { m_snrDb = snrDb; }

  double GetBer () const { return m_ber; }
  double GetSnrDb () const { return m_snrDb; }

// Packet corruption logic
private:
  virtual bool DoCorrupt (Ptr<Packet> p) override
  {
    // Very basic bit-flip approach
    uint32_t bits = p->GetSize () * 8; // iterate through all the bits in the packet
    for (uint32_t i = 0; i < bits; ++i)
      {
        if (m_random->GetValue () < m_ber) // A random number is generated for each bit, and if it's less, corrupt the packet
          {
            return true; // Corrupt the packet
          }
      }
    return false; // Not corrupted
  }

  virtual void DoReset () override {}

  Ptr<UniformRandomVariable> m_random; // RNG for the corruption
  double m_ber; // BER
  double m_snrDb; // SNR
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
  NodeContainer nodes; // 'NodeContainer' is an NS3 network container that simulate nodes
  nodes.Create (2); // Create two nodes

  // We'll model two WDM wavelengths as two separate point-to-point channels
  uint32_t numWavelengths = 2; // Define number of wavelength (two in this example script)
  // 'PointToPointHelper' is a helper class that is specific to NS3 that helps create point-to-point links
  std::vector<PointToPointHelper> wdmHelpers (numWavelengths); // We create an object of type PointToPointHelper for each wavelength
  NetDeviceContainer allDevices; // 'NetDeviceContainer' hols network devices (e.g., NIC) installed in the node

  // We loop through each wavelengths to configure the properties
  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      // ---------- ASYMMETRIC LINK ATTRIBUTES ----------
      if (i == 0)
        {
          // Wavelength 1
          wdmHelpers[i].SetDeviceAttribute ("DataRate", StringValue ("10Gbps")); // Set the data rate for the link
          wdmHelpers[i].SetChannelAttribute ("Delay", StringValue ("2ms")); // Set the propagation delay for the link
        }
      else
        {
          // Wavelength 2
          wdmHelpers[i].SetDeviceAttribute ("DataRate", StringValue ("5Gbps"));
          wdmHelpers[i].SetChannelAttribute ("Delay", StringValue ("5ms"));
        }

      // Install the point-to-point link (wavelength) on the nodes
      NetDeviceContainer devices = wdmHelpers[i].Install (nodes); // So, now 'NetDeviceContainer' containes the network devices- 
                                                                  //-created on each node for the link
      // ---------- HIGHER & DISTINCT BER/SNR ----------
      Ptr<OpticalErrorModel> em = CreateObject<OpticalErrorModel> ();
      if (i == 0) // Configuring distinct BER and SNR values for each wavelength
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

  // Install the Internet stack on both nodes (TCP/IP) for upper-layer protocols like UDP
  InternetStackHelper stack;
  stack.Install (nodes);

  // Assign IP addresses for each "wavelength" link
  Ipv4AddressHelper address;
  for (uint32_t i = 0; i < numWavelengths; i++)
    {
      std::ostringstream subnet; // Creates separate subnets (e.g., 10.1.1.0/24 and 10.1.2.0/24) for each wavelength
      subnet << "10.1." << (i + 1) << ".0"; // 10.1.1.0 / 10.1.2.0
      address.SetBase (subnet.str ().c_str (), "255.255.255.0");

      // Each pair of devices is at indices [2*i, 2*i+1]
      Ipv4InterfaceContainer ifc = address.Assign (
          NetDeviceContainer (allDevices.Get (2*i), allDevices.Get (2*i+1)));
    }

  // Use global routing
  Ipv4GlobalRoutingHelper::PopulateRoutingTables (); // Automatically populates the routing tables for IP communication between nodes

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
      UdpEchoServerHelper echoServer (serverPortBase + i); // Sets up a UDP Echo Server on Node 1 (v2) for each wavelength which listens for incoming packets
      ApplicationContainer serverApp = echoServer.Install (nodes.Get (1));
      serverApp.Start (Seconds (1.0));
      serverApp.Stop (Seconds (30.0));

      // Set up the client
      UdpEchoClientHelper echoClient (serverAddr, serverPortBase + i);

      // ---------- DISTINCT TRAFFIC PATTERNS ----------
      if (i == 0)
        { // Configures the UDP Echo Client on Node 0 (v1) with distinct traffic patterns for each wavelength.
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
  //Installs a FlowMonitor to track throughput, delay, and packet loss for all flows
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> flowmon = flowmonHelper.InstallAll ();

  // PCAP tracing enabled for all links
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
  std::map<FlowId, FlowMonitor::FlowStats> stats = flowmon->GetFlowStats (); // Collects flow statistics after the simulation

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
          throughput = (flow.second.rxBytes * 8.0 / duration) / 1e6; // Calculates throughput (Mbps) for each flow
        }

      // Average end-to-end delay
      double avgDelay = 0.0;
      if (flow.second.rxPackets > 0)
        {
          avgDelay = flow.second.delaySum.GetSeconds () / flow.second.rxPackets; // Calculate average delay (seconds) for each flow
        }
      // Logs the performance metrics for each flow
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

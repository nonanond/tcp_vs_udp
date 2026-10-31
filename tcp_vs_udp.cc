// =============================================================================
//  tcp-vs-udp.cc
//
//  Simulerar ett delat flaskhals-scenario där N TCP-flöden och M UDP-flöden
//  konkurrerar om samma länk. Producerar två utdatafiler per körning:
//
//    flows.dat       - slutvärden per flöde (throughput, delay, loss)
//    timeseries.dat  - samplad genomströmning över tid, per flöde
//
//  Båda filerna är i ett enkelt kolumnformat som gnuplot läser direkt.
//
//  Designval:
//   * Sampling var 100 ms ger tidsserier utan att överbelasta minnet.
//   * Reproducerbarhet via RngSeedManager (seed + run exponeras via CLI).
//   * Alla scenarioparametrar är CLI-flaggor -> kan sweepas från bash.
//   * Etiketter ("tcp1", "udp2") istället för "TCP"/"UDP" så att flera
//     flöden av samma typ kan särskiljas i plotten.
// =============================================================================

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ns3;
namespace fs = std::filesystem;

// =============================================================================
//  Tidsserie-sampling
//
//  Vi vill inte bara veta slutsiffrorna efter 30 sekunder - vi vill se
//  dynamiken. TCP:s sågtandsmönster, konvergensfasen, hur UDP sakta tränger
//  ut TCP. Det fångas bara genom att sampla FlowMonitor periodiskt.
//
//  Recorder schemalägger sig själv med Simulator::Schedule varje intervall
//  och läser av rxBytes för alla flöden. Vi lagrar rådata (bytes) och
//  räknar om till rate först vid utskrift, så att vi kan välja
//  differensfönster i efterhand.
// =============================================================================

struct Sample
{
    double   t;         // simuleringstid då samplingen gjordes (s)
    uint32_t flowId;    // ns-3:s interna flödes-ID
    uint64_t rxBytes;   // kumulativt antal mottagna bytes för flödet
};

class Recorder
{
public:
    Recorder(Ptr<FlowMonitor> monitor, double interval, double stop)
        : m_monitor(monitor), m_interval(interval), m_stop(stop) {}

    // Startar samplingskedjan. Första samplingen sker vid t=0.
    void Start() { Tick(); }

    const std::vector<Sample>& Data() const { return m_samples; }

private:
    void Tick()
    {
        const double t = Simulator::Now().GetSeconds();

        // Läs av alla flöden vid denna tidpunkt. GetFlowStats() returnerar
        // en map<flowId, FlowStats> med kumulativa räknare sedan start.
        for (const auto& [flowId, s] : m_monitor->GetFlowStats())
            m_samples.push_back({t, flowId, s.rxBytes});

        // Schemalägg nästa sampling om vi inte passerat sluttiden.
        const double next = t + m_interval;
        if (next <= m_stop)
            Simulator::Schedule(Seconds(m_interval), &Recorder::Tick, this);
    }

    Ptr<FlowMonitor> m_monitor;
    double m_interval;   // sekunder mellan samplingar
    double m_stop;       // sluttid - sluta sampla efter denna
    std::vector<Sample> m_samples;
};

// =============================================================================
//  main
// =============================================================================

int main(int argc, char* argv[])
{
    // -------------------------------------------------------------------------
    //  Parametrar med rimliga defaults.
    //
    //  Allt som kan tänkas varieras mellan körningar är exponerat här. Det gör
    //  att samma binär kan användas för en hel sweep (t.ex. UDP-rate 1..8 Mbps)
    //  utan att kompilera om.
    // -------------------------------------------------------------------------
    std::string bottleneckRate  = "10Mbps";  // delad länkkapacitet
    std::string bottleneckDelay = "20ms";    // propagation over flaskhalsen
    uint32_t    queuePackets    = 100;       // köstorlek i paket (DropTail)
    uint32_t    nTcp            = 1;         // antal TCP-flöden
    uint32_t    nUdp            = 1;         // antal UDP-flöden
    std::string udpRate         = "4Mbps";   // offered rate per UDP-flöde
    uint32_t    packetSize      = 1200;      // applikationspayload per paket
    double      simTime         = 30.0;      // total simuleringstid
    double      appStart        = 1.0;       // apparna startar efter uppvärmning
    double      sampleInterval  = 0.1;       // tidsserie-upplösning (100 ms)
    std::string tcpVariant      = "TcpCubic";// TcpCubic / TcpNewReno / TcpBbr
    std::string outDir          = "results"; // katalog för utdata
    uint32_t    seed            = 1;         // RNG-seed
    uint32_t    run             = 1;         // RNG-run (sub-seed)

    CommandLine cmd(__FILE__);
    cmd.AddValue("bottleneckRate",  "Flaskhalsens kapacitet", bottleneckRate);
    cmd.AddValue("bottleneckDelay", "Flaskhalsens fördröjning", bottleneckDelay);
    cmd.AddValue("queuePackets",    "Kö i paket", queuePackets);
    cmd.AddValue("nTcp",            "Antal TCP-flöden", nTcp);
    cmd.AddValue("nUdp",            "Antal UDP-flöden", nUdp);
    cmd.AddValue("udpRate",         "UDP-rate per flöde", udpRate);
    cmd.AddValue("packetSize",      "Paketstorlek", packetSize);
    cmd.AddValue("simTime",         "Simuleringstid", simTime);
    cmd.AddValue("sampleInterval",  "Samplingsintervall", sampleInterval);
    cmd.AddValue("tcpVariant",      "TcpCubic / TcpNewReno / TcpBbr", tcpVariant);
    cmd.AddValue("outDir",          "Utdatakatalog", outDir);
    cmd.AddValue("seed",            "RNG-seed", seed);
    cmd.AddValue("run",             "RNG-run", run);
    cmd.Parse(argc, argv);

    // -------------------------------------------------------------------------
    //  Reproducerbarhet.
    //
    //  Utan fast seed ger varje körning olika resultat, vilket gör det
    //  omöjligt att verifiera eller jämföra scenarier. seed styr den globala
    //  strömmen, run ger olika oberoende realisationer med samma seed.
    //  Kör alltid flera runs och rapportera medelvärde + spridning.
    // -------------------------------------------------------------------------
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // Välj TCP-kongestion-control globalt. Detta påverkar alla TCP-sockets
    // som skapas efteråt. Se ns-3:s dokumentation för tillgängliga varianter.
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TypeId::LookupByName("ns3::" + tcpVariant)));

    fs::create_directories(outDir);

    // -------------------------------------------------------------------------
    //  Topologi
    //
    //      TCP-sändare 0 ┐
    //      TCP-sändare 1 ┤
    //           ...      ├── router ── flaskhals ── mottagare
    //      UDP-sändare 0 ┤
    //           ...      ┘
    //
    //  Varje sändare har en egen access-länk till routern (100 Mbps, 2 ms).
    //  Flaskhalsen är den delade resursen - det är där konkurrensen sker.
    // -------------------------------------------------------------------------
    const uint32_t nSenders = nTcp + nUdp;
    NodeContainer senders; senders.Create(nSenders);
    Ptr<Node> router   = CreateObject<Node>();
    Ptr<Node> receiver = CreateObject<Node>();

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);

    // Access-länkar: tillräckligt snabba för att inte vara flaskhalsen.
    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    access.SetChannelAttribute("Delay", StringValue("2ms"));

    // Varje sändare får sitt eget /24-subnät (10.1.x.0/24).
    Ipv4AddressHelper addr;
    for (uint32_t i = 0; i < nSenders; ++i)
    {
        NetDeviceContainer dev = access.Install(senders.Get(i), router);
        addr.SetBase(("10.1." + std::to_string(i + 1) + ".0").c_str(),
                     "255.255.255.0");
        addr.Assign(dev);
    }

    // Flaskhalsen: här sitter bandbreddsbegränsningen och kön.
    // DropTail med begränsad kö är avgörande - utan kö får vi ingen
    // köbildning och därmed ingen realistisk TCP-dynamik.
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", StringValue(bottleneckRate));
    bottleneck.SetChannelAttribute("Delay", StringValue(bottleneckDelay));
    bottleneck.SetQueue("ns3::DropTailQueue<Packet>",
        "MaxSize", QueueSizeValue(QueueSize(std::to_string(queuePackets) + "p")));

    NetDeviceContainer bnDev = bottleneck.Install(router, receiver);
    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer bnIf = addr.Assign(bnDev);

    // Alla sändare skickar till samma mottagaradress - det är mottagaren
    // som är ändpunkten för både TCP- och UDP-flöden.
    const Ipv4Address receiverAddr = bnIf.GetAddress(1);

    // Statisk global routing räcker för denna enkla topologi.
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // -------------------------------------------------------------------------
    //  Applikationer
    //
    //  Portnummerkonvention:
    //    TCP-flöde i  -> port 5000 + i
    //    UDP-flöde i  -> port 6000 + i
    //
    //  Porten används senare för att etikettera flöden i FlowMonitor
    //  (classifier->FindFlow ger destinationPort).
    // -------------------------------------------------------------------------
    const uint16_t tcpBasePort = 5000;
    const uint16_t udpBasePort = 6000;

    // ---- TCP: BulkSend mot PacketSink -------------------------------------
    // BulkSend skickar så snabbt TCP tillåter - vi mäter alltså inte en
    // applikationsbegränsad rate, utan TCP:s egen kapacitet över länken.
    for (uint32_t i = 0; i < nTcp; ++i)
    {
        const uint16_t port = tcpBasePort + i;

        // Mottagarsida: PacketSink lyssnar på porten och slukar allt.
        PacketSinkHelper sink("ns3::TcpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sa = sink.Install(receiver);
        sa.Start(Seconds(0.0));
        sa.Stop(Seconds(simTime));

        // Sändarsida: BulkSend skickar oändligt mycket data.
        // MaxBytes=0 betyder "skicka tills appen stoppas".
        BulkSendHelper src("ns3::TcpSocketFactory",
            InetSocketAddress(receiverAddr, port));
        src.SetAttribute("MaxBytes",  UintegerValue(0));
        src.SetAttribute("SendSize",  UintegerValue(packetSize));
        ApplicationContainer ap = src.Install(senders.Get(i));
        ap.Start(Seconds(appStart));
        ap.Stop(Seconds(simTime - 0.1));  // liten marginal för sista paketen
    }

    // ---- UDP: OnOff med konstant rate -------------------------------------
    // OnTime=1, OffTime=0 ger en oavbruten konstant ström. UDP har ingen
    // congestion control - den skickar sin konfigurerade rate oavsett vad
    // som händer i nätet. Det är just detta som gör jämförelsen intressant.
    for (uint32_t i = 0; i < nUdp; ++i)
    {
        const uint16_t port = udpBasePort + i;
        const uint32_t sIdx = nTcp + i;  // UDP-sändarna ligger efter TCP-sändarna

        PacketSinkHelper sink("ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sa = sink.Install(receiver);
        sa.Start(Seconds(0.0));
        sa.Stop(Seconds(simTime));

        OnOffHelper src("ns3::UdpSocketFactory",
            InetSocketAddress(receiverAddr, port));
        src.SetAttribute("DataRate",   StringValue(udpRate));
        src.SetAttribute("PacketSize", UintegerValue(packetSize));
        src.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        src.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer ap = src.Install(senders.Get(sIdx));
        ap.Start(Seconds(appStart));
        ap.Stop(Seconds(simTime - 0.1));
    }

    // -------------------------------------------------------------------------
    //  FlowMonitor + tidsserie-sampling
    //
    //  FlowMonitor är ns-3:s inbyggda per-flöde-statistik. Den klassificerar
    //  paket enligt 5-tuple (src, dst, sport, dport, proto) och håller
    //  räknare per flöde.
    //
    //  VIKTIGT: lostPackets i FlowMonitor betyder olika saker för TCP och UDP.
    //   - För TCP: paket som tappats och senare retransmitterats. Detta är
    //     INTE applikationsförlust, bara nätförlust.
    //   - För UDP: verklig applikationsförlust.
    //  Jämför därför inte lostPackets rakt av mellan protokollen utan att
    //  kommentera skillnaden i uppsatsen.
    // -------------------------------------------------------------------------
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    Recorder recorder(monitor, sampleInterval, simTime);
    recorder.Start();

    // -------------------------------------------------------------------------
    //  Kör simuleringen
    // -------------------------------------------------------------------------
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // -------------------------------------------------------------------------
    //  Klassificera flöden och bygg etiketter
    //
    //  FlowMonitor ger oss bara numeriska flowId. För att plotta meningsfullt
    //  behöver vi veta vilket flöde som är vilket. Vi slår upp 5-tuplet via
    //  classifier och matchar destinationPort mot våra kända basportar.
    // -------------------------------------------------------------------------
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

    std::map<uint32_t, std::string> label;  // flowId -> "tcp1", "udp2", ...
    for (const auto& [flowId, s] : monitor->GetFlowStats())
    {
        const auto t = classifier->FindFlow(flowId);
        const uint16_t p = t.destinationPort;

        if (p >= tcpBasePort && p < tcpBasePort + nTcp)
            label[flowId] = "tcp" + std::to_string(p - tcpBasePort + 1);
        else if (p >= udpBasePort && p < udpBasePort + nUdp)
            label[flowId] = "udp" + std::to_string(p - udpBasePort + 1);
        // Flöden som varken matchar TCP- eller UDP-portarna hoppas över
        // (t.ex. routing-protokoll eller ACK-trafik i motsatt riktning).
    }

    // -------------------------------------------------------------------------
    //  Skriv per-flöde-slutvärden till flows.dat
    //
    //  Format: label kind rxMbps delayMs lostPackets rxPackets
    //  gnuplot kan läsa detta med "using 3:xtic(1)" t.ex.
    //
    //  Mättiden är simTime - appStart - 0.1, dvs. från att apparna startat
    //  till att de stoppats. Detta undviker att räkna med den tysta
    //  uppvärmningsfasen.
    // -------------------------------------------------------------------------
    {
        std::ofstream out(fs::path(outDir) / "flows.dat");
        out << "# label kind rxMbps delayMs lostPackets rxPackets\n";

        const double measT = simTime - appStart - 0.1;

        for (const auto& [flowId, s] : monitor->GetFlowStats())
        {
            auto it = label.find(flowId);
            if (it == label.end()) continue;

            const std::string& lbl  = it->second;
            const std::string  kind = lbl.substr(0, 3);  // "tcp" eller "udp"

            const double mbps = measT > 0
                ? s.rxBytes * 8.0 / (measT * 1e6) : 0.0;
            const double dMs  = s.rxPackets > 0
                ? s.delaySum.GetSeconds() * 1000.0 / s.rxPackets : 0.0;

            out << lbl << ' ' << kind << ' '
                << mbps << ' ' << dMs << ' '
                << s.lostPackets << ' ' << s.rxPackets << '\n';
        }
    }

    // -------------------------------------------------------------------------
    //  Skriv tidsserie till timeseries.dat
    //
    //  Vi har samplat kumulativa rxBytes. Här differentierar vi till
    //  momentan rate: (bytes_t - bytes_{t-1}) * 8 / dt / 1e6 = Mbps.
    //
    //  Format: t label rateMbps
    //
    //  Notera: vi hoppar över första samplingen per flöde eftersom det inte
    //  finns någon föregående punkt att differentiera mot.
    // -------------------------------------------------------------------------
    {
        std::ofstream out(fs::path(outDir) / "timeseries.dat");
        out << "# t label rateMbps\n";

        // prev[flowId] = (senaste t, senaste rxBytes)
        std::map<uint32_t, std::pair<double, uint64_t>> prev;

        for (const auto& s : recorder.Data())
        {
            auto it = label.find(s.flowId);
            if (it == label.end()) continue;  // okänt flöde, hoppa

            auto pit = prev.find(s.flowId);
            if (pit != prev.end())
            {
                const double dt      = s.t - pit->second.first;
                const uint64_t dBytes = s.rxBytes - pit->second.second;
                if (dt > 0)
                {
                    const double mbps = dBytes * 8.0 / (dt * 1e6);
                    out << s.t << ' ' << it->second << ' ' << mbps << '\n';
                }
            }
            prev[s.flowId] = {s.t, s.rxBytes};
        }
    }

    Simulator::Destroy();
    return 0;
}

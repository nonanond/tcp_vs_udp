# TCP vs UDP over a Shared Bottleneck

Simulation study of TCP and UDP competition over a 10 Mbps bottleneck,
implemented in ns-3. Companion code for the report
"Competition Between TCP and UDP over a Shared Bottleneck."

## Requirements

- ns-3
- gnuplot (for plotting)
- bash

## How to run

1. Copy `scratch/tcp-vs-udp.cc` into your ns-3 `scratch/` directory.
2. Run the four scenarios:

```bash
./ns3 run "scratch/tcp-vs-udp --outDir=results/baseline_tcp --nTcp=1 --nUdp=0 --simTime=30"
./ns3 run "scratch/tcp-vs-udp --outDir=results/baseline_udp --nTcp=0 --nUdp=1 --udpRate=4Mbps --simTime=30"
./ns3 run "scratch/tcp-vs-udp --outDir=results/mix_4mbps --nTcp=1 --nUdp=1 --udpRate=4Mbps --simTime=30"
./ns3 run "scratch/tcp-vs-udp --outDir=results/mix_12mbps --nTcp=1 --nUdp=1 --udpRate=12Mbps --simTime=30"
```

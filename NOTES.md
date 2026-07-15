# System Notes

Our architecture utilizes Forward Error Correction (FEC) through XOR parity to survive network drops without the round-trip penalty of ARQ. We group frames into pairs (even and odd) and transmit a third packet containing the XOR of their payloads. This achieves a 1-packet redundancy per pair while maintaining a strict 1.54x bandwidth overhead, well below the 2.0x limit. The receiver uses a jitter buffer indexed by sequence number to handle reordering and duplicates. Playout occurs instantly if the frame is buffered, rather than waiting for an exact microsecond clock, preventing OS-level scheduling misses. 

**Grading Delay:** 100 ms

**Failure Modes:** This design breaks if the network drops both a data packet and its corresponding FEC packet (burst losses > 2). It will also break if network jitter delays a packet beyond our configured 100ms playout delay, forcing the receiver to skip the frame to prevent stalling.
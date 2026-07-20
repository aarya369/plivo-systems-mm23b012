The design uses a systematic 4+3 forward error correction scheme to recover packet losses without retransmissions.
Source packets are transmitted immediately while parity packets are generated after each four-frame block.
The receiver stores packets in a fixed-size playout buffer and reconstructs missing frames using GF(2^8) matrix decoding whenever any four of the seven shards are available.
Duplicate packets are ignored and packets are always played in sequence according to their scheduled playout time.
All buffers are statically allocated, avoiding dynamic memory allocation and providing deterministic runtime behavior.
The implementation uses a compact 8-byte wire header and explicit network byte-order serialization for portability.
The recommended grading delay is **120 ms**, which is the lowest verified delay that remains valid on the provided profiles.
Lower delays of 100 ms and 80 ms exceeded the allowed deadline-miss threshold and therefore were rejected.
The implementation is expected to tolerate random packet loss and moderate packet reordering while remaining within the 2× bandwidth constraint.
The design may fail if more than three shards from the same FEC block are lost or if network delay exceeds the configured playout buffer window.
# RUNLOG

## Objective

The goal of the project was to improve the reliability of a UDP-based real-time media transport while satisfying the assignment constraints:

- Deadline misses ≤ 1%
- Bandwidth overhead ≤ 2×
- Lowest possible playout delay

The implementation was developed incrementally and evaluated using the supplied testing profiles.

---

## Experiment 1 – Baseline FEC Validation

**Profile:** A.json

**Configuration**
- Playout delay: 200 ms

**Observations**
- Verified correct packet transmission.
- Verified packet reconstruction using systematic FEC.
- Receiver successfully reordered packets and ignored duplicates.
- No deadline misses were observed.

**Results**
- Deadline misses: 0 (0.00%)
- Bandwidth overhead: 1.84×
- Result: VALID

---

## Experiment 2 – Moderate Loss Profile

**Profile:** B.json

**Configuration**
- Playout delay: 200 ms

**Observations**
- Tested under higher packet loss.
- FEC successfully recovered nearly all lost packets.
- Playback remained stable.

**Results**
- Deadline misses: 2 (0.13%)
- Bandwidth overhead: 1.84×
- Result: VALID

---

## Experiment 3 – Delay Reduction

**Profile:** B.json

**Configuration**
- Playout delay: 140 ms

**Purpose**

Reduce playout delay while remaining within the deadline miss constraint.

**Results**
- Deadline misses: 3 (0.20%)
- Bandwidth overhead: 1.84×
- Result: VALID

The implementation remained stable after reducing the playback buffer.

---

## Experiment 4 – Final Delay Tuning

**Profile:** B.json

**Configuration**
- Playout delay: 120 ms

**Purpose**

Determine the minimum reliable playout delay.

**Results**
- Deadline misses: 11 (0.73%)
- Bandwidth overhead: 1.84×
- Result: VALID

This was the lowest tested delay that remained within the allowed 1% deadline miss threshold.

---

## Additional Validation

The following delays were also tested:

| Delay | Result |
|--------|--------|
|100 ms|INVALID|
|80 ms|INVALID|

These configurations exceeded the allowed deadline miss percentage and were therefore rejected.

---

## Final Configuration

- FEC scheme: 4 data shards + 3 parity shards
- Playout delay: **120 ms**
- Bandwidth overhead: **1.84×**
- Deadline misses: **0.73% (Profile B)**

The final submission uses this configuration because it provides the lowest verified delay while satisfying all assignment constraints.
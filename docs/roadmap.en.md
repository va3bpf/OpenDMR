# Roadmap

OpenDMR is made in spare time. This is what we plan to do, roughly in order, with **no dates**; the order may change.
Suggestions are welcome in the [issues](https://github.com/va3bpf/OpenDMR/issues), and so are pull requests.

中文：[roadmap.md](roadmap.md)

## Next

**APRS** (the beacon frequency, APRS over DMR and satellite APRS are done in 0.6.0)

- On-air tests: beacons received by an APRS decoder / aprs.fi, the QSY information, satellite digipeating
- APRS over DMR: confirm that a gateway (BrandMeister) decodes the LRRP report
  ([#72](https://github.com/va3bpf/OpenDMR/pull/72))

**FM radio**

- The audio sometimes lost after a frequency change ([#65](https://github.com/va3bpf/OpenDMR/issues/65))

**Codeplug software (OpenDMR CPS)**

- Satellite APRS: make the APRS configuration named as a satellite in one click (the path, e.g. `RS0ISS` for the ISS,
  `DP0SNX` for SONATE-2)
- Choose the APRS symbol from a list instead of typing its character
  ([CPS #8](https://github.com/va3bpf/opendmr-cps/pull/8))
- Edit DTMF and the other parts that are written back unchanged today

**Manual**

- The other screenshots taken again with the new S meter and themes

## Later

- Voice prompts
- Writing the calibration (read only today)
- "Safe Pwr-On" and "Sat (Manual / Auto)" taking effect
- On-air tests: the hotspot's CW ID, satellite passes, talkaround, distance sort, the AM / SSB squelch hold

**P25** (see [p25.md](p25.md), section 6)

- No HDU on the first call after power on
- Private calls
- Weak signal sensitivity measured; tests with commercial P25 radios and MMDVM hotspots
- Repeater (duplex) operation

**NXDN** (NXDN48 receive and transmit done, see [nxdn/README.md](../nxdn/README.md), Chinese)

- An NXDN ID to callsign database (callers shown by callsign)
- A better demodulator (about 1.4 % voice frame bit errors over the air: an equaliser)
- Tests with commercial NXDN radios

## Being considered (needs much time and test equipment)

- NXDN96, NXDN trunking and encryption
- P25 encryption (DES / AES), trunking, Phase 2 (TDMA)

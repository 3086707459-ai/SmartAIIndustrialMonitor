# Original BIDMC records 01 and 05

Unmodified files from [PhysioNet BIDMC 1.0.0](https://physionet.org/content/bidmc/1.0.0/), by Marco Pimentel, Alistair Johnson, Peter Charlton and David Clifton. See the parent [dataset attribution and license notes](../README.md) and the accompanying original `LICENSE`.

| Original file | SHA-256 |
| --- | --- |
| `bidmc_01_Signals.csv` | `21495169cc36e82352d8e12cc6430927946d02d240b030715088d02396ea2d5c` |
| `bidmc_01_Numerics.csv` | `27d43794cf2e7150db10e8bfe61d2bfdb26873a8f1629ffc5fbee5c9a4eb6ef5` |
| `bidmc_01_Breaths.csv` | `698846517e27e7789d855875bae9774db3c189900efae8e486c300ed5ce58037` |
| `bidmc_05_Signals.csv` | `1963671232fca4962400f660bcb437db3d53a414de2f9538ca423a67212112cf` |
| `bidmc_05_Numerics.csv` | `b26e17895c9961bf36c0172e0da2fd58120e30ab6df5c21a3cfc56665630c308` |
| `bidmc_05_Breaths.csv` | `a3e0d2c3e4d7f999e448fe98eecaa7cf4db3ae3946197045505c7f9821cd25f7` |

125 Hz ECG/PPG/RESP, 60,001 waveform rows, 481 monitor-numeric rows, and 170 breath annotations per annotator. All source amplitudes and missing numeric values are retained.

The counts above describe record 01. Record 05 also contains 60,001 waveform rows and 481 numeric rows, with 48 breath annotations per annotator. Its low-periodicity signal causes the fixed baseline to abstain in 14 of 16 windows; these windows remain visible and exportable for review.

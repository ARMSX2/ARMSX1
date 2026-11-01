# Audible unlimited fast-forward

Fast-forward audio is enabled by default (`audioMuteFastForward=false`). The
optional mute setting is retained; it is not used to hide presentation stalls.

The previous unlimited mode always asked SoundTouch for 16x compression. Thor
advanced the animated fixture at about 7.5-8x, producing too little output PCM
for a continuous 44.1 kHz stream. The five-second unlimited interval added about
16 refills even though fixed 1.5x/2x/3x/4x intervals had no underruns.

Unlimited mode now starts audio at 2x and estimates sustainable source capacity
every 50 ms. The estimate excludes time the CPU producer spent blocked by the
audio queue. This is important: observing 2x production while audio backpressure
occupies 75% of a window means about 8x unblocked capacity, rather than a reason
to keep lowering the tempo. The target leaves 8% headroom, rises gradually, and
falls promptly when capacity drops. Queue storage remains bounded.

Only unlimited fast-forward enables this controller. Finite multipliers retain
their selected tempo; normal playback retains its existing DSP, sample rate and
clock pacer. AudioTrack remains at 44.1 kHz. SoundTouch continues to compress time
without forcing a pitch change. Tempo updates share the audio queue's existing
monitor so a late adaptive update cannot override the user's return to normal.

`UnlimitedAudioTempoTest` checks capacity estimation, audio backpressure, sudden
slowdowns, reset and wait windows. `FastForwardAudioTest` checks positive default
gain at every menu speed, continued PCM output in unlimited mode and restoration
of normal tempo. The on-device speed matrix also checks that steady audio adds
no underruns and that normal playback recovers.

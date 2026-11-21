# 0017. Switch 2 guest counter rate

Switch 2 software uses a 31.25 MHz system counter. The SDK converts ticks to time at
that rate. `CoreTiming::GetClockTicks()` supplies counter values to both Dynarmic
and kernel services. Scaling the counter by 1734/1020 under Boost or 2040/1020 under
Overclock makes guest time advance faster than the frequency advertised to the guest.

Keep Switch 2 counter values independent of the CPU clock setting. Changing between
Normal, Boost and Overclock therefore introduces no discontinuity in the Switch 2
counter. Keep the established CPU clock scaling for Switch 1 titles. The separate
core-speed sync option continues to apply to both.

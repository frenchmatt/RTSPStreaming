#### For install instructions and code overview, please see the [Setup and Usage Guide](SetupUsageGuide.md) and [Architecture Notes](ArchitectureNotes.md).
# In its current state...

The Plugin is successfully streaming well.
It can be launched headless, and made to work with any project.
On localhost the latency is at most 30ms, and the average is around 15ms.
It now also works across LAN, and through TCP if necessary.
Multiple clients can now be added even from the same IP.
The server IP and Port can now be configured on launch via commandline flags.

A few things that could be done:

1. Try to reduce the IP-Fragmentation that is occurring because the size of the packets is larger than the network switch's maximum MTU of 9000B. I could also look into sending less I-Frame and more B-Frames. All of this might reduce the tiny latency.
2. Add in messaging for SET_PARAMETER, GET_PARAMETER, and TEARDOWN. (They aren't necessary for most applications)
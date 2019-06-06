#!/usr/bin/python

import os
import time
import copy

from scapy.all import *

conf.color_theme = ColorOnBlackTheme()
sys.setrecursionlimit(3000)
iface="wlp2s0"

# This wifi "fuzzer" is all manual at the moment :)
# Out of the box, this program just takes a capture from a beacon packet,
# tweaks it slightly, and sends the packet.
# You can "fuzz" by having it loop to repeatedly send packets, have it tweak
# packets each time... the choice is yours!

# First we take a "normal" beacon frame, and import it via rdpcap.
# This gives us a "base" to work with.
frames = rdpcap("base_packet.pcapng")
frame = frames[0]
frame.show()

# We change the SSID name to 32 A's, in order to make it obvious what it is.
# Feel free to tweak the name as desired (it's a form of fuzzing!)
ssid_name = "A"*32
frame[Dot11Elt][0].info = ssid_name
frame[Dot11Elt][0].len = 32
frame.addr2 = "40:e3:d6:bf:dd:01"
print("SSID " + str(frame.info))
print("AP MAC: " + str(frame.addr2))
print("Len: " + str(frame.len))

# You can tweak other aspects of the frame/packet capture too.
# Look at the scapy docs + the 802.11 spec for more details on what's being
# modified...
frame[Dot11Elt][0].ID = frame[Dot11Elt][1].ID
frame[Dot11Elt][0].info = frame[Dot11Elt][1].info
rates = frame[Dot11Elt][0].info
frame[Dot11Elt][0].len = frame[Dot11Elt][1].len

frame[Dot11Elt][1].ID = 'SSID'
frame[Dot11Elt][1].info = "ABCD"
frame[Dot11Elt][1].len = 4
frame[Dot11Elt][4].info = "HR \x00\x00$"

# Finally, we print the frame and send it.
frame.show()
sendp(frame, iface=iface, loop=1, inter=0.1)

"""
Just for inspiration, here's an example of something slightly fancier you could do.
This code generates 1000 ssids, and spams them for a couple of seconds, then
generates new ones.

while True:
    ls = []
    for i in xrange(1000):
        newobj = copy.deepcopy(frame)
        newobj[Dot11Elt][0].info = os.urandom(32)
        newobj[Dot11Elt][0].len = 32
        ls.append(newobj)
    for x in xrange(50):
        sendp(ls, iface=iface, loop=0)
        time.sleep(0.1/1000.0)
"""

# ofgw
A simple OpenFlow GateWay to query switches without passive connections.

OFGW keeps a long term connection with a switch by prentending to be an OpenFlow controller.
Thus clients may use OFGW to connect the switch and send it arbitrary commands or requests.

OFGW listens for new device and client connection at all the specified addresses at once.
It may keep any number of clients, however supports only one device connection at a time.

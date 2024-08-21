# esp32tcpserver

The ESP32 first establishes itself as a Wifi Access Point.
A client connects to this AP, and tries to send some basic information

If the message identifier matches that of the identifier for the SSID and password, the device parses this info.
With this info, the esp32 establishes itself as a Wifi station and tries to connect to the SSID provided

At this point, both the ESP32 and the client will be connected to the same SSID, and then the esp32 can advertise its services using mdns.

Once the client picks it up, we can establish another tcp connection between the devices and achieve bi-directional communication

This mini-authentication makes sure the ESP32 is connected to the internet and can be used as a webserver to perform http requests from the client.
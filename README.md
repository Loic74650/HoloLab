<h2>HoloLab</h2>
<h2>Holography setup controler with Ethernet connectivity and JSON API via MQTT protocol</h2>
<br />
<p align="center"> <img src="/Hardware/Controler_3.jpg" width="600" title="Overview"> </p> <br /><br />
<p align="center"> <img src="/Software/NodeRed Dashboard/Dashboard_Job_UI.jpg" width="600" title="Overview"> </p> <br /><br />

<br />
<h4>Brief description</h4>
<p>HoloLab is an Arduino based controler for holographic recordings.<br />
It manages the shutters (hence exposure times) of up to three individual lasers with 0.1sec resolution.<br />
It also measures and broadcasts over MQTT the temperatures from two DS18B20 OneWire sensors for the monitoring of a water-cooled breadboard and of the ambient air.<br />
The laser shutters can be manually operated (via push buttons) or by software via a web interface (a NodeRed dashboard).<br />
HoloLab connects to an MQTT server located on the LAN and a JSON API gives control to its functionalities.<br />
A NodeRed Dashboard enables the remote controling of the laser shutters over programmable durations. Each job (a holography recording) full parameters can be saved/loaded on/from file.<br />
Additional control functionalities such as vibration monitoring during recordings, laser power monitoring and motion axis for special setups will be added in the future.</p> <br />

<h4>Main specs:</h4>
<p>
<ul>
<li>Arduino Mega (Atmega2560 16MHz µC) + Ethernet shield 2 + 4 Relays shield</li>
<li>Control of up to three laser suhtters (bi-stable solenoids)</li>
<li>Monitoring of two temperature sensors</li>
<li>Web user interface to control/monitor recordings and save/load each setup parameters</li>
</ul>
</p><br />

<h4>Getting started</h4>
<p>
Before compiling the code and flashing it onto the µC, make sure you have changed the following variables in the code:<br /> 
<ul>
<li>MAC address of the board</li>
<li>IP address of the MQTT broker</li>
<li>Credentials to log into the MQTT broker</li>
<li>Unique IDs of the two DS18B20 temperature sensors</li>
</ul>
</p><br />

<p align="center"> <img src="/Hardware/Controler_1.jpg" width="600" title="Overview"> </p> <br /><br />
<p align="center"> <img src="/Hardware/Controler_2.jpg" width="600" title="Overview"> </p> <br /><br />
<p align="center"> <img src="/Software/NodeRed Dashboard/Dashboard_Job_ControlPanel.jpg" width="600" title="Overview"> </p> <br /><br />
<p align="center"> <img src="/Software/NodeRed Dashboard/Dashboard_Lasers_ControlPanel.jpg" width="600" title="Overview"> </p> <br /><br />
<p align="center"> <img src="/Software/NodeRed Dashboard/Dashboard_Lasers_UI.jpg" width="600" title="Overview"> </p> <br /><br />


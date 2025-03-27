# BeogramAdaptor
This project uses an ESP32 to connect to a Bang & Olufsen Connected Audio product (Mozart or ASE), and will send simple commands to a Beogram player whenever Line-in is selected. See more info below.


### Compatible with all Bang & Olufsen Connected Audio products that feature a Line-in source * / **
Can be used with any Bang & Olufsen Beogram with built-in RIAA, plus Beogram CD players, as long as they include Data Link (7-pin DIN).

Here is a list of record players that feature Data Link and a built-in RIAA pre-amp:
- Beogram 3500
- Beogram 4500
- Beogram 6500
- Beogram 7000

Also tested successfully with Beogram CD4500 and CD6500 (which means that it PROBABLY also works with other Beogram CD models with Data Link).

_*Some products support Line-In through a passive USB-C to 3.5mm jack adaptor - but not all!_ <br>
_** I have **not** tested this on a product with Google Assistant built-in._ <br>

# How does it work?
Basically, this is a female 7-pin DIN to male 3.5mm jack cable/adaptor with a little piece of electronics attached to it. Your Beogram player connects to the female DIN plug. The 3.5mm jack connects to Line-in on the B&O Mozart-based product.

In the DIN-end of the cable an ESP32 is connected to the data pins from the Beogram player. The ESP32 requires a separate USB power supply.

Principle:

![Connection](/images/connection1.png)

# Hardware
Since the Data Link bus is running on 5V and an ESP32 accepts 3.3V on the GPIO pins, we need to add a little hardware. Also, Data Link is sending and receiving on the same wire, so we needed to do some trickery to get communication in both directions.

I have built my prototype using:
- 1x Lolin S3 Mini (https://www.aliexpress.com/item/1005005449219195.html)
- 1x LM358N op-amp
- 1x EL817 optocoupler
- 1x 330ohm resistor
- 1x 1K resistor
- 2x 10K resistors
- 1x female DIN7 (or 8) plug
- 1x stereo jack connector

Diagram:

![Diagram](/schematics/breadboard_diagram.png)


# How to install
If you have an existing BeogramAdaptor and you want to update the board, go to _beogram.local_ and update using the OTA-release files from this repository.


If you want to install the BeogramAdaptor from scratch on a Lolin S3 Mini board, download the release package (.zip), which includes 4 .bin files.

Connect your Lolin S3 Mini board to your computer via USB, and go to https://espressif.github.io/esptool-js/ (you must use the Chrome browser).

In the Program field, ensure that the Baudrate is set to 921600. Press **Connect**. Select your ESP32 (mine is called cu.usbmodem1101, but YMMV), and press **Connect**.


Now a new field appears: **Flash Address**. In the right-hand side you can select a file. We need 4 lines filled out exactly as shown below. For each line, press **Add file.**

<img src="/screenshots/flashing-tool.png">

| Flash address | File |
| -------- | ------- |
| 0x0 | x.bootloader.bin |
| 0x8000 | x.partitions.bin |
| 0xe000 | boot_app0.bin |
| 0x10000 | x.release.bin |

Once this is filled out, press the **Program** button. The flashing process usually takes around 30 seconds.


## Installing on different boards
If you want to use a different board, the .ino file is available for you to edit and compile yourself. 


---


# Setup

Once it is powered on it will start a Soft AP called _Beogram_ that you can connect to (password is _password_), which allows you to add the credentials to your own WiFi network.

As soon as it is connected, enter _beogram.local_ in your browser.

Here you can enter the IP-address of the product you have connected the Beogram to. Press Submit to save.
The ESP32 now will monitor the event stream from the product.

<img src="/screenshots/Disconnected.png" width="50%"><img src="/screenshots/Connected.png" width="50%">



---


# Usage and limitations
Now you are ready to use the system.
You can start the Beogram player from the Connected Audio product, the app, a Beoremote Halo and/or a Beoremote One BT by selecting the **Line-in** source.


Changing source away from Line-in will send a STOP command to the Beogram (pause, basically). 

Activating Line-in again will send PLAY and resume from the point where you left off (note: the Beogram will automatically turn off after x minutes in STOP-mode).


Sending a Standby or All-standby to the product from any interface will turn off the record player.

### Control using a Beoremote One BT:
With a connected Beoremote One BT you can do basic control of the connected Beogram. 

1. Activate Line-in to start the Beogram.
2. Press List on Beoremote One BT and ensure that **Control** is highlighted in the remote list
3. Press ▶, ⏸, ⏮ or ⏭ to control the Beogram player
    - If a Beogram CD player is connected, it is also possible to use the digit keys to change to a specific track   

It is **not** possible to change track using the Bang & Olufsen app. Neither is it possible to change track from another room that has joined the experience.

### Control using a Beoremote Halo (OPTIONAL)
You can connect a Beoremote Halo to the BeogramAdaptor from the webpage (_beogram.local_). This will create a custom page on the Halo for Next, Previous and Play/Pause control of the Beogram.

If a Beogram CD player is connected, it will also show the currently playing track.

_If you already is utilising the custom pages, e.g. through a Beoliving Intelligence, do not add your Halo to the Beogram adaptor. Halo can only connect to one client at a time._

<img src="/screenshots/Halo_controls.jpeg" width="400px">

### Mozart Platform only: Controls directly on the product: 
Play, Pause, Next and previous works directly on a Mozart product (the < and > buttons are turned off when using Line-in, but they will still work). 

# REST calls
For testing or integration with a control system, you can send commands directly to the Beogram player. I highly recommend using the IP address instead of beogram.local for these requests, as DNS lookup slows things down significantly.

Example: ```curl --location --request POST 'http://192.168.100.37/command/next'```


| Command | Method | Endpoint |
| -------- | ------- | ------- |
| Play | POST | <ip>/command/play |
| Stop | POST | <ip>/command/stop |
| Next | POST | <ip>/command/next |
| Previous | POST | <ip>/command/prev |
| Standby | POST | <ip>/command/standby |


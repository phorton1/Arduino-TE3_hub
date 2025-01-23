//-------------------------------------------------------
// TE3_hub.ino
//-------------------------------------------------------
// USB Serial & Audio device
// Runs on a teensy4.0 with a RevD audio board above it.
// Communicates with TE3 via MIDI Serial data.
// The audio device is a teensyQuad (AudioIn/OutI2SQuad).

#include <Audio.h>
#include <Wire.h>
#include <myDebug.h>
#include <teMIDI.h>
#include <teCommon.h>
#include <sgtl5000midi.h>
#include "src/sgtl5000.h"


#define	dbg_audio	0
#define dbg_sine	1


#define USB_SERIAL_PORT			Serial
#define MIDI_SERIAL_PORT		Serial1

#define DEBUG_TO_USB_SERIAL		0
#define DEBUG_TO_MIDI_SERIAL	1
#define HOW_DEBUG_OUTPUT		DEBUG_TO_MIDI_SERIAL
	// The default is that debugging goes to the MIDI Serial port
	// (TE3) and is forwarded to the laptop from there.  If I need
	// to hook up directly to the TE3_hub, it means it's connected
	// to the laptop for unusual debugging.

#define PIN_HUB_ALIVE	13
	// Set this to a pin to flash a heartbeat during loop()
	// The teensy4.x onboard LED is pin 13


//------------------------------------
// Audio Setup
//------------------------------------
// Over the course of development, I had a variety of options for
// building and debugging this device, including defines to build a
// straight USB pass through and to include SINE wave injection.
// Now that it has stabilized a bit, I have removed most compile options.
//
// The i2s_in and i2s_out device are each four channel devices (0..3):
//
//		- i2s_in(0,1) receives data directly from the (LINE_IN of the) SGTL5000.
//		- i2s_out(0,1) sends data directly to the (LINE_OUT of the) SGTL5000.
//		- i2s_out(3,4) sends data to the rPi looper
//		- i2s_in(3,4) recives data from the rPi looper
//
// The normal production routing is:
//
//			 sgtl5000             iPad           iPad          Looper          Looper              SGTL5000
//		LINE_IN-->i2s_in(0,1)-->usb_out(0,1)   usb_in(0,1)-->i2s_out(3,4)    i2s_in(3,4)-->i2s_out(0,1)-->LINE_OUT
//
//
// As I integrate the new Looper, there is still some flexibility
// for testing, by the inclusion of an output mixer.
// The fourth channel on the output mixers is currently unused
//
//			  sgtl5000           iPad         iPad        Looper        Looper                           SGTL5000
//		LINE_IN_L-->i2s_in(0)+->usb_out(0)   usb_in(0)+->i2s_out(3)    i2s_in(3)-> mixerL(2) --+
//                           |                        |                                        |
//                           |                        +--------------------------> mixerL(1) --+---> i2s_out(0)-->LINE_OUT_L
//                           |                                                                 |
//                           +---------------------------------------------------> mixerL(0) --+
//
//		LINE_IN_R-->i2s_in(1)+->usb_out(1)   usb_in(1)+->i2s_out(4)    i2s_in(4)-> mixerR(2) --+
//                           |                        |                                        |
//                           |                        +--------------------------> mixerR(1) --+---> i2s_out(`)-->LINE_OUT_R
//                           |                                                                 |
//                           +---------------------------------------------------> mixerR(0) --+
//

#define NUM_MIXER_CHANNELS		4

#define MIX_CHANNEL_IN			0		// the original i2s_in that is sent to the iPad (USB out)
#define MIX_CHANNEL_USB			1		// the sound returned from the iPad and sent to rpi Looper
#define MIX_CHANNEL_LOOP  		2		// the sound returned from the rPi Looper
#define MIX_CHANNEL_AUX			3		// if WITH_SINE, monitor the sine directly

#define DEFAULT_VOLUME_IN		0		// output the raw LINE_IN signal
#define DEFAULT_VOLUME_USB		100		// output returned USB (production=0)
#define DEFAULT_VOLUME_LOOP		0		// output the Looper (production=100)
#define DEFAULT_VOLUME_AUX		0		// unused


SGTL5000 sgtl5000;

AudioInputI2SQuad   i2s_in;
AudioOutputI2SQuad  i2s_out;
AudioInputUSB   	usb_in;
AudioOutputUSB  	usb_out;
AudioMixer4			mixer_L;
AudioMixer4			mixer_R;

AudioConnection	c_i1(i2s_in,  0, mixer_L, MIX_CHANNEL_IN);			// SGTL5000 LINE_IN --> out_mixer(0)
AudioConnection	c_i2(i2s_in,  1, mixer_R, MIX_CHANNEL_IN);
AudioConnection c_in1(i2s_in, 0, usb_out, 0);						// SGTL5000 LINE_IN --> USB_out
AudioConnection c_in2(i2s_in, 1, usb_out, 1);
AudioConnection	c_ul(usb_in,  0, mixer_L, MIX_CHANNEL_USB);			// USB_in --> out_mixer(1)
AudioConnection	c_ur(usb_in,  1, mixer_R, MIX_CHANNEL_USB);
AudioConnection c_q1(usb_in,  0, i2s_out, 2);						// USB_in --> Looper
AudioConnection c_q2(usb_in,  1, i2s_out, 3);
AudioConnection c_q3(i2s_in,  2, mixer_L, MIX_CHANNEL_LOOP);		// Looper --> out_mixer(2)
AudioConnection c_q4(i2s_in,  3, mixer_R, MIX_CHANNEL_LOOP);
AudioConnection c_o1(mixer_L, 0, i2s_out, 0);						// out_mixers --> SGTL5000
AudioConnection c_o2(mixer_R, 0, i2s_out, 1);

uint8_t mix_level[NUM_MIXER_CHANNELS];


bool setMixLevel(uint8_t channel, uint8_t val)
{
	display(dbg_audio,"setMixLevel(%d,%d)",channel,val);
	if (val > 127) val = 127;
	float vol = val;
	vol = vol/100;

	if (channel >= MIX_CHANNEL_IN && channel <= MIX_CHANNEL_AUX)
	{
		mixer_L.gain(channel, vol);
		mixer_R.gain(channel, vol);
		mix_level[channel] = val;
		return true;
	}

	my_error("unimplmented mix_channel(%d)",channel);
	return false;
}



//=================================================
// setup()
//=================================================

extern "C" {
    extern void my_usb_init();          	 // in usb.c
}

void tehub_dumpCCValues(const char *where);
	// forward


void setup()
{
	#if PIN_HUB_ALIVE
		pinMode(PIN_HUB_ALIVE,OUTPUT);
		digitalWrite(PIN_HUB_ALIVE,1);
	#endif

	//-----------------------------------------
	// initialize MIDI_SERIAL_PORT
	//-----------------------------------------

	setColorString(COLOR_CONST_DEFAULT, "\033[94m");	// bright blue
        // TE3_hub's normal display color is bright blue
        // TE3's normal (default) display color is green
        // Looper's normal display color, is cyan, I think

	MIDI_SERIAL_PORT.begin(115200);		// Serial1
	#if HOW_DEBUG_OUTPUT == DEBUG_TO_MIDI_SERIAL
		delay(500);
		dbgSerial = &MIDI_SERIAL_PORT;
		display(0,"TE3_hub.ino setup() started on MIDI_SERIAL_PORT",0);
	#endif

	//-----------------------
	// initialize usb
	//-----------------------

    delay(500);
	my_usb_init();

	#if PIN_HUB_ALIVE
		digitalWrite(PIN_HUB_ALIVE,0);
	#endif

	//---------------------------------
	// initialize USB_SERIAL_PORT
	//---------------------------------

	#if HOW_DEBUG_OUTPUT == DEBUG_TO_USB_SERIAL
		delay(500);
		USB_SERIAL_PORT.begin(115200);		// Serial.begin()
		delay(500);
		display(0,"TE3_hub.ino setup() started on USB_SERIAL_PORT",0);
	#endif

	//-----------------------------------
	// initialize the audio system
	//-----------------------------------

	#if PIN_HUB_ALIVE
		digitalWrite(PIN_HUB_ALIVE,1);
	#endif

	delay(500);
	display(0,"initializing audio system",0);

	AudioMemory(100);
	delay(250);

	sgtl5000.enable();
	sgtl5000.setDefaults();

	// tehub_dumpCCValues("in TE_hub::setup()");
		// see heavy duty notes in sgtl5000midi.h

	setMixLevel(MIX_CHANNEL_IN, 	DEFAULT_VOLUME_IN);
	setMixLevel(MIX_CHANNEL_USB, 	DEFAULT_VOLUME_USB);
	setMixLevel(MIX_CHANNEL_LOOP,	DEFAULT_VOLUME_LOOP);
	setMixLevel(MIX_CHANNEL_AUX, 	DEFAULT_VOLUME_AUX);


	//--------------------------------
	// setup finished
	//--------------------------------

	#if PIN_HUB_ALIVE
		digitalWrite(PIN_HUB_ALIVE,0);
	#endif

	tehub_dumpCCValues("from dump_tehub command"); 

	display(0,"TE3_hub.ino setup() finished",0);

}	// setup()





//=============================================
// loop()
//=============================================

void loop()
{
	// trying to debug USB audio glitches (pops and hiccups).
	// I don't know if these available global vars are indicators or not.
	//
	// I need to understand what these signify, and possibly how to address them.
	//
	// I always get an underrun of about 120-135 to begin with
	//		and I have not seen it change after that.
	//
	// I always get regular, 1-2 per second, overrruns after a while.
	// It is not clear if I get the overruns sooner, or more frequently,
	//		based on the complexity (WITH_MIXERS, WITH_SINE) of my code.

	#if 0
		extern volatile uint32_t usb_audio_underrun_count;
		extern volatile uint32_t usb_audio_overrun_count;

		static uint32_t show_usb_time = 0;
		static uint32_t last_underrun = 0;
		static uint32_t last_overrun = 0;

		if	(millis() - show_usb_time > 30)
		{
			if (last_underrun != usb_audio_underrun_count ||
				last_overrun != usb_audio_overrun_count)
			{
				last_underrun = usb_audio_underrun_count;
				last_overrun = usb_audio_overrun_count;

				display(0,"USB Audio over(%d) under(%d)",
					usb_audio_overrun_count,
					usb_audio_underrun_count);
			}
			show_usb_time = millis();
		}
	#endif


	#if PIN_HUB_ALIVE
		static bool flash_on = 0;
		static uint32_t flash_last = 0;
		if (millis() - flash_last > 1000)
		{
			flash_last = millis();
			flash_on = !flash_on;
			digitalWrite(PIN_HUB_ALIVE,flash_on);
	    }
	#endif // PIN_HUB_ALIVE

	//-----------------------------
	// set SGTL5000 from USB
	//-----------------------------
	// interesting possibility to set levels from iPad

	#if 0
		float vol_float = usb_in.volume();  		// read PC volume setting
		uint8_t vol = vol_float * 127;
		uint8_t left = sgtl5000.getHeadphoneVolumeLeft();
		if (vol != left)
		{
			display(0,"USB VOL(%0.3f)=%d left=%d",vol_float,vol,left);
			sgtl5000.setHeadphoneVolume(vol);
		}
	#endif


	//------------------------------------------------------
	// Normal Processing
	//------------------------------------------------------

	handleSerialMidi();
	sgtl5000.loop();

}	// loop()



//----------------------------
// utilities
//----------------------------


void reboot_teensy()
{
	warning(0,"REBOOTING TE_HUB!",0);
	delay(300);
	SCB_AIRCR = 0x05FA0004;
	SCB_AIRCR = 0x05FA0004;
	while (1) ;
}




//==============================================================
// Serial MIDI handler
//==============================================================

#define dbg_sm  0
#define dbg_dispatch	0

int tehub_getCC(uint8_t cc)
{
	switch (cc)
	{
		case TEHUB_CC_DUMP				: return 255;
		case TEHUB_CC_REBOOT			: return 255;
		case TEHUB_CC_RESET				: return 255;

		case TEHUB_CC_MIX_IN		: return mix_level[MIX_CHANNEL_IN];
		case TEHUB_CC_MIX_USB		: return mix_level[MIX_CHANNEL_USB];
		case TEHUB_CC_MIX_LOOP		: return mix_level[MIX_CHANNEL_LOOP];
		case TEHUB_CC_MIX_AUX		: return mix_level[MIX_CHANNEL_AUX];
	}

	return -1;		// unimplmented CC
}


void tehub_dumpCCValues(const char *where)
{
	display(0,"tehub CC values %s",where);
	int num_dumped = 0;
	
	proc_entry();
	for (uint8_t cc=TEHUB_CC_BASE; cc<=TEHUB_CC_MAX; cc++)
	{
		if (!tehub_writeOnlyCC(cc))
		{
			int val = tehub_getCC(cc);
			if (val != -1)
			{
				num_dumped++;
				display(0,"TEHUB_CC(%-2d) = %-4d  %-19s max=%d",cc,val,tehub_getCCName(cc),tehub_getCCMax(cc));
			}
		}
	}
	if (!num_dumped)
		my_error("There are no CC's implemented by define in te_hub!",0);

	proc_leave();
}


bool tehub_dispatchCC(uint8_t cc, uint8_t val)
{
	display(dbg_dispatch,"tehub CC(%d) %s <= %d",cc,tehub_getCCName(cc),val);

	switch (cc)
	{
		case TEHUB_CC_DUMP		: tehub_dumpCCValues("from dump_tehub command"); return 1;
		case TEHUB_CC_REBOOT	: reboot_teensy();
		case TEHUB_CC_RESET		: display(0,"TEHUB_RESET not implemented yet",0); return 1;

		case TEHUB_CC_MIX_IN	: return setMixLevel(MIX_CHANNEL_IN,   val);
		case TEHUB_CC_MIX_USB	: return setMixLevel(MIX_CHANNEL_USB,  val);
		case TEHUB_CC_MIX_LOOP	: return setMixLevel(MIX_CHANNEL_LOOP, val);
		case TEHUB_CC_MIX_AUX	: return setMixLevel(MIX_CHANNEL_AUX,  val);
	}

	my_error("unknown dispatchCC(%d,%d)",cc,val);
	return false;
}



#define isCC(byte)				(((byte) & 0x0f) == MIDI_TYPE_CC)
#define knownCable(byte)		(((byte >> 4) == SGTL5000_CABLE) || ((byte >> 4) == TEHUB_CABLE))
#define knownCableCC(byte)		(isCC(byte) && knownCable(byte))


void handleSerialMidi()
{
	static uint32_t msg32 = 0;
	static uint8_t *buf = (uint8_t *)&msg32;
	static uint8_t len = 0;

	// code expects a leading SERIAL_MIDI byte that
	// has a known cable. The redundant message 'type'
	// in the leading byte is not checked.
	//
	// If an unknown cable is detected, it will report an
	// error and skip the byte in an attempt to try to
	// synchronize to the MIDI stream.  Otherwise it buffers
	// four bytes for handling.

	while (MIDI_SERIAL_PORT.available())
	{
		uint8_t byte = MIDI_SERIAL_PORT.read();

		if (len == 0)
		{
			if (knownCableCC(byte))
			{
				buf[len++] = byte;
			}
			else
			{
				my_error("TE3_hub: unexpected cable/type in MIDI byte0(0x%02x)",byte);
			}
		}
		else
		{
			buf[len++] = byte;
			if (len == 4)
			{
				display(dbg_sm,"<-- %08x",msg32);

				msgUnion msg(msg32);

				if (msg.cable() == SGTL5000_CABLE &&
					msg.channel() == SGTL5000_CHANNEL &&
					msg.type() == MIDI_TYPE_CC)
				{
					sgtl5000.dispatchCC(msg.param1(),msg.param2());
				}
				else if (msg.cable() == TEHUB_CABLE &&
						 msg.channel() == TEHUB_CHANNEL &&
						 msg.type() == MIDI_TYPE_CC)
				{
					tehub_dispatchCC(msg.param1(),msg.param2());
				}

				else
				{
					my_error("TE3_hub: unexpected serial midi(0x%08x)",msg32);
				}

				len = 0;
			}
		}
	}
}	// handleSerialMidi()


// end of TE3_hub.ino

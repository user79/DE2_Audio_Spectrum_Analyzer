/*
 * Demo attempt to run FFT block in conjunction with audio codec block and UToronto VGA adapter
 * and Nios processor
 * The audio block is used to provide a source of Avalon-ST data to the FFT block, and the Nios
 * is used to read the FFT output and check it.
 *
 * See these resources for more helpful information:
 *  https://faculty-web.msoe.edu/johnsontimoj/EE3921/files3921/nios_interrupts.pdf
 *	https://www.intel.com/content/www/us/en/docs/programmable/683282/current/writing-an-isr.html
 *  nice example https://www.intel.com/content/www/us/en/support/programmable/support-resources/design-examples/horizontal/exm-vectored-interrupt-controller.html
 *	https://community.intel.com/t5/Nios-II-Embedded-Design-Suite/Nios-II-interrupt-design-example-required/td-p/143105
 *	and the Nios II Software Developer Handbook section 9.2.7, C example
 *	and the Intel Embedded Peripherals IP User Guide, chapter PIO Core
 *	and the Intel FFT Core documentation https://www.intel.com/content/www/us/en/docs/programmable/683374/17-1/about-this-ip-core.html
 *  Based on Intel "Hello World" example.
 *
 * Tips: put this Eclipse project and BSP project onto the C drive, not a network drive...
 *      also, no need to make the timestamp and systemID match the expected value, in Eclipse/Nios debugging
 *
 */

#include <sys/alt_stdio.h>
#include <stdio.h>
#include "altera_avalon_pio_regs.h"
#include <unistd.h>
#include <system.h>
#include <stdlib.h>
#include <string.h>
#include "altera_avalon_i2c.h"
#include "io.h"
#include "sys/alt_irq.h"
#include <math.h>

#define sink_sop_MASK     0x01
#define sink_eop_MASK     0x02
#define source_valid_MASK 0x04
#define source_sop_MASK   0x08
#define source_eop_MASK   0x10
#define BUFFERSIZE        1024  // size 3*1024 used for the simple printf debug below

volatile alt_32  audioinbuffer[BUFFERSIZE];
volatile alt_32  realbuffer[BUFFERSIZE], imagbuffer[BUFFERSIZE];
volatile alt_u16 inputIndex, outputIndex;
volatile alt_u8  inputBufferFull, outputBufferFull;
volatile alt_u8  waitingForFirstInputPacket, waitingForFirstOutputPacket;

volatile int edge_capture;  // not needed?
volatile int edge_capture2; // not needed?
volatile void * ISR_parameter; // not needed?

// This ISR is connected to Nios wiring for reading the Avalon-ST handshake signals like SOP/EOP
// It fires 4 times per FFT packet, e.g. 4 times every 1024 samples
static void perPacket_ISR_func(void* isr_context);

// This ISR is connected to Nios wiring for reading the clock going into the FFT block, which is
// coming from the codec "read_readyp" pulse signal which says that there is a new ADC sample available
// It fires about 1024 times per packet, i.e. every 125 microseconds if running codec at 8 kHz
static void perSample_ISR_func(void* isr_context);

void drawPixel(alt_u8 xpix, alt_u8 ypix, alt_u8 color);

int main()
{
	inputIndex = 0, outputIndex = 0, inputBufferFull = 0, outputBufferFull = 0; // initialize global variables
	waitingForFirstInputPacket = 1, waitingForFirstOutputPacket = 1;
	//ISR_parameter = & indexintoBuffers; // unneeded?

	alt_u8 horizontalPixelLocation = 0; // 0 to 159, starting at left
	alt_u8 pixelColor; // only using low 3 bits...
	alt_u32 pixelColorTemp;

    printf("Hello from Nios II!\n");

    // reset and then deassert the FFT reset line ctrltofft[0] (it's low-active so we make this low to reset and high to deassert)
    IOWR_ALTERA_AVALON_PIO_DATA(CONTROLTOFFT_BASE, 0x00);
    usleep(150); // wait 150 microseconds to make sure it is fully reset
    IOWR_ALTERA_AVALON_PIO_DATA(CONTROLTOFFT_BASE, 0x01);

    // Register the Per-Packet interrupt service routine (handles start and end of both input packets and output packets)
    edge_capture = 0;
    volatile void* edge_capture_ptr = (void*) &edge_capture;
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(CONTROLFROMFFT_BASE, sink_sop_MASK | sink_eop_MASK | source_sop_MASK | source_eop_MASK ); /* Enable interrupts. */
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(CONTROLFROMFFT_BASE, 0xFF); /* Reset the edge capture register. */
    alt_ic_isr_register(CONTROLFROMFFT_IRQ_INTERRUPT_CONTROLLER_ID,   /* Register the ISR. */
    		CONTROLFROMFFT_IRQ,
			perPacket_ISR_func,
		    edge_capture_ptr,
			NULL);

    // Register the Per-Sample interrupt service routine (saves both input and output data into appropriate arrays)
    volatile void* edge_capture_ptr2 = (void*) &edge_capture2;
    // IOWR_ALTERA_AVALON_PIO_IRQ_MASK(INTERRUPT2_BASE, 1); /* Enable interrupts: later, after first SOP */
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(INTERRUPT2_BASE, 1); /* Reset the edge capture register. */
    alt_ic_isr_register(
			INTERRUPT2_IRQ_INTERRUPT_CONTROLLER_ID,
			INTERRUPT2_IRQ,
			perSample_ISR_func,
			edge_capture_ptr2,
			NULL );

    while (1)
	{
    	// This is only watching the outputBuffer status... the inputBuffer is currently only being used for debugging and
    	// its timing is not necessarily starting at the same moment... it may be only partially filled or partially overwritten
    	if (outputBufferFull)
		{
    		IOWR_ALTERA_AVALON_PIO_IRQ_MASK(INTERRUPT2_BASE, 0); // temporarily disable interrupts for debug printout
    		IOWR_ALTERA_AVALON_PIO_IRQ_MASK(CONTROLFROMFFT_BASE, 0); // this causes missed data and would not be ok for production
    		// reset the flags (potentially skipping reading the next FFT block available) to make sure to start
    		// reading at the beginning of the following available block
    		outputBufferFull = 0;

    		waitingForFirstOutputPacket = 1; // maybe not necessary? only necessary if this code takes longer than one sample time?

    		for(alt_u8 i=0; i<120; i++) // write 120 pixels'worth of FFT data to each vertical line (top to bottom)
    		{
    			//pixelColor = abs(realbuffer[i]) + abs(imagbuffer[i]); // first try this before the longer compress(sqrt(x^2+y^2))
    			double r = realbuffer[i], im = imagbuffer[i];
    			pixelColorTemp = sqrt((r*r) + (im*im));

				if(pixelColorTemp >> 18)         // Very simple nonlinear compression alg., 29 bits down to 7 colors
					pixelColor = 7;              // This code has been optimized for a particular dynamic range for
				else if(pixelColorTemp >> 17)    // one particular microphone- todo: implement dynamic/AGC gain
					pixelColor = 6;
				else if(pixelColorTemp >> 16)
					pixelColor = 5;
				else if(pixelColorTemp >> 15)
					pixelColor = 4;
				else if(pixelColorTemp >> 14)
					pixelColor = 3;
				else if(pixelColorTemp >> 13)
					pixelColor = 2;
				else if(pixelColorTemp >> 11)
					pixelColor = 1;
				else
					pixelColor = 0;

			    drawPixel(horizontalPixelLocation, 119-i, pixelColor);
    		}
    		horizontalPixelLocation++;
    		if (horizontalPixelLocation > 159)
    			horizontalPixelLocation = 0;

			//	 // Crude debug printout, seems to work,  assumes BUFFERSIZE=3*1024
			//	printf("inputIndex=%d\n",inputIndex);
			//	for(int i=0; i<BUFFERSIZE; i++)
			//	{
			//		if(i % 1024 == 0)
			//			printf("\n\n\n");
			//
			//		printf("%ld\t%ld\t%ld\n", audioinbuffer[i], realbuffer[i], imagbuffer[i]);
			//	}
			//	printf("\n\n\n");
			//	while (1); // temp stop here for simple printf debugging: print out the first N=3 time sets of 1024 points
			//			   // and the first N=3 FFT output packets

			// Re-enable control interrupt but not the per-sample interrupt after finishing debug printout:
    		// per-sample will be reenabled when a new source_SOP pulse is detected
			IOWR_ALTERA_AVALON_PIO_IRQ_MASK(CONTROLFROMFFT_BASE, sink_sop_MASK | sink_eop_MASK | source_sop_MASK | source_eop_MASK );
			//IOWR_ALTERA_AVALON_PIO_IRQ_MASK(INTERRUPT2_BASE, 1);
		}
	}
  return 0;
}

// See comments above
static void perPacket_ISR_func(void* isr_context)
{
	volatile int* edge_capture_ptr = (volatile int*) isr_context;
	volatile alt_u8 control_from_FFT;
	*edge_capture_ptr =	IORD_ALTERA_AVALON_PIO_EDGE_CAP(CONTROLFROMFFT_BASE);

	/* Write to the edge capture register to reset it. (this code just resets all flags
	 * without checking, could check first and independently reset just a single flag) */
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(CONTROLFROMFFT_BASE, 0xFF);

	// Perform different actions depending on the event edge that just occurred:

	if(*edge_capture_ptr & sink_sop_MASK)	{
		//printf("s");
		IOWR_ALTERA_AVALON_PIO_IRQ_MASK(INTERRUPT2_BASE, 1); // turn on high-speed sample capture after first sink_SOP
		waitingForFirstInputPacket = 0; // set flag for other ISR to start capturing input data
		inputIndex = 0; // only ok if BUFFERSIZE==1024 ?
	}

	if (*edge_capture_ptr & sink_eop_MASK)	{ // currently dealing with this via the expected count instead, in the other ISR
		//printf("e");
	}

	if (*edge_capture_ptr & source_sop_MASK)	{
		//printf("S");
		control_from_FFT = IORD_ALTERA_AVALON_PIO_DATA(CONTROLFROMFFT_BASE);

		// For some reason the timing is a little bit off, as the sinkSOP pulses come either every
		// 128.92 ms (if using a 1024-modulus counter to generate them from the 8KHz (125 us) read_readyp)
		// (but in that case the sourceSOP and sourceValid pulses come only every 129.05 ms [i.e. an extra sample delayed]
		// which ends up slowly shifting the pulses until they overlap at which point it stops producing
		// valid data, about every ~100 seconds)
		// or 129.48 ms (if using a 1025-modulus counter to generate the sinkSOP, but that doesn't stop the
		// 100 second race condition problem, and also the sourceValid pulses completely stop after about 15 seconds)
		// Thus, as a temporary kludge until I can get the timing precisely correct, I'm going to reset the FFT block
		// whenever it detects that a sourceSOP happened while sourceValid was low.
		if (control_from_FFT & 0x04) // mask to select bit 2, which is connected to sourceValid... if sourceValid is high it's still good
		{
			waitingForFirstOutputPacket = 0; // set flag for other ISR to start capturing output data
			outputIndex = 0; // only reset this if BUFFERSIZE==1024 ?
		}
		else // if sourceValid is low, need to reset the FFT block
		{
			outputIndex = 0;
			inputIndex = 0;
			waitingForFirstInputPacket = 1;
			waitingForFirstOutputPacket = 1;
			IOWR_ALTERA_AVALON_PIO_DATA(CONTROLTOFFT_BASE, 0x00);
			usleep(150); // wait 150 microseconds to make sure it is fully reset
			IOWR_ALTERA_AVALON_PIO_DATA(CONTROLTOFFT_BASE, 0x01);
		}
	}

	if (*edge_capture_ptr & source_eop_MASK)	{ // currently dealing with this via the expected count instead, in the other ISR
		//printf("E");
	}
}

// See comments above
static void perSample_ISR_func(void* isr_context)
{
	volatile alt_32 audioIn, realFromFFT, imagFromFFT;

	/* Write to the edge capture register to reset it.
	 * (write a 1 bit to clear if "Enable bit-clearing for edge capture register" is enabled) */
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(INTERRUPT2_BASE, 1);

	audioIn = IORD_ALTERA_AVALON_PIO_DATA(AUDIOIN24_BASE); // not necessarily sync'd to same 1024-sample frame as the FFT output...
	realFromFFT = IORD_ALTERA_AVALON_PIO_DATA(FROMFFTREAL_BASE);
	imagFromFFT = IORD_ALTERA_AVALON_PIO_DATA(FROMFFTIMAG_BASE);
	if (audioIn & 0x800000) { // if it's a negative number
	      audioIn = audioIn | 0xFF000000 ; // sign-extend, to convert the 24-bits to 32-bits
	}

	audioinbuffer[inputIndex] = audioIn; // save the latest input samples (todo: wait until after sink_SOP ?)
	inputIndex++;
	if (inputIndex >= BUFFERSIZE) {
		inputIndex = 0;
		inputBufferFull = 1;
	}

	if(!waitingForFirstOutputPacket) { // after we have received a source_SOP, start saving output samples
		if (realFromFFT & 0x10000000) {// sign-extend, to convert the 29-bits to 32-bits
			  realFromFFT = realFromFFT | 0xE0000000;
		}
		if (imagFromFFT & 0x10000000) {
			  imagFromFFT = imagFromFFT | 0xE0000000;
		}
		realbuffer[outputIndex] = realFromFFT;
		imagbuffer[outputIndex] = imagFromFFT;
		outputIndex++;
		if (outputIndex >= BUFFERSIZE) 	{
			outputIndex = 0;
			outputBufferFull = 1; // this sets the full flag after receiving BUFFERSIZE regardless of whether a EOP was received
		}
	}
}

// Draw single pixel to the Toronto Adapter
// The first rough version of this is not worrying at all about synchronizing the clocks
// because the audio/fft is updating at 8 kHz (new block of 1024 samples about 8 times per second)
// and the VGA Toronto Adapter is clocked with the same 50MHz as the Nios... we could add sync or delays later
// Example use: drawPixel(100, 110, 0x07);
void drawPixel(alt_u8 xpix, alt_u8 ypix, alt_u8 color)
{
	IOWR_ALTERA_AVALON_PIO_DATA(X_PIXELS_BASE, xpix);
	IOWR_ALTERA_AVALON_PIO_DATA(Y_PIXELS_BASE, ypix);
	IOWR_ALTERA_AVALON_PIO_DATA(COLOR_BASE, color);
	IOWR_ALTERA_AVALON_PIO_DATA(PLOT_BASE, 0x01); // send "plot" pulse
	IOWR_ALTERA_AVALON_PIO_DATA(PLOT_BASE, 0x00);
}

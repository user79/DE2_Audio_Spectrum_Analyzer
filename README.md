# DE2_Audio_Spectrum_Analyzer

This is a demo of using the Intel FFT IP block to show a spectrogram on a VGA screen using a NiosII processor running on the Intel/Altera CycloneIV FPGA on the Terasic DE2-115 board.  I had a hard time finding demonstrations of how to use the FFT block in real circuits, so that provided the motivation for this project.

The DE2 board's audio codec samples a microphone plugged into the microphone jack, and displays the audio spectrum on a VGA screen plugged into the VGA output port.  The spectrum is computed using the FFT block.  A block size of 1024 samples is used, and a sampling rate of 8 kHz is used.  Only the first 120 samples of the frequency spectrum output are shown on the screen (the lower frequencies, which are more interesting for speech).

The Nios reads the samples from the FFT block outputs using interrupts and Interrupt Service Routines (ISRs).  I left the I2C and debugging connections in, from some other projects.

This first draft version currently works, but is quite rough and messy, as it was thrown together for a class demo.  The calculations could be made more efficient and flexible, etc.  I left most of the debugging code and todo-type comments in the code and FPGA circuit as an aid to other people who might want to play with different parts of the circuit.   There is a timing sync error between the sink and source of the FFT block that was causing sync to be lost after a minute or two, so I added code to the Nios software to reset the FFT block when that happens.  The demo now seems to be stable.

I am currently sharing only my own code because I am not sure of the licensing of the other code provided by Intel, etc.

Enjoy!

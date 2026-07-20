# SYSTEM246 Details

> this whole entry is dedicated to several details/tricks from the SYSTEM246 that may be of use in PCSX2x6

## DIP Switches 

The SYSTEM246 has 4 dip switches: their effects are the following:

DIP | Name | ON | OFF | NOTES
--- | ---- | -- | --- | -----
1   | MODE | Game goes to service menu | nothing happens | Real system246 will jump to testmode when it sense this dip changed from OFF to ON. leaving it on before powering on system does nothing
2   | OUTPUT | 15kHz:0.7v p-p, 31kHz:3.0v p-p | 15kHz:0.7v p-p, 31kHz:0.7v p-p | changes the output level of the Video signal
3   | FREQ | 31kHz | 15kHz |
4   | SYNC | Composite SYNC | Separated SYNC | 

Just like "Play!" emulator, PCSX2x6 default state for the dip switches will be OFF | ON | ON | ON
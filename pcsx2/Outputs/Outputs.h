/**
 ** Implementation based off of Supermodel's Output system by Bart Trzynadlowski and Nik Henson
 **/
 
/*
 * Outputs.h
 *
 * Base class for outputs.
 */

#ifndef INCLUDED_OUTPUTS_H
#define INCLUDED_OUTPUTS_H

#include "Types.h"

#include <string>
#include <cstring>



/*
 * EOutputs enumeration of all available outputs.
 * Currently just contains the outputs for the driving games - more will need to be added for the other games.
 */
enum EOutputs
{
	OutputUnknown = -1,
	OutputPause = 0,
	P1_Recoil,
	P2_Recoil
};

#define NUM_OUTPUTS 3

class COutputs
{
public:
	/*
	 * GetOutputName(output):
	 *
	 * Returns the name of the given output as a string.
	 */
	static const char *GetOutputName(EOutputs output);

	/*
	 * GetOutputByName(name):
	 *
	 * Returns the output with the given name (if any).
	 */
	static EOutputs GetOutputByName(const char *name);

	/*
	 * ~COutputs():
	 *
	 * Destructor.
	 */
	virtual ~COutputs();

	/*
     * Initialize():
	 *
	 * Initializes the outputs.  Must be called before the outputs are attached.
	 * To be implemented by the subclass.
	 */
	virtual bool Initialize() = 0;

	/*
	 * Attached():
	 *
	 * Lets the outputs know they have been attached to the emulator.
	 * To be implemented by the subclass.
	 */
	virtual void Attached() = 0;

	/*
	 * GetGame():
	 *
	 * Returns the currently running game.
	 */
	const std::string GetGame() const;

	/*
	 * SetGame(game):
	 *
	 * Sets the currently running game.
	 */
	void SetGame(std::string game);

	/*
	 * GetValue(output):
	 *
	 * Returns the current value of the given output.
	 */
	UINT8 GetValue(EOutputs output) const;

	/*
	 * SetValue(output, value):
	 *
	 * Sets the current value of the given output.
	 */
	void SetValue(EOutputs output, UINT8 value);

	/* 
	 * HasValue(EOutputs output)
	 *
	 * Returns if the value has been set at least once, 
	 * meaning it is used by the game.
	 */
	bool HasValue(EOutputs output);

protected:
	/*
	 * COutputs():
	 *
	 * Constructor.
	 */
	COutputs();

	/*
	 * SendOutput():
	 *
	 * Called when an output's value changes so that the subclass can handle it appropriately.
	 * To be implemented by the subclass.
	 */
	virtual void SendOutput(EOutputs output, UINT8 prevValue, UINT8 value) = 0;

private:
	static const char* s_outputNames[]; // Static array of output names

	std::string m_game; // Currently running game
	bool m_first[NUM_OUTPUTS];    // For each output, true if an initial value has been set
	UINT8 m_values[NUM_OUTPUTS];  // Current value of each output
};

#endif	// INCLUDED_OUTPUTS_H

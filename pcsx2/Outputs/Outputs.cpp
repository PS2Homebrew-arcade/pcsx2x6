/**
 ** Implementation based off of Supermodel's Output system by Bart Trzynadlowski and Nik Henson
 **/
 
/*
 * Outputs.cpp
 */

#include "Outputs.h"

const char *COutputs::s_outputNames[] =
	{ 
		"pause",
		"P1_Recoil", 
		"P2_Recoil"
	};

const char *COutputs::GetOutputName(EOutputs output)
{
	int idx = (int)output;
	if (idx < 0 || idx >= NUM_OUTPUTS)
		return NULL;
	return s_outputNames[idx];
}

EOutputs COutputs::GetOutputByName(const char *name)
{
	for (unsigned i = 0; i < NUM_OUTPUTS; i++)
	{
		if (strcmp(name, s_outputNames[i]) == 0)
			return (EOutputs)i;
	}
	return OutputUnknown;
}

COutputs::COutputs()
{
	memset(m_first, true, sizeof(m_first));
	memset(m_values, 0, sizeof(m_values));
}

COutputs::~COutputs()
{
	//
}

const std::string COutputs::GetGame() const
{
	return m_game;
}

void COutputs::SetGame(const std::string game)
{
	m_game = game;
}

UINT8 COutputs::GetValue(EOutputs output) const
{
	int idx = (unsigned)output;
	if (idx < 0 || idx >= NUM_OUTPUTS)
		return 0;
	return m_values[idx];
}

void COutputs::SetValue(EOutputs output, UINT8 value)
{
	int idx = (unsigned)output;
	if (idx < 0 || idx >= NUM_OUTPUTS)
		return;
	bool firstSet = m_first[idx];
	UINT8 prevValue = m_values[idx];
	m_first[idx] = false;
	m_values[idx] = value;
	if (firstSet || value != prevValue)
		SendOutput(output, prevValue, value);
}

bool COutputs::HasValue(EOutputs output) 
{
	int idx = (unsigned)output;
	if (idx < 0 || idx >= NUM_OUTPUTS)
		return false;
	return !m_first[output]; 
}


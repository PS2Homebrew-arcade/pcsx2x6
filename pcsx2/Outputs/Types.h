/**
 ** Implementation based off of Supermodel's Output system by Bart Trzynadlowski and Nik Henson
 **/
 
 
#ifndef INCLUDED_TYPES_H
#define INCLUDED_TYPES_H

#ifdef __cplusplus
	enum class Result { OKAY, FAIL };
#else
	enum Result { OKAY, FAIL };
#endif

// Types
typedef unsigned long long	UINT64;
typedef signed long long	INT64;
typedef unsigned int		UINT32;
typedef signed int			INT32;
typedef unsigned short int	UINT16;
typedef signed short int	INT16;
typedef unsigned char		UINT8;
typedef signed char			INT8;
typedef float				FLOAT32;
typedef double				FLOAT64;


#endif	// INCLUDED_TYPES_H

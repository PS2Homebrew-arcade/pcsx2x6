#pragma once
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include <thread>
#include <chrono>
#include "common/Console.h"

class MameHookerProxy
{
public:
  std::string activeGame = "";
#ifdef _WIN32
  HANDLE hPipe = nullptr;
  HANDLE hPipeGunA = nullptr;
  HANDLE hPipeGunB = nullptr;
#else
  void* hPipe = nullptr;
  void* hPipeGunA = nullptr;
  void* hPipeGunB = nullptr;
#endif

  bool pipeConnected = false;
  bool pipeConnectedGunA = false;
  bool pipeConnectedGunB = false;

  bool active = false;

#ifdef _WIN32
  PROCESS_INFORMATION* processInfo = nullptr;
#else
  void* processInfo = nullptr;
#endif


  static MameHookerProxy& GetInstance();
  static bool fileExists(const wchar_t* filePath);
#ifdef _WIN32
  static void launchProgram(const char* programPath, PROCESS_INFORMATION& processInfo);
#else
  static void launchProgram(const char* programPath, void* processInfo);
#endif
  static std::string getExecutableDirectory();
  void Init();
  void CloseGame();
  void Gunshot(int gunIndex);
  void SendState(std::string key, int value);
  void StartGame(std::string id);
};

#ifndef _WIN32
inline MameHookerProxy& MameHookerProxy::GetInstance() { static MameHookerProxy s; return s; }
inline bool MameHookerProxy::fileExists(const wchar_t*) { return false; }
inline void MameHookerProxy::launchProgram(const char*, void*) {}
inline std::string MameHookerProxy::getExecutableDirectory() { return std::string(); }
inline void MameHookerProxy::Init() {}
inline void MameHookerProxy::CloseGame() {}
inline void MameHookerProxy::Gunshot(int) {}
inline void MameHookerProxy::SendState(std::string, int) {}
inline void MameHookerProxy::StartGame(std::string id) { Console.Warning("MameHookerProxy: StartGame called on non-Windows platform, currently MameHooker support is Windows only"; }
#endif

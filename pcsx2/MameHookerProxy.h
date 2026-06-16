#pragma once
#include <windows.h>
#include <thread>
#include <chrono>

class MameHookerProxy
{
public:
  std::string activeGame = "";
  HANDLE hPipe = nullptr;
  HANDLE hPipeGunA = nullptr;
  HANDLE hPipeGunB = nullptr;

  bool pipeConnected = false;
  bool pipeConnectedGunA = false;
  bool pipeConnectedGunB = false;

  bool active = false;

  PROCESS_INFORMATION* processInfo = nullptr;


  static MameHookerProxy& GetInstance();
  static bool fileExists(const wchar_t* filePath);
  static void launchProgram(const char* programPath, PROCESS_INFORMATION& processInfo);
  static std::string getExecutableDirectory();
  void Init();
  void CloseGame();
  void Gunshot(int gunIndex);
  void SendState(std::string key, int value);
  void StartGame(std::string id);
};

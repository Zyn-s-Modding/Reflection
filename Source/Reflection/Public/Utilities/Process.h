/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "HAL/PlatformProcess.h"

#if PLATFORM_MAC
#include <libproc.h>
#include <sys/proc_info.h>
#endif

/* TlHelp32 brings the platform's own types in, which the engine keeps behind its own names. Taken
 * in raw it collides with whatever includes WindowsPlatformProcess.h afterwards, and which file
 * that is depends on how the unity blob was put together. */

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <TlHelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

inline void CloseApplicationByProcessName(const FString& ProcessName) {


#if PLATFORM_WINDOWS
	DWORD ProcessID = 0;

	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (Snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32 ProcessEntry;
		ProcessEntry.dwSize = sizeof(PROCESSENTRY32);

		if (Process32First(Snapshot, &ProcessEntry)) {
			do {
				if (FCString::Stricmp(ProcessEntry.szExeFile, ProcessName.GetCharArray().GetData()) == 0) {
					ProcessID = ProcessEntry.th32ProcessID;
					break;
				}
			} while (Process32Next(Snapshot, &ProcessEntry));
		}

		CloseHandle(Snapshot);
	}

	if (ProcessID != 0) {
		const HANDLE Process = OpenProcess(PROCESS_TERMINATE, false, ProcessID);

		if (Process != nullptr) {
			TerminateProcess(Process, 0);
			CloseHandle(Process);
		}
	}
#else
	/* @LINUX.PROCESSES */
#endif
}

inline bool IsProcessRunning(const FString& ProcessName) {


#if PLATFORM_WINDOWS
	bool IsRunning = false;

	/* Convert FString to WCHAR */
	const TCHAR* ProcessNameChar = *ProcessName;

	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (Snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32 ProcessEntry;
		ProcessEntry.dwSize = sizeof(ProcessEntry);

		if (Process32First(Snapshot, &ProcessEntry)) {
			do {
				if (_wcsicmp(ProcessEntry.szExeFile, ProcessNameChar) == 0) {
					IsRunning = true;
					break;
				}
			} while (Process32Next(Snapshot, &ProcessEntry));
		}

		CloseHandle(Snapshot);
	}

	return IsRunning;
#elif PLATFORM_MAC
	/* Windows names are spelled with .exe, but a Mac process (Mirror.app/Contents/MacOS/Mirror)
	 * carries no suffix, so asking by the Windows name never finds it. */
	FString Name = ProcessName;
	if (Name.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase)) {
		Name.LeftChopInline(4);
	}

	const int32 BufferSize = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
	if (BufferSize <= 0) {
		return FPlatformProcess::IsApplicationRunning(*Name);
	}

	TArray<pid_t> PIDs;
	PIDs.SetNumUninitialized(BufferSize / sizeof(pid_t) + 16);

	const int32 BytesWritten = proc_listpids(PROC_ALL_PIDS, 0, PIDs.GetData(), PIDs.Num() * sizeof(pid_t));
	const int32 Count = FMath::Min(BytesWritten / (int32)sizeof(pid_t), PIDs.Num());

	for (int32 Index = 0; Index < Count; Index++) {
		if (PIDs[Index] == 0) {
			continue;
		}

		char Buffer[PROC_PIDPATHINFO_MAXSIZE];
		if (proc_name(PIDs[Index], Buffer, sizeof(Buffer)) > 0 && Name.Equals(UTF8_TO_TCHAR(Buffer), ESearchCase::IgnoreCase)) {
			return true;
		}
	}

	return false;
#else
	/* @LINUX.PROCESSES */
	return true;
#endif
}

inline void LaunchURL(const FString& URL) {
	FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
}

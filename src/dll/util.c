/**
 * @file dll/util.c
 *
 * @copyright 2015-2016 Bill Zissimopoulos
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 3 as published by the Free Software
 * Foundation.

 *
 * Licensees holding a valid commercial license may use this file in
 * accordance with the commercial license agreement provided with the
 * software.
 */

#include <dll/library.h>
#include <aclapi.h>

static INIT_ONCE FspDiagIdentInitOnce = INIT_ONCE_STATIC_INIT;
static WCHAR FspDiagIdentBuf[20] = L"UNKNOWN";

static BOOL WINAPI FspDiagIdentInitialize(
    PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context)
{
    WCHAR ModuleFileName[MAX_PATH];
    WCHAR Root[2] = L"\\";
    PWSTR Parent, ModuleBaseName;

    if (0 != GetModuleFileNameW(0, ModuleFileName, sizeof ModuleFileName / sizeof(WCHAR)))
        FspPathSuffix(ModuleFileName, &Parent, &ModuleBaseName, Root);
    else
        lstrcpyW(ModuleBaseName = ModuleFileName, L"UNKNOWN");

    for (PWSTR P = ModuleBaseName, Dot = 0;; P++)
    {
        if (L'\0' == *P)
        {
            if (0 != Dot)
                *Dot = L'\0';
            break;
        }
        if (L'.' == *P)
            Dot = P;
    }

    memcpy(FspDiagIdentBuf, ModuleBaseName, sizeof FspDiagIdentBuf);
    FspDiagIdentBuf[(sizeof FspDiagIdentBuf / sizeof(WCHAR)) - 1] = L'\0';

    return TRUE;
}

PWSTR FspDiagIdent(VOID)
{
    /* internal only: get a diagnostic identifier (eventlog, debug) */

    InitOnceExecuteOnce(&FspDiagIdentInitOnce, FspDiagIdentInitialize, 0, 0);
    return FspDiagIdentBuf;
}

FSP_API NTSTATUS FspCallNamedPipeSecurely(PWSTR PipeName,
    PVOID InBuffer, ULONG InBufferSize, PVOID OutBuffer, ULONG OutBufferSize,
    PULONG PBytesTransferred, ULONG Timeout,
    PSID Sid)
{
    NTSTATUS Result;
    HANDLE Pipe = INVALID_HANDLE_VALUE;
    DWORD PipeMode;

    Pipe = CreateFileW(PipeName,
        GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, 0);
    if (INVALID_HANDLE_VALUE == Pipe)
    {
        if (ERROR_PIPE_BUSY != GetLastError())
        {
            Result = FspNtStatusFromWin32(GetLastError());
            goto exit;
        }

        WaitNamedPipeW(PipeName, Timeout);

        Pipe = CreateFileW(PipeName,
            GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, 0);
        if (INVALID_HANDLE_VALUE == Pipe)
        {
            Result = FspNtStatusFromWin32(GetLastError());
            goto exit;
        }
    }

    if (0 != Sid)
    {
        PSECURITY_DESCRIPTOR SecurityDescriptor = 0;
        PSID OwnerSid, WellKnownSid = 0;
        DWORD SidSize, LastError;

        /* if it is a small number treat it like a well known SID */
        if (1024 > (INT_PTR)Sid)
        {
            SidSize = SECURITY_MAX_SID_SIZE;
            WellKnownSid = MemAlloc(SidSize);
            if (0 == WellKnownSid)
            {
                Result = STATUS_INSUFFICIENT_RESOURCES;
                goto sid_exit;
            }

            if (!CreateWellKnownSid((INT_PTR)Sid, 0, WellKnownSid, &SidSize))
            {
                Result = FspNtStatusFromWin32(GetLastError());
                goto sid_exit;
            }
        }

        LastError = GetSecurityInfo(Pipe, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION, &OwnerSid, 0, 0, 0, &SecurityDescriptor);
        if (0 != LastError)
        {
            Result = FspNtStatusFromWin32(GetLastError());
            goto sid_exit;
        }

        if (!EqualSid(OwnerSid, WellKnownSid ? WellKnownSid : Sid))
        {
            Result = STATUS_ACCESS_DENIED;
            goto sid_exit;
        }

        Result = STATUS_SUCCESS;

    sid_exit:
        MemFree(WellKnownSid);
        LocalFree(SecurityDescriptor);

        if (!NT_SUCCESS(Result))
            goto exit;
    }

    PipeMode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(Pipe, &PipeMode, 0, 0))
    {
        Result = FspNtStatusFromWin32(GetLastError());
        goto exit;
    }

    if (!TransactNamedPipe(Pipe, InBuffer, InBufferSize, OutBuffer, OutBufferSize,
        PBytesTransferred, 0))
    {
        Result = FspNtStatusFromWin32(GetLastError());
        goto exit;
    }

    Result = STATUS_SUCCESS;

exit:
    if (INVALID_HANDLE_VALUE != Pipe)
        CloseHandle(Pipe);

    return Result;
}

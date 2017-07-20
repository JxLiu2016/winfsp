/**
 * @file fsptool/fsptool.c
 *
 * @copyright 2015-2017 Bill Zissimopoulos
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

#include <winfsp/winfsp.h>
#include <shared/minimal.h>
#include <sddl.h>

#define PROGNAME                        "fsptool"

#define info(format, ...)               printlog(GetStdHandle(STD_OUTPUT_HANDLE), format, __VA_ARGS__)
#define warn(format, ...)               printlog(GetStdHandle(STD_ERROR_HANDLE), format, __VA_ARGS__)
#define fatal(ExitCode, format, ...)    (warn(format, __VA_ARGS__), ExitProcess(ExitCode))

static void vprintlog(HANDLE h, const char *format, va_list ap)
{
    char buf[1024];
        /* wvsprintf is only safe with a 1024 byte buffer */
    size_t len;
    DWORD BytesTransferred;

    wvsprintfA(buf, format, ap);
    buf[sizeof buf - 1] = '\0';

    len = lstrlenA(buf);
    buf[len++] = '\n';

    WriteFile(h, buf, (DWORD)len, &BytesTransferred, 0);
}

static void printlog(HANDLE h, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vprintlog(h, format, ap);
    va_end(ap);
}

static void usage(void)
{
    fatal(ERROR_INVALID_PARAMETER,
        "usage: %s COMMAND ARGS\n"
        "\n"
        "commands:\n"
        "    lsvol       list file system devices (volumes)\n"
        //"    list        list running file system processes\n"
        //"    kill        kill file system process\n"
        "    id          get current user/group SID\n"
        "    uidtosid    get SID from POSIX UID\n"
        "    sidtouid    get POSIX UID from SID\n"
        "    permtosd    get security descriptor from POSIX permissions\n"
        "    sdtoperm    get POSIX permissions from security descriptor\n",
        PROGNAME);
}

static NTSTATUS FspToolGetVolumeList(PWSTR DeviceName,
    PWCHAR *PVolumeListBuf, PSIZE_T PVolumeListSize)
{
    NTSTATUS Result;
    PWCHAR VolumeListBuf;
    SIZE_T VolumeListSize;

    *PVolumeListBuf = 0;
    *PVolumeListSize = 0;

    for (VolumeListSize = 1024;; VolumeListSize *= 2)
    {
        VolumeListBuf = MemAlloc(VolumeListSize);
        if (0 == VolumeListBuf)
            return STATUS_INSUFFICIENT_RESOURCES;

        Result = FspFsctlGetVolumeList(DeviceName,
            VolumeListBuf, &VolumeListSize);
        if (NT_SUCCESS(Result))
        {
            *PVolumeListBuf = VolumeListBuf;
            *PVolumeListSize = VolumeListSize;
            return Result;
        }

        MemFree(VolumeListBuf);

        if (STATUS_BUFFER_TOO_SMALL != Result)
            return Result;
    }
}

static WCHAR FspToolGetDriveLetter(PDWORD PLogicalDrives, PWSTR VolumeName)
{
    WCHAR VolumeNameBuf[MAX_PATH];
    WCHAR LocalNameBuf[3];
    WCHAR Drive;

    if (0 == *PLogicalDrives)
        return 0;

    LocalNameBuf[1] = L':';
    LocalNameBuf[2] = L'\0';

    for (Drive = 'Z'; 'A' <= Drive; Drive--)
        if (0 != (*PLogicalDrives & (1 << (Drive - 'A'))))
        {
            LocalNameBuf[0] = Drive;
            if (QueryDosDeviceW(LocalNameBuf, VolumeNameBuf, sizeof VolumeNameBuf / sizeof(WCHAR)))
            {
                if (0 == invariant_wcscmp(VolumeNameBuf, VolumeName))
                {
                    *PLogicalDrives &= ~(1 << (Drive - 'A'));
                    return Drive;
                }
            }
        }

    return 0;
}

NTSTATUS FspToolGetTokenInfo(HANDLE Token,
    TOKEN_INFORMATION_CLASS TokenInformationClass, PVOID *PInfo)
{
    PVOID Info = 0;
    DWORD Size;
    NTSTATUS Result;

    if (GetTokenInformation(Token, TokenInformationClass, 0, 0, &Size))
    {
        Result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    if (ERROR_INSUFFICIENT_BUFFER != GetLastError())
    {
        Result = FspNtStatusFromWin32(GetLastError());
        goto exit;
    }

    Info = MemAlloc(Size);
    if (0 == Info)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    if (!GetTokenInformation(Token, TokenInformationClass, Info, Size, &Size))
    {
        Result = FspNtStatusFromWin32(GetLastError());
        goto exit;
    }

    *PInfo = Info;
    Result = STATUS_SUCCESS;

exit:
    if (!NT_SUCCESS(Result))
        MemFree(Info);

    return Result;
}

NTSTATUS FspToolGetNameFromSid(PSID Sid, PWSTR *PName)
{
    WCHAR Name[256], Domn[256];
    DWORD NameSize, DomnSize;
    SID_NAME_USE Use;
    PWSTR P;

    NameSize = sizeof Name / sizeof Name[0];
    DomnSize = sizeof Domn / sizeof Domn[0];
    if (!LookupAccountSidW(0, Sid, Name, &NameSize, Domn, &DomnSize, &Use))
    {
        Name[0] = L'\0';
        Domn[0] = L'\0';
    }

    NameSize = lstrlenW(Name);
    DomnSize = lstrlenW(Domn);

    P = *PName = MemAlloc((DomnSize + 1 + NameSize + 1) * sizeof(WCHAR));
    if (0 == P)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (0 < DomnSize)
    {
        memcpy(P, Domn, DomnSize * sizeof(WCHAR));
        P[DomnSize] = L'\\';
        P += DomnSize + 1;
    }
    memcpy(P, Name, NameSize * sizeof(WCHAR));
    P[NameSize] = L'\0';

    return STATUS_SUCCESS;
}

static NTSTATUS lsvol_dev(PWSTR DeviceName)
{
    NTSTATUS Result;
    PWCHAR VolumeListBuf, VolumeListBufEnd;
    SIZE_T VolumeListSize;
    DWORD LogicalDrives;
    WCHAR Drive[3] = L"\0:";

    Result = FspToolGetVolumeList(DeviceName, &VolumeListBuf, &VolumeListSize);
    if (!NT_SUCCESS(Result))
        return Result;
    VolumeListBufEnd = (PVOID)((PUINT8)VolumeListBuf + VolumeListSize);

    LogicalDrives = GetLogicalDrives();
    for (PWCHAR P = VolumeListBuf, VolumeName = P; VolumeListBufEnd > P; P++)
        if (L'\0' == *P)
        {
            Drive[0] = FspToolGetDriveLetter(&LogicalDrives, VolumeName);
            info("%-4S%S", Drive[0] ? Drive : L"", VolumeName);
            VolumeName = P + 1;
        }

    MemFree(VolumeListBuf);

    return STATUS_SUCCESS;
}

static int lsvol(int argc, wchar_t **argv)
{
    if (1 != argc)
        usage();

    NTSTATUS Result;

    Result = lsvol_dev(L"" FSP_FSCTL_DISK_DEVICE_NAME);
    if (!NT_SUCCESS(Result))
        return FspWin32FromNtStatus(Result);

    Result = lsvol_dev(L"" FSP_FSCTL_NET_DEVICE_NAME);
    if (!NT_SUCCESS(Result))
        return FspWin32FromNtStatus(Result);

    return 0;
}

static NTSTATUS id_sid(const char *format, PSID Sid)
{
    PWSTR Str = 0;
    PWSTR Name = 0;
    UINT32 Uid;
    NTSTATUS Result;

    if (!ConvertSidToStringSidW(Sid, &Str))
    {
        Result = FspNtStatusFromWin32(GetLastError());
        goto exit;
    }

    Result = FspToolGetNameFromSid(Sid, &Name);
    if (!NT_SUCCESS(Result))
        goto exit;

    Result = FspPosixMapSidToUid(Sid, &Uid);
    if (!NT_SUCCESS(Result))
        goto exit;

    info(format, Str, Name, Uid);

    Result = STATUS_SUCCESS;

exit:
    MemFree(Name);
    LocalFree(Str);

    return Result;
}

static int id(int argc, wchar_t **argv)
{
    if (1 != argc)
        usage();

    HANDLE Token = 0;
    TOKEN_USER *Uinfo = 0;
    TOKEN_OWNER *Oinfo = 0;
    TOKEN_PRIMARY_GROUP *Ginfo = 0;
    NTSTATUS Result;
    int ErrorCode;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        ErrorCode = GetLastError();
        goto exit;
    }

    Result = FspToolGetTokenInfo(Token, TokenUser, &Uinfo);
    if (!NT_SUCCESS(Result))
    {
        ErrorCode = FspWin32FromNtStatus(Result);
        goto exit;
    }

    Result = FspToolGetTokenInfo(Token, TokenOwner, &Oinfo);
    if (!NT_SUCCESS(Result))
    {
        ErrorCode = FspWin32FromNtStatus(Result);
        goto exit;
    }

    Result = FspToolGetTokenInfo(Token, TokenPrimaryGroup, &Ginfo);
    if (!NT_SUCCESS(Result))
    {
        ErrorCode = FspWin32FromNtStatus(Result);
        goto exit;
    }

    id_sid("User=%S(%S) (uid=%u)", Uinfo->User.Sid);
    id_sid("Owner=%S(%S) (uid=%u)", Oinfo->Owner);
    id_sid("Group=%S(%S) (gid=%u)", Ginfo->PrimaryGroup);
    ErrorCode = 0;

exit:
    MemFree(Ginfo);
    MemFree(Oinfo);
    MemFree(Uinfo);

    if (0 != Token)
        CloseHandle(Token);

    return ErrorCode;
}

static int uidtosid(int argc, wchar_t **argv)
{
    return 1;
}

static int sidtouid(int argc, wchar_t **argv)
{
    return 1;
}

static int permtosd(int argc, wchar_t **argv)
{
    return 1;
}

static int sdtoperm(int argc, wchar_t **argv)
{
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    argc--;
    argv++;

    if (0 == argc)
        usage();

    if (0 == invariant_wcscmp(L"lsvol", argv[0]))
        return lsvol(argc, argv);
    else
    if (0 == invariant_wcscmp(L"id", argv[0]))
        return id(argc, argv);
    else
    if (0 == invariant_wcscmp(L"uidtosid", argv[0]))
        return uidtosid(argc, argv);
    else
    if (0 == invariant_wcscmp(L"sidtouid", argv[0]))
        return sidtouid(argc, argv);
    else
    if (0 == invariant_wcscmp(L"permtosd", argv[0]))
        return permtosd(argc, argv);
    else
    if (0 == invariant_wcscmp(L"sdtoperm", argv[0]))
        return sdtoperm(argc, argv);

    else
        usage();

    return 0;
}

void wmainCRTStartup(void)
{
    DWORD Argc;
    PWSTR *Argv;

    Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    if (0 == Argv)
        ExitProcess(GetLastError());

    ExitProcess(wmain(Argc, Argv));
}

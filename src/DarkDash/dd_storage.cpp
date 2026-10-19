/*--------------------------------------------------------------------------
    dd_storage.cpp -- HDD partition registry, safe secondary-disk probe.

    CerBIOS HDD1 support: probe kernel objects read-only, create free DOS
    aliases, verify accessible directory roots, roll back invalid mounts. Never
    format, write a partition table, or steal a preexisting drive letter.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_storage.h"

/* MU slots use A/B/H/I/J/K/L/M; D is DarkDash's own assets, S optical,
   Q reserved for legacy disc access, X/Y/Z are primary cache volumes.
   A secondary disk may be formatted with E alone, or E/F/G/C separately.
   These are ALIASES, not claims that a physical HDD1 is always present. */
static DD_StorageVolume s_volumes[] = {
    { 'C', 0, 2, "\\Device\\Harddisk0\\Partition2", 0, 0 },
    { 'E', 0, 1, "\\Device\\Harddisk0\\Partition1", 0, 0 },
    { 'F', 0, 6, "\\Device\\Harddisk0\\Partition6", 0, 0 },
    { 'G', 0, 7, "\\Device\\Harddisk0\\Partition7", 0, 0 },
    { 'X', 0, 3, "\\Device\\Harddisk0\\Partition3", 0, 0 },
    { 'Y', 0, 4, "\\Device\\Harddisk0\\Partition4", 0, 0 },
    { 'Z', 0, 5, "\\Device\\Harddisk0\\Partition5", 0, 0 },
    { 'N', 1, 1, "\\Device\\Harddisk1\\Partition1", 0, 0 },
    { 'O', 1, 6, "\\Device\\Harddisk1\\Partition6", 0, 0 },
    { 'P', 1, 7, "\\Device\\Harddisk1\\Partition7", 0, 0 },
    { 'R', 1, 2, "\\Device\\Harddisk1\\Partition2", 0, 0 }
};
#define DD_STORAGE_COUNT ((int)(sizeof(s_volumes) / sizeof(s_volumes[0])))

static char StorageUpper(char letter) {
    return (letter >= 'a' && letter <= 'z') ? (char)(letter - 32) : letter;
}
static void StorageString(STRING* s, char* buffer) {
    int n = 0;
    while (buffer[n]) ++n;
    s->Length = (USHORT)n;
    s->MaximumLength = (USHORT)(n + 1);
    s->Buffer = buffer;
}
static void StorageLink(char letter, char* out) {
    out[0] = '\\'; out[1] = '?'; out[2] = '?'; out[3] = '\\';
    out[4] = letter; out[5] = ':'; out[6] = 0;
}

int DD_Storage_Count(void) { return DD_STORAGE_COUNT; }
const DD_StorageVolume* DD_Storage_At(int index) {
    return index < 0 || index >= DD_STORAGE_COUNT ? NULL : &s_volumes[index];
}
const DD_StorageVolume* DD_Storage_Letter(char letter) {
    int i;
    letter = StorageUpper(letter);
    for (i = 0; i < DD_STORAGE_COUNT; ++i)
        if (s_volumes[i].letter == letter) return &s_volumes[i];
    return NULL;
}

static int StorageMount(const DD_StorageVolume* v, int replace) {
    char name[8];
    STRING link, device;
    LONG status;
    StorageLink(v->letter, name);
    StorageString(&link, name);
    StorageString(&device, (char*)v->device);
    if (replace) IoDeleteSymbolicLink(&link);
    /* For HDD1, no deletion/overwrite: existing letter always wins safely. */
    status = IoCreateSymbolicLink(&link, &device);
    return status == 0 ? 1 : 0;
}

void DD_Storage_MountPrimary(void) {
    int i;
    for (i = 0; i < DD_STORAGE_COUNT; ++i) {
        if (s_volumes[i].disk != DD_STORAGE_HDD0) continue;
        /* This is the original DarkDash behaviour: remap HDD0 C/E/F/G/X/Y/Z,
           even if an optional partition (F/G) is absent. No HDD1 link touched. */
        s_volumes[i].mounted = StorageMount(&s_volumes[i], 1);
    }
}

/* Access probe: success only says that the kernel exposes this object and
   permits read-attribute access. It does NOT assert FATX validity, physical
   drive identity, filesystem space, or compatibility with a particular BIOS. */
static int StorageProbe(const char* device) {
    STRING path;
    OBJECT_ATTRIBUTES object;
    IO_STATUS_BLOCK io;
    HANDLE handle = NULL;
    NTSTATUS status;
    StorageString(&path, (char*)device);
    object.RootDirectory = NULL;
    object.ObjectName = &path;
    object.Attributes = 0;
    ZeroMemory(&io, sizeof(io));
    status = NtOpenFile(&handle, FILE_READ_ATTRIBUTES, &object, &io,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (status < 0 || !handle) return 0;
    NtClose(handle);
    return 1;
}
int DD_Storage_ProbeSecondary(void) {
    int count = 0, i;
    for (i = 0; i < DD_STORAGE_COUNT; ++i) {
        DD_StorageVolume* v = &s_volumes[i];
        if (v->disk != DD_STORAGE_HDD1) continue;
        /* Don't change a mounted volume's status just because a later probe
           hits transient media or driver state. */
        if (!v->mounted) v->probed = StorageProbe(v->device);
        if (v->probed) ++count;
    }
    return count;
}
int DD_Storage_Ready(const DD_StorageVolume* v) {
    char root[4];
    DWORD attr;
    if (!v || !v->mounted) return 0;
    root[0] = v->letter; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    attr = GetFileAttributesA(root);
    return attr != 0xFFFFFFFFUL && (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* Name is stable and corresponds to partition number, not a guessed game path. */
static char StoragePartLetter(const DD_StorageVolume* v) {
    if (!v) return '?';
    if (!v->disk) return v->letter;
    switch (v->partition) {
    case 1: return 'E';
    case 2: return 'C';
    case 6: return 'F';
    case 7: return 'G';
    default: return '?';
    }
}
void DD_Storage_Label(const DD_StorageVolume* v, char* out, int cap) {
    int j = 0;
    const char* prefix = " (HDD1-";
    if (!out || cap < 1) return;
    out[0] = 0;
    if (!v) return;
    /* Put the actual DOS letter first: the file manager and folder picker
       sort these labels alphabetically, and users can immediately recognize
       N:/O:/P:/R: rather than hunting for an HDD1- prefix. */
    if (j < cap - 1) out[j++] = v->letter;
    if (j < cap - 1) out[j++] = ':';
    if (v->disk) {
        while (*prefix && j < cap - 1) out[j++] = *prefix++;
        if (j < cap - 1) out[j++] = StoragePartLetter(v);
        if (j < cap - 1) out[j++] = ')';
    }
    out[j] = 0;
}
const char* DD_Storage_FtpName(const DD_StorageVolume* v) {
    if (!v || !v->disk) return NULL;
    switch (v->partition) {
    case 1: return "HDD1-E";
    case 2: return "HDD1-C";
    case 6: return "HDD1-F";
    case 7: return "HDD1-G";
    default: return NULL;
    }
}

int DD_Storage_MountSecondary(void) {
    int i, count = 0;
    for (i = 0; i < DD_STORAGE_COUNT; ++i) {
        DD_StorageVolume* v = &s_volumes[i];
        if (v->disk != DD_STORAGE_HDD1) continue;
        if (!v->mounted && v->probed && StorageMount(v, 0)) {
            /* Kernel object existence alone does not mean a usable filesystem.
               Only expose volumes that Win32 can actually traverse. */
            v->mounted = 1;
            if (!DD_Storage_Ready(v)) {
                char name[8];
                STRING link;
                StorageLink(v->letter, name);
                StorageString(&link, name);
                IoDeleteSymbolicLink(&link);  /* our newly created link ONLY */
                v->mounted = 0;
            }
        }
        if (DD_Storage_Ready(v)) ++count;
    }
    return count;
}
int DD_Storage_RefreshSecondary(void) {
    DD_Storage_ProbeSecondary();
    return DD_Storage_MountSecondary();
}

const char* DD_Storage_LaunchDevice(char letter) {
    const DD_StorageVolume* v = DD_Storage_Letter(letter);
    if (!v) return NULL;
    if (v->disk == DD_STORAGE_HDD1 && !DD_Storage_Ready(v)) return NULL;
    return v->device;
}

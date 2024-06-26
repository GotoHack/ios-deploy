#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/sysctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <getopt.h>
#include <pwd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/mobile_image_mounter.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/afc.h>

#include <vector>
#include <string>
using namespace std;

#define APP_VERSION    "1.4.0-Linux"
#define PREP_CMDS_PATH "/tmp/fruitstrap-lldb-prep-cmds-"
#ifndef __APPLE__
#define LLDB_SHELL "ios-lldb -s " PREP_CMDS_PATH
#else
#define LLDB_SHELL "lldb -s " PREP_CMDS_PATH
#endif
/*
 * Startup script passed to lldb.
 * To see how xcode interacts with lldb, put this into .lldbinit:
 * log enable -v -f /Users/vargaz/lldb.log lldb all
 * log enable -v -f /Users/vargaz/gdb-remote.log gdb-remote all
 */
// "platform select remote-ios --sysroot {symbols_path}\n\ "
#define MAIN_LLDB_PREP_CMDS "\
    platform select remote-ios --sysroot /home/david/syms\n\
    target create \"{disk_app}\"\n\
    script fruitstrap_device_app=\"{device_app}\"\n\
    script fruitstrap_connect_url=\"connect://localhost:{device_port}\"\n\
    command script import \"{python_file_path}\"\n\
    command script add -f {python_command}.connect_command connect\n\
    command script add -s asynchronous -f {python_command}.run_command run\n\
    command script add -s asynchronous -f {python_command}.autoexit_command autoexit\n\
    command script add -s asynchronous -f {python_command}.safequit_command safequit\n\
    connect\n\
"

#define LLDB_PREP_CMDS MAIN_LLDB_PREP_CMDS

const char* lldb_prep_no_cmds = "";

const char* lldb_prep_interactive_cmds = "\
    run\n\
";

#ifdef __APPLE__
const char* lldb_prep_noninteractive_justlaunch_cmds = "\
    run\n\
    safequit\n\
";
#else
const char* lldb_prep_noninteractive_justlaunch_cmds = "\
    run\n\
    safequit\n\
    quit\n\
";
#endif

const char* lldb_prep_noninteractive_cmds = "\
    run\n\
    autoexit\n\
";

/*
 * Some things do not seem to work when using the normal commands like process connect/launch, so we invoke them
 * through the python interface. Also, Launch () doesn't seem to work when ran from init_module (), so we add
 * a command which can be used by the user to run it.
 */
#define LLDB_FRUITSTRAP_MODULE ("\
import lldb\n\
import sys\n\
import shlex\n\
\n\
def connect_command(debugger, command, result, internal_dict):\n\
    # These two are passed in by the script which loads us\n\
    connect_url = internal_dict['fruitstrap_connect_url']\n\
    error = lldb.SBError()\n\
\n\
    process = lldb.target.ConnectRemote(lldb.target.GetDebugger().GetListener(), connect_url, None, error)\n\
\n\
    # Wait for connection to succeed\n\
    listener = lldb.target.GetDebugger().GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged)\n\
    events = []\n\
    state = (process.GetState() or lldb.eStateInvalid)\n\
    while state != lldb.eStateConnected:\n\
        event = lldb.SBEvent()\n\
        if listener.WaitForEvent(1, event):\n\
            state = process.GetStateFromEvent(event)\n\
            events.append(event)\n\
        else:\n\
            state = lldb.eStateInvalid\n\
\n\
    # Add events back to queue, otherwise lldb freezes\n\
    for event in events:\n\
        listener.AddEvent(event)\n\
\n\
def run_command(debugger, command, result, internal_dict):\n\
    device_app = internal_dict['fruitstrap_device_app']\n\
    error = lldb.SBError()\n\
    lldb.target.modules[0].SetPlatformFileSpec(lldb.SBFileSpec(device_app))\n\
    lldb.target.Launch(lldb.SBLaunchInfo(shlex.split('{args}')), error)\n\
    lockedstr = ': Locked'\n\
    if lockedstr in str(error):\n\
       print('\\nDevice Locked\\n')\n\
       sys.exit(254)\n\
    else:\n\
       print(str(error))\n\
\n\
def safequit_command(debugger, command, result, internal_dict):\n\
    process = lldb.target.process\n\
    listener = debugger.GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged | lldb.SBProcess.eBroadcastBitSTDOUT | lldb.SBProcess.eBroadcastBitSTDERR)\n\
    event = lldb.SBEvent()\n\
    while True:\n\
        if listener.WaitForEvent(1, event):\n\
            state = process.GetStateFromEvent(event)\n\
        else:\n\
            state = lldb.eStateInvalid\n\
        process.Detach()\n\
        sys.exit(0)\n\
\n\
def autoexit_command(debugger, command, result, internal_dict):\n\
    process = lldb.target.process\n\
    listener = debugger.GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged | lldb.SBProcess.eBroadcastBitSTDOUT | lldb.SBProcess.eBroadcastBitSTDERR)\n\
    event = lldb.SBEvent()\n\
    while True:\n\
        if listener.WaitForEvent(1, event):\n\
            state = process.GetStateFromEvent(event)\n\
        else:\n\
            state = lldb.eStateInvalid\n\
\n\
        stdout = process.GetSTDOUT(1024)\n\
        while stdout:\n\
            sys.stdout.write(stdout)\n\
            stdout = process.GetSTDOUT(1024)\n\
\n\
        stderr = process.GetSTDERR(1024)\n\
        while stderr:\n\
            sys.stdout.write(stderr)\n\
            stderr = process.GetSTDERR(1024)\n\
\n\
        if lldb.SBProcess.EventIsProcessEvent(event):\n\
            if state == lldb.eStateExited:\n\
                sys.exit(process.GetExitStatus())\n\
            if state == lldb.eStateStopped:\n\
                debugger.HandleCommand('frame select')\n\
                debugger.HandleCommand('bt')\n\
                sys.exit({exitcode_app_crash})\n\
")

#if 0

typedef struct am_device * AMDeviceRef;
mach_error_t AMDeviceSecureStartService(struct am_device *device, CFStringRef service_name, unsigned int *unknown, service_conn_t *handle);
int AMDeviceSecureTransferPath(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceSecureInstallApplication(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceMountImage(AMDeviceRef device, CFStringRef image, CFDictionaryRef options, void *callback, int cbarg);
mach_error_t AMDeviceLookupApplications(AMDeviceRef device, CFDictionaryRef options, CFDictionaryRef *result);
int AMDeviceGetInterfaceType(struct am_device *device);

#endif

bool found_device = false, debug = false, verbose = false, unbuffered = false, nostart = false, detect_only = false, install = true, uninstall = false;
bool command_only = false;
const char *command = NULL;
char *target_filename = NULL;
char *upload_pathname = NULL;
char *bundle_id = NULL;
bool interactive = true;
bool justlaunch = false;
char *app_path = NULL;
char *device_id = NULL;
char *args = NULL;
char *list_root = NULL;
int timeout = 0;
int port = 12345;
const char *last_path = NULL;
int gdbfd; // TODO:Q: was service_conn_t
pid_t parent = 0;
// PID of child process running lldb
pid_t child = 0;
pid_t debugserver = 0;
// Signal sent from child to parent process when LLDB finishes.
const int SIGLLDB = SIGUSR1;
char * best_device_match = NULL; // type was AMDeviceRef

// Error codes we report on different failures, so scripts can distinguish between user app exit
// codes and our exit codes. For non app errors we use codes in reserved 128-255 range.
const int exitcode_error = 253;
const int exitcode_app_crash = 254;

#if 0

Boolean path_exists(CFTypeRef path) {
    if (CFGetTypeID(path) == CFStringGetTypeID()) {
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
        Boolean result = CFURLResourceIsReachable(url, NULL);
        CFRelease(url);
        return result;
    } else if (CFGetTypeID(path) == CFURLGetTypeID()) {
        return CFURLResourceIsReachable(path, NULL);
    } else {
        return false;
    }
}

CFStringRef find_path(CFStringRef rootPath, CFStringRef namePattern, CFStringRef expression) {
    FILE *fpipe = NULL;
    CFStringRef quotedRootPath = rootPath;
    CFStringRef cf_command;
    CFRange slashLocation;

    if (CFStringGetCharacterAtIndex(rootPath, 0) != '`') {
        quotedRootPath = CFStringCreateWithFormat(NULL, NULL, CFSTR("'%@'"), rootPath);
    }

    slashLocation = CFStringFind(namePattern, CFSTR("/"), 0);
    if (slashLocation.location == kCFNotFound) {
        cf_command = CFStringCreateWithFormat(NULL, NULL, CFSTR("find %@ -name '%@' %@ 2>/dev/null | sort | tail -n 1"), quotedRootPath, namePattern, expression);
    } else {
        cf_command = CFStringCreateWithFormat(NULL, NULL, CFSTR("find %@ -path '%@' %@ 2>/dev/null | sort | tail -n 1"), quotedRootPath, namePattern, expression);
    }

    if (quotedRootPath != rootPath) {
        CFRelease(quotedRootPath);
    }

    char command[1024] = { '\0' };
    CFStringGetCString(cf_command, command, sizeof(command), kCFStringEncodingUTF8);
    CFRelease(cf_command);

    if (!(fpipe = (FILE *)popen(command, "r")))
    {
        perror("Error encountered while opening pipe");
        exit(exitcode_error);
    }

    char buffer[256] = { '\0' };

    fgets(buffer, sizeof(buffer), fpipe);
    pclose(fpipe);

    strtok(buffer, "\n");
    return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

CFStringRef copy_long_shot_disk_image_path() {
    return find_path(CFSTR("`xcode-select --print-path`"), CFSTR("DeveloperDiskImage.dmg"), CFSTR(""));
}

CFStringRef copy_xcode_dev_path() {
    static char xcode_dev_path[256] = { '\0' };
    if (strlen(xcode_dev_path) == 0) {
        FILE *fpipe = NULL;
        char *command = "xcode-select -print-path";

        if (!(fpipe = (FILE *)popen(command, "r")))
        {
            perror("Error encountered while opening pipe");
            exit(exitcode_error);
        }

        char buffer[256] = { '\0' };

        fgets(buffer, sizeof(buffer), fpipe);
        pclose(fpipe);

        strtok(buffer, "\n");
        strcpy(xcode_dev_path, buffer);
    }
    return CFStringCreateWithCString(NULL, xcode_dev_path, kCFStringEncodingUTF8);
}

const char *get_home() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd *pwd = getpwuid(getuid());
        home = pwd->pw_dir;
    }
    return home;
}

CFStringRef copy_xcode_path_for(CFStringRef subPath, CFStringRef search) {
    CFStringRef xcodeDevPath = copy_xcode_dev_path();
    CFStringRef path;
    bool found = false;
    const char* home = get_home();
    CFRange slashLocation;


    // Try using xcode-select --print-path
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@/%@"), xcodeDevPath, subPath, search);
        found = path_exists(path);
    }
    // Try find `xcode-select --print-path` with search as a name pattern
    if (!found) {
        slashLocation = CFStringFind(search, CFSTR("/"), 0);
        if (slashLocation.location == kCFNotFound) {
        path = find_path(CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, subPath), search, CFSTR("-maxdepth 1"));
        } else {
             path = find_path(CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, subPath), search, CFSTR(""));
        }
        found = CFStringGetLength(path) > 0 && path_exists(path);
    }
    // If not look in the default xcode location (xcode-select is sometimes wrong)
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Applications/Xcode.app/Contents/Developer/%@&%@"), subPath, search);
        found = path_exists(path);
    }
    // If not look in the users home directory, Xcode can store device support stuff there
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/%@/%@"), home, subPath, search);
        found = path_exists(path);
    }

    CFRelease(xcodeDevPath);

    if (found) {
        return path;
    } else {
        CFRelease(path);
        return NULL;
    }
}

#endif

#define GET_FRIENDLY_MODEL_NAME(VALUE, INTERNAL_NAME, FRIENDLY_NAME)  if (!strcmp(VALUE, INTERNAL_NAME)) { free(model); return FRIENDLY_NAME; };


// Please ensure that device is connected or the name will be unknown
const char *get_device_hardware_name(plist_t deviceinfo) {

    char *get_plist_string_value(plist_t plist, const char *key);
    char *model = get_plist_string_value(deviceinfo, "HardwareModel");

    if (model == NULL) {
        return "Unknown Device";
    }

    // iPod Touch
    
    GET_FRIENDLY_MODEL_NAME(model, "N45AP",  "iPod Touch")
    GET_FRIENDLY_MODEL_NAME(model, "N72AP",  "iPod Touch 2G")
    GET_FRIENDLY_MODEL_NAME(model, "N18AP",  "iPod Touch 3G")
    GET_FRIENDLY_MODEL_NAME(model, "N81AP",  "iPod Touch 4G")
    GET_FRIENDLY_MODEL_NAME(model, "N78AP",  "iPod Touch 5G")
    GET_FRIENDLY_MODEL_NAME(model, "N78AAP", "iPod Touch 5G")
        
    // iPad
        
    GET_FRIENDLY_MODEL_NAME(model, "K48AP",  "iPad")
    GET_FRIENDLY_MODEL_NAME(model, "K93AP",  "iPad 2")
    GET_FRIENDLY_MODEL_NAME(model, "K94AP",  "iPad 2 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "K95AP",  "iPad 2 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "K93AAP", "iPad 2 (Wi-Fi, revision A)")
    GET_FRIENDLY_MODEL_NAME(model, "J1AP",   "iPad 3")
    GET_FRIENDLY_MODEL_NAME(model, "J2AP",   "iPad 3 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "J2AAP",  "iPad 3 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "P101AP", "iPad 4")
    GET_FRIENDLY_MODEL_NAME(model, "P102AP", "iPad 4 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "P103AP", "iPad 4 (CDMA)")
        
    // iPad Mini

    GET_FRIENDLY_MODEL_NAME(model, "P105AP", "iPad mini")
    GET_FRIENDLY_MODEL_NAME(model, "P106AP", "iPad mini (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "P107AP", "iPad mini (CDMA)")

    // Apple TV
    
    GET_FRIENDLY_MODEL_NAME(model, "K66AP",  "Apple TV 2G")
    GET_FRIENDLY_MODEL_NAME(model, "J33AP",  "Apple TV 3G")
    GET_FRIENDLY_MODEL_NAME(model, "J33IAP", "Apple TV 3.1G")
        
    // iPhone

    GET_FRIENDLY_MODEL_NAME(model, "M68AP", "iPhone")
    GET_FRIENDLY_MODEL_NAME(model, "N82AP", "iPhone 3G")
    GET_FRIENDLY_MODEL_NAME(model, "N88AP", "iPhone 3GS")
    GET_FRIENDLY_MODEL_NAME(model, "N90AP", "iPhone 4 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N92AP", "iPhone 4 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N90BAP", "iPhone 4 (GSM, revision A)")
    GET_FRIENDLY_MODEL_NAME(model, "N94AP", "iPhone 4S")
    GET_FRIENDLY_MODEL_NAME(model, "N41AP", "iPhone 5 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N42AP", "iPhone 5 (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N48AP", "iPhone 5c (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N49AP", "iPhone 5c (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N51AP", "iPhone 5s (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N53AP", "iPhone 5s (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N61AP", "iPhone 6 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N56AP", "iPhone 6 Plus")

    return model;
}

#if 0

char * MYCFStringCopyUTF8String(CFStringRef aString) {
  if (aString == NULL) {
    return NULL;
  }

  CFIndex length = CFStringGetLength(aString);
  CFIndex maxSize =
  CFStringGetMaximumSizeForEncoding(length,
                                    kCFStringEncodingUTF8);
  char *buffer = (char *)malloc(maxSize);
  if (CFStringGetCString(aString, buffer, maxSize,
                         kCFStringEncodingUTF8)) {
    return buffer;
  }
  return NULL;
}

#endif

plist_t get_device_info_plist(idevice_t device) {
    // Stuff from ideviceinfo.c
    lockdownd_client_t client = NULL;
	lockdownd_error_t ldret = LOCKDOWN_E_UNKNOWN_ERROR;
	//int format = FORMAT_KEY_VALUE;
	plist_t node = NULL;
    bool simple = true;

	if (LOCKDOWN_E_SUCCESS != (ldret = simple ?
			lockdownd_client_new(device, &client, "ideviceinfo"):
			lockdownd_client_new_with_handshake(device, &client, "ideviceinfo"))) {
		fprintf(stderr, "ERROR: Could not connect to lockdownd, error code %d\n", ldret);
		return NULL;
	}

	/* run query and output information */ //domain, key, node
	if((ldret = lockdownd_get_value(client, NULL, NULL, &node)) != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "Unable to get lockdown values, error code %d\n", ldret);
	}

	lockdownd_client_free(client);

	return node;
}

char *get_plist_string_value(plist_t plist, const char *key)
{
    plist_t val;
    char *ret = NULL;
    if (!plist)
        return NULL;
    val = plist_dict_get_item(plist, key);
    if (!val)
        return NULL;
    plist_get_string_val(val, &ret);
    return ret;
}

const char *get_device_full_name(idevice_t device) {
    char *full_name = NULL,
         *device_udid = NULL,
         *device_name = NULL,
         *model_name = NULL;
    plist_t info;

    idevice_get_udid(device, &device_udid);
    info = get_device_info_plist(device);
    
    device_name = get_plist_string_value(info, "DeviceName");
    model_name = strdup(get_device_hardware_name(info));

    if (verbose)
    {
        printf("Device Name:[%s]\n",device_name);
        printf("Model Name:[%s]\n",model_name);
    }

    if(device_name != NULL && model_name != NULL)
    {
        asprintf(&full_name, "%s '%s' (%s)", model_name, device_name, device_udid);
    }
    else
    {
        asprintf(&full_name, "(%s)", device_udid);
    }

    if(device_udid != NULL)
        free(device_udid);
    if(device_name != NULL)
        free(device_name);
    if(model_name != NULL)
        free(model_name);

    return full_name;
}

const char *get_device_interface_name(idevice_t device) {
    (void)device;
    // AMDeviceGetInterfaceType(device) 0=Unknown, 1 = Direct/USB, 2 = Indirect/WIFI
    switch(1) {
        case 1:
            return "USB";
        case 2:
            return "WIFI";
        default:
            return "Unknown Connection";
    }
}

#if 0

CFMutableArrayRef get_device_product_version_parts(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFArrayRef parts = CFStringCreateArrayBySeparatingStrings(NULL, version, CFSTR("."));
    CFMutableArrayRef result = CFArrayCreateMutableCopy(NULL, CFArrayGetCount(parts), parts);
    CFRelease(version);
    CFRelease(parts);
    return result;
}

CFStringRef copy_device_support_path(AMDeviceRef device) {
    CFStringRef version = NULL;
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;
    CFMutableArrayRef version_parts = get_device_product_version_parts(device);

    while (CFArrayGetCount(version_parts) > 0) {
        version = CFStringCreateByCombiningStrings(NULL, version_parts, CFSTR("."));
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("iOS DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (*)"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), version);
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest"), CFSTR(""));
        }
        CFRelease(version);
        if (path != NULL) {
            break;
        }
        CFArrayRemoveValueAtIndex(version_parts, CFArrayGetCount(version_parts) - 1);
    }

    CFRelease(version_parts);
    CFRelease(build);

    if (path == NULL)
    {
        printf("[ !! ] Unable to locate DeviceSupport directory.\n[ !! ] This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!\n");
        exit(exitcode_error);
    }

    return path;
}

CFStringRef copy_developer_disk_image_path(AMDeviceRef device) {
    CFStringRef version = NULL;
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;
    CFMutableArrayRef version_parts = get_device_product_version_parts(device);

    while (CFArrayGetCount(version_parts) > 0) {
        version = CFStringCreateByCombiningStrings(NULL, version_parts, CFSTR("."));
    if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("iOS DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)/DeveloperDiskImage.dmg"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)/DeveloperDiskImage.dmg"), version, build));
    }
        if (path == NULL) {
             path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("*/%@ (*)/DeveloperDiskImage.dmg"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/DeveloperDiskImage.dmg"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest"), CFSTR("DeveloperDiskImage.dmg"));
        }
        CFRelease(version);
        if (path != NULL) {
            break;
        }
        CFArrayRemoveValueAtIndex(version_parts, CFArrayGetCount(version_parts) - 1);
    }

    CFRelease(version_parts);
    CFRelease(build);
    if (path == NULL)
    {
        printf("[ !! ] Unable to locate DeveloperDiskImage.dmg.\n[ !! ] This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!\n");
        exit(exitcode_error);
    }

    return path;
}

void mount_callback(CFDictionaryRef dict, int arg) {
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));

    if (CFEqual(status, CFSTR("LookingUpImage"))) {
        printf("[  0%%] Looking up developer disk image\n");
    } else if (CFEqual(status, CFSTR("CopyingImage"))) {
        printf("[ 30%%] Copying DeveloperDiskImage.dmg to device\n");
    } else if (CFEqual(status, CFSTR("MountingImage"))) {
        printf("[ 90%%] Mounting developer disk image\n");
    }
}

#endif

void mount_developer_image(idevice_t device) {
    mobile_image_mounter_client_t mnt = NULL;
    plist_t lookup = NULL;
    plist_t imagepresent = NULL;
    uint8_t has_developer_image = 0;
    int ret = 0;

    mobile_image_mounter_start_service(device, &mnt, "ios-deploy");
    mobile_image_mounter_lookup_image(mnt, "Developer", &lookup);

    imagepresent = plist_dict_get_item(lookup, "ImagePresent");
    if (!imagepresent) {
        printf("[ !! ] Unable to read image status.\n");
        ret = exitcode_error;
        goto exit;
    }

    plist_get_bool_val(imagepresent, &has_developer_image);

    if (has_developer_image) {
        printf("[ 95%%] Developer disk image already mounted\n");
        goto exit;
    }

    printf("[ !! ] Please mount the developer disk image.\n");
    ret = exitcode_error;
    goto exit;


//    CFStringRef ds_path = copy_device_support_path(device);
//    CFStringRef image_path = copy_developer_disk_image_path(device);
//    CFStringRef sig_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@.signature"), image_path);
//
//    if (verbose) {
//        printf("Device support path: %s\n", CFStringGetCStringPtr(ds_path, CFStringGetSystemEncoding()));
//        printf("Developer disk image: %s\n", CFStringGetCStringPtr(image_path, CFStringGetSystemEncoding()));
//    }
//    CFRelease(ds_path);
//
//    FILE* sig = fopen(CFStringGetCStringPtr(sig_path, kCFStringEncodingMacRoman), "rb");
//    void *sig_buf = malloc(128);
//    assert(fread(sig_buf, 1, 128, sig) == 128);
//    fclose(sig);
//    CFDataRef sig_data = CFDataCreateWithBytesNoCopy(NULL, sig_buf, 128, NULL);
//    CFRelease(sig_path);
//
//    CFTypeRef keys[] = { CFSTR("ImageSignature"), CFSTR("ImageType") };
//    CFTypeRef values[] = { sig_data, CFSTR("Developer") };
//    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
//    CFRelease(sig_data);
//
//    int result = AMDeviceMountImage(device, image_path, options, &mount_callback, 0);
//    if (result == 0) {
//        printf("[ 95%%] Developer disk image mounted successfully\n");
//    } else if (result == 0xe8000076 /* already mounted */) {
//        printf("[ 95%%] Developer disk image already mounted\n");
//    } else {
//        printf("[ !! ] Unable to mount developer disk image. (%x)\n", result);
//        exit(exitcode_error);
//    }

exit:
    if (mnt) {
        mobile_image_mounter_hangup(mnt);
        mobile_image_mounter_free(mnt);
    }
    if (ret)
        exit(ret);
}

#if 0

mach_error_t transfer_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    if (CFEqual(status, CFSTR("CopyingFile"))) {
        CFStringRef path = CFDictionaryGetValue(dict, CFSTR("Path"));

        if ((last_path == NULL || !CFEqual(path, last_path)) && !CFStringHasSuffix(path, CFSTR(".ipa"))) {
            printf("[%3d%%] Copying %s to device\n", percent / 2, CFStringGetCStringPtr(path, kCFStringEncodingMacRoman));
        }

        if (last_path != NULL) {
            CFRelease(last_path);
        }
        last_path = CFStringCreateCopy(NULL, path);
    }

    return 0;
}

mach_error_t install_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    printf("[%3d%%] %s\n", (percent / 2) + 50, CFStringGetCStringPtr(status, kCFStringEncodingMacRoman));
    return 0;
}

#endif

// Returns the path to the .app bundle on the device.
char *copy_device_app_url(idevice_t device, const char *identifier) {
    char *path = NULL;
    instproxy_client_t ipc = NULL;
    lockdownd_client_t ldc = NULL;
    lockdownd_service_descriptor_t svc = NULL;

    lockdownd_client_new_with_handshake(device, &ldc, "ios-deploy");
    lockdownd_start_service(ldc, "com.apple.mobile.installation_proxy", &svc);
    instproxy_client_new(device, svc, &ipc);
    instproxy_client_get_path_for_bundle_identifier(ipc, identifier, &path);

    instproxy_client_free(ipc);
    lockdownd_service_descriptor_free(svc);
    lockdownd_client_free(ldc);

    // get_path_for_bundle_identifier() returns the path to the
    // executable, so we need to remove the last path component.
    *strrchr(path, '/') = '\0';

    return path;
}



string copy_disk_app_identifier(const char *disk_app_url) {
    extern char *get_bundle_id(const char *);
    return string(get_bundle_id(disk_app_url)); // How is this function different from get_bundle_id?
#if 0
    CFURLRef plist_url = CFURLCreateCopyAppendingPathComponent(NULL, disk_app_url, CFSTR("Info.plist"), false);
    CFReadStreamRef plist_stream = CFReadStreamCreateWithFile(NULL, plist_url);
    CFReadStreamOpen(plist_stream);
    CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, plist_stream, 0, kCFPropertyListImmutable, NULL, NULL);
    CFStringRef bundle_identifier = CFRetain(CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier")));
    CFReadStreamClose(plist_stream);

    CFRelease(plist_url);
    CFRelease(plist_stream);
    CFRelease(plist);

    return bundle_identifier;
#endif
}

#ifndef __APPLE__
string get_disk_app_executable(const string &disk_app_url) {
    string infoplistpath = disk_app_url;
    string executablepath;    
    plist_t infoplist = NULL;
    char *executable = NULL;
    
    if (infoplistpath[infoplistpath.size() - 1] != '/')
        infoplistpath += "/Info.plist";
    else
        infoplistpath += "Info.plist";

    extern plist_t plist_from_path(const char *path);
    infoplist = plist_from_path(infoplistpath.c_str());
    executable = get_plist_string_value(infoplist, "CFBundleExecutable");
    plist_free(infoplist);

    if (!executable) {
        printf("Unable to find bundle executable for app %s\n", disk_app_url.c_str());
        exit(1);
    }

    executablepath = disk_app_url;
    if (executablepath[executablepath.size() - 1] != '/')
        executablepath += "/";
    executablepath += executable;
    free(executable);

    return executablepath;
}
#endif

string path_dirname(const string &path) {
    string r;
    char *str = strdup(path.c_str());
    char *dname = dirname(str);
    if (dname != str)
        free(str);
    r = string(dname);
    free(dname);
    return r;
}

void replace_substring(string &content, const string &needle, const string &replacement) {
    size_t start = 0;
    while ((start = content.find(needle, start)) != string::npos) {
        content.replace(start, needle.length(), replacement);
        start += needle.length();
    }
}

static string int_to_string(int val) {
    char buf[20];
    sprintf(buf, "%d", val);
    return string(buf);
}

void write_lldb_prep_cmds(idevice_t device, const char *disk_app_url) {
    string ds_path = "DSPATH?"; //copy_device_support_path(device);
    string symbols_path = "SYMBOLPATH?"; //CFStringCreateWithFormat(NULL, NULL, CFSTR("'%@/Symbols'"), ds_path);

    string cmds(LLDB_PREP_CMDS);

    replace_substring(cmds, "{symbols_path}", symbols_path);
    replace_substring(cmds, "{ds_path}", ds_path);

    string pmodule(LLDB_FRUITSTRAP_MODULE);

    string exitcode_app_crash_str = int_to_string(exitcode_app_crash);

    replace_substring(pmodule, "{exitcode_app_crash}", exitcode_app_crash_str);

    if (args) {
        replace_substring(cmds, "{args}", args);
        replace_substring(pmodule, "{args}", args);
        //printf("write_lldb_prep_cmds:args: [%s][%s]\n", CFStringGetCStringPtr (cmds,kCFStringEncodingMacRoman), 
        //    CFStringGetCStringPtr(pmodule, kCFStringEncodingMacRoman));
    } else {
        replace_substring(cmds, "{args}", "");
        replace_substring(pmodule, "{args}", "");
        //printf("write_lldb_prep_cmds: [%s][%s]\n", CFStringGetCStringPtr (cmds,kCFStringEncodingMacRoman), 
        //    CFStringGetCStringPtr(pmodule, kCFStringEncodingMacRoman));
    }

    string bundle_identifier = copy_disk_app_identifier(disk_app_url);

    string device_app_path = copy_device_app_url(device, bundle_identifier.c_str());
    replace_substring(cmds, "{device_app}", device_app_path);
    //printf("Device app path: %s\n", device_app_path.c_str());
#ifdef __APPLE__
    replace_substring(cmds, "{disk_app}", disk_app_url);
#else
    // Non-Apple LLDB hosts don't resolve .app bundles to their
    // contained binaries, so we have to do that here.
    string disk_executable = get_disk_app_executable(disk_app_url);
    replace_substring(cmds, "{disk_app}", disk_executable);
#endif

    string device_port = int_to_string(port);
    replace_substring(cmds, "{device_port}", device_port);

    string device_container_path = path_dirname(device_app_path);
    string dcp_noprivate = device_container_path;

    replace_substring(dcp_noprivate, "/private/var/", "/var/");

    replace_substring(cmds, "{device_container}", dcp_noprivate);
    //printf("dcp_noprivate: %s\n", dcp_noprivate.c_str());


    // There aren't any references to {disk_container} ?
    //CFURLRef disk_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, disk_app_url);
    //CFStringRef disk_container_path = CFURLCopyFileSystemPath(disk_container_url, kCFURLPOSIXPathStyle);
    //CFStringFindAndReplace(cmds, CFSTR("{disk_container}"), disk_container_path, range, 0);


    string python_file_path = "/tmp/fruitstrap_";
    string python_command = "fruitstrap_";
    if(device_id != NULL) {
        python_file_path += device_id;
        python_command += device_id;
    }
    python_file_path += ".py";

    replace_substring(cmds, "{python_command}", python_command);
    replace_substring(cmds, "{python_file_path}", python_file_path);


    string prep_cmds_path = PREP_CMDS_PATH;
    if(device_id != NULL)
        prep_cmds_path += device_id;
    FILE *out = fopen(prep_cmds_path.c_str(), "w");
    fwrite(cmds.c_str(), cmds.size(), 1, out);

    // Write additional commands based on mode we're running in
    const char *extra_cmds;
    if (!interactive)
    {
        if (justlaunch)
          extra_cmds = lldb_prep_noninteractive_justlaunch_cmds;
        else
          extra_cmds = lldb_prep_noninteractive_cmds;
    }
    else if (nostart)
        extra_cmds = lldb_prep_no_cmds;
    else
        extra_cmds = lldb_prep_interactive_cmds;
    fwrite(extra_cmds, strlen(extra_cmds), 1, out);
    fclose(out);


    out = fopen(python_file_path.c_str(), "w");
    fwrite(pmodule.c_str(), pmodule.size(), 1, out);
    fclose(out);
}

#if 0

CFSocketRef server_socket;
CFSocketRef lldb_socket;
CFWriteStreamRef serverWriteStream = NULL;
CFWriteStreamRef lldbWriteStream = NULL;

int kill_ptree(pid_t root, int signum);
void
server_callback (CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info)
{
    int res;

    if (CFDataGetLength (data) == 0) {
        // FIXME: Close the socket
        //shutdown (CFSocketGetNative (lldb_socket), SHUT_RDWR);
        //close (CFSocketGetNative (lldb_socket));
        CFSocketInvalidate(lldb_socket);
        CFSocketInvalidate(server_socket);
        int mypid = getpid();
        assert((child != 0) && (child != mypid)); //child should not be here
        if ((parent != 0) && (parent == mypid) && (child != 0))
        {
            if (verbose)
            {
                printf("Got an empty packet hence killing child (%d) tree\n", child);
            }
            kill_ptree(child, SIGHUP);
        }
        exit(exitcode_error);
        return;
    }
    res = write (CFSocketGetNative (lldb_socket), CFDataGetBytePtr (data), CFDataGetLength (data));
}

void lldb_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info)
{
    //printf ("lldb: %s\n", CFDataGetBytePtr (data));

    if (CFDataGetLength (data) == 0)
        return;
    write (gdbfd, CFDataGetBytePtr (data), CFDataGetLength (data));
}

void fdvendor_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info) {
    CFSocketNativeHandle socket = (CFSocketNativeHandle)(*((CFSocketNativeHandle *)data));

    assert (callbackType == kCFSocketAcceptCallBack);
    //PRINT ("callback!\n");

    lldb_socket  = CFSocketCreateWithNative(NULL, socket, kCFSocketDataCallBack, &lldb_callback, NULL);
    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, lldb_socket, 0), kCFRunLoopCommonModes);
}

#endif
#if 0
#endif

void kill_process(int exitstatus, void *arg) {
    printf("In kill_process\n");
    (void)exitstatus;
    pid_t pid = *(pid_t *)arg;
    int st= kill(pid, SIGTERM);
    printf("pid = %d\tst = %d\n", (int)pid, st);
    perror("kill_process");
}

void start_remote_debug_server() {
    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();
        debugserver = child;

        int status;
        string portstr = int_to_string(port);
        if (device_id)
            status = execlp("idevicedebugserverproxy", "idevicedebugserverproxy", "--udid", device_id, portstr.c_str(), NULL);
        else
            status = execlp("idevicedebugserverproxy", "idevicedebugserverproxy", portstr.c_str(), NULL);

        if (status == -1)
            perror("failed to exec debugserver");
        
        // We might want to eventually determine if the debug server
        // failed and act accordingly, rather than just ignore failure.
        //kill(parent, SIGLLDB);

        // Pass lldb exit code
        //_exit(0);//
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        on_exit(&kill_process, new pid_t(pid));
        debugserver = pid;
        if (setpgid(debugserver, getpgid(0)) < 0)
            perror("setpgid(debugserver)");
    } else {
        perror("fork failed");
        exit(exitcode_error);
    }
}

#ifdef __APPLE__ // I guess this might also work on other BSDs?

void kill_ptree_inner(pid_t root, int signum, struct kinfo_proc *kp, int kp_len) {
    int i;
    for (i = 0; i < kp_len; i++) {
        if (kp[i].kp_eproc.e_ppid == root) {
            kill_ptree_inner(kp[i].kp_proc.p_pid, signum, kp, kp_len);
        }
    }
    if (root != getpid()) {
        kill(root, signum);
    }
}

int kill_ptree(pid_t root, int signum) {
    int mib[3];
    size_t len;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_ALL;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) == -1) {
        return -1;
    }

    struct kinfo_proc *kp = calloc(1, len);
    if (!kp) {
        return -1;
    }

    if (sysctl(mib, 3, kp, &len, NULL, 0) == -1) {
        free(kp);
        return -1;
    }

    kill_ptree_inner(root, signum, kp, len / sizeof(struct kinfo_proc));

    free(kp);
    return 0;
}

#else //ifdef __APPLE__
#ifdef __linux__

int kill_ptree(pid_t root, int signum) {
    fprintf(stderr, "TODO: Implement kill_ptree for linux.\n");
    return kill(root, signum);
    //return kill(-root, signum);  // Maybe this?
}

#else //ifdef __linux
#error Unsupported platform.
#endif
#endif

void killed(int signum) {
    (void)signum;
    // SIGKILL needed to kill lldb, probably a better way to do this.
    kill(0, SIGKILL);
    _exit(0);
}

void lldb_finished_handler(int signum)
{printf("DQ: in lldb_finished_handler!\n");
    (void)signum;
    int status = 0;
    if (waitpid(child, &status, 0) == -1)
        perror("waitpid failed");
    _exit(WEXITSTATUS(status));
}

void bring_process_to_foreground() {
    if (setpgid(0, 0) == -1)
        perror("setpgid failed");

    signal(SIGTTOU, SIG_IGN);
    if (tcsetpgrp(STDIN_FILENO, getpid()) == -1)
        perror("tcsetpgrp failed");
    signal(SIGTTOU, SIG_DFL);
}

void setup_dummy_pipe_on_stdin(int pfd[2]) {
    if (pipe(pfd) == -1)
        perror("pipe failed");
    if (dup2(pfd[0], STDIN_FILENO) == -1)
        perror("dup2 failed");
}

void setup_lldb(idevice_t device, const char *url) {
    const char *device_full_name;

    device_full_name = get_device_full_name(device);

    //device_interface_name = get_device_interface_name(device);
    
    //AMDeviceConnect(device);
    //assert(AMDeviceIsPaired(device));
    //assert(AMDeviceValidatePairing(device) == 0);
    //assert(AMDeviceStartSession(device) == 0);
    
    printf("------ Debug phase ------\n");
    
//    if(AMDeviceGetInterfaceType(device) == 2)
//    {
//        printf("Cannot debug %s over %s.\n", CFStringGetCStringPtr(device_full_name, CFStringGetSystemEncoding()), CFStringGetCStringPtr(device_interface_name, CFStringGetSystemEncoding()));
//        exit(0);
//    }
    
    printf("Starting debug of %s connected through %s...\n", device_full_name, "USB");
    
    mount_developer_image(device);      // put debugserver on the device
    start_remote_debug_server();  // start debugserver
    write_lldb_prep_cmds(device, url);   // dump the necessary lldb commands into a file
    
    printf("[100%%] Connecting to remote debug server\n");
    printf("-------------------------\n");
    
    setpgid(getpid(), 0);
    signal(SIGHUP, killed);
    signal(SIGINT, killed);
    signal(SIGTERM, killed);
    // Need this before fork to avoid race conditions. For child process we remove this right after fork.
    signal(SIGLLDB, lldb_finished_handler);
    
    parent = getpid();
}

void launch_debugger(idevice_t device, char *url) {
    setup_lldb(device, url);


    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();

        int pfd[2] = {-1, -1};
        if (isatty(STDIN_FILENO))
            // If we are running on a terminal, then we need to bring process to foreground for input
            // to work correctly on lldb's end.
            bring_process_to_foreground();
        else
            // If lldb is running in a non terminal environment, then it freaks out spamming "^D" and
            // "quit". It seems this is caused by read() on stdin returning EOF in lldb. To hack around
            // this we setup a dummy pipe on stdin, so read() would block expecting "user's" input.
            setup_dummy_pipe_on_stdin(pfd);

        char lldb_prep[400];
        sprintf(lldb_prep, PREP_CMDS_PATH);
        if(device_id != NULL)
            strcat(lldb_prep, device_id);

        printf("lldb_prep: %s\n", lldb_prep);

        const char *args[8] = { "ios-lldb", "-s", lldb_prep, NULL };
        int status = execvp("ios-lldb", (char* const*)args);

        if (status == -1)
            perror("failed to exec lldb");

        close(pfd[0]);
        close(pfd[1]);
        
        // Notify parent we're exiting
        kill(parent, SIGLLDB);
        // Pass lldb exit code
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        child = pid;
        int status = -1;
        waitpid(child, &status, 0);
        printf("child exited with status %d\n", status);
    } else {
        perror("fork failed");
        exit(exitcode_error);
    }
}

void launch_debugger_and_exit(idevice_t device, char *url) {
    setup_lldb(device,url);
    int pfd[2] = {-1, -1};
    if (pipe(pfd) == -1)
        perror("Pipe failed");
    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();
        
        if (dup2(pfd[0],STDIN_FILENO) == -1)
            perror("dup2 failed");
        
        char lldb_prep[400];
        sprintf(lldb_prep, PREP_CMDS_PATH);
        if(device_id != NULL)
            strcat(lldb_prep, device_id);

        printf("lldb_prep: %s\n", lldb_prep);

        const char *args[8] = { "ios-lldb", "-s", lldb_prep, NULL };
        int status = execvp("ios-lldb", (char* const*)args);
        
        close(pfd[0]);
        
        // Notify parent we're exiting
        kill(parent, SIGLLDB);
        // Pass lldb exit code
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        child = pid;
        if (verbose)
            printf("Waiting for child [Child: %d][Parent: %d]\n", child, parent);
        int status = -1;
        waitpid(child, &status, 0);
    } else {
        perror("fork failed");
        exit(exitcode_error);
    }
}

#if 0

void launch_debugger_and_exit(AMDeviceRef device, CFURLRef url) {
    setup_lldb(device,url);
    int pfd[2] = {-1, -1};
    if (pipe(pfd) == -1)
        perror("Pipe failed");
    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();
        
        if (dup2(pfd[0],STDIN_FILENO) == -1)
            perror("dup2 failed");
        
        char lldb_shell[400];
        sprintf(lldb_shell, LLDB_SHELL);
        if(device_id != NULL)
            strcat(lldb_shell, device_id);
        
        int status = system(lldb_shell); // launch lldb
        if (status == -1)
            perror("failed launching lldb");
        
        close(pfd[0]);
        
        // Notify parent we're exiting
        kill(parent, SIGLLDB);
        // Pass lldb exit code
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        child = pid;
        if (verbose)
            printf("Waiting for child [Child: %d][Parent: %d]\n", child, parent);
    } else {
        perror("fork failed");
        exit(exitcode_error);
    }
}

#endif

plist_t plist_from_path(const char *path)
{
    char buf[4096];
    FILE *file;
    vector<char> data;
    size_t read;
    plist_t root = NULL;

    if (!(file = fopen(path, "r")))
        return NULL;

    while ((read = fread(buf, 1, sizeof(buf), file)))
        data.insert(data.end(), buf, &buf[read]);
    assert(!ferror(file));
    fclose(file);

    if (!data.size())
        return NULL;

    if (data[0] == '<')
        plist_from_xml(data.data(), data.size(), &root);
    else
        plist_from_bin(data.data(), data.size(), &root);

    return root;
}

char* get_bundle_id(const char *app_url)
{
    if (app_url == NULL)
        return NULL;

    string url_s = string(app_url) + "/Info.plist";
    const char *url = url_s.c_str();
    plist_t infoplist, bundle_id;
    char *value = NULL;

    infoplist = plist_from_path(url);

    if (infoplist == NULL)
        return NULL;

    bundle_id = plist_dict_get_item(infoplist, "CFBundleIdentifier");
    if (!bundle_id)
        return NULL;

    plist_get_string_val(bundle_id, &value);

    return value;
}

#if 1

typedef char **afc_dictionary_t;

// Returns 0 if good, returns 1 if no more key/value pairs.
int afc_key_value_read(afc_dictionary_t dict, char **key, char **value) {
    static afc_dictionary_t cur_dict = NULL;
    static afc_dictionary_t cur_ptr = NULL;

    if (!dict) {
        cur_dict = cur_ptr = NULL;
    }

    *key = NULL;
    *value = NULL;

    if (dict != cur_dict) {
        cur_dict = dict;
        cur_ptr = dict;
    }
    
    if (*cur_ptr) {
        *key = *cur_ptr;
        cur_ptr++;
        *value = *cur_ptr;
        cur_ptr++;
        return 0;
    }

    return 1;
}

void read_dir(house_arrest_client_t afcFd, afc_client_t afc_conn_p, const char* dir,
              void(*callback)(afc_client_t conn,const char *dir,int file))
{
    char *dir_ent = NULL;

    if (!afc_conn_p) {
        printf("================ !afc_conn_p. What? =================\n");
//        afc_conn_p = &afc_conn;
//        AFCConnectionOpen(afcFd, 0, &afc_conn_p);
    }
    
    printf("%s\n", dir);
    
    afc_dictionary_t afc_dict = NULL;
    char *key, *val;
    int not_dir = 0;

    unsigned int code = (unsigned int)afc_get_file_info(afc_conn_p, dir, &afc_dict);
    if (code != 0) {
        // there was a problem reading or opening the file to get info on it, abort
        return;
    }

    while((afc_key_value_read(afc_dict,&key,&val) == 0) && key && val) {
        if (strcmp(key,"st_ifmt")==0) {
            not_dir = strcmp(val,"S_IFDIR");
            break;
        }
    }

    afc_dictionary_free(afc_dict);

    if (not_dir) {
    	if (callback) (*callback)(afc_conn_p, dir, not_dir);
        return;
    }

    char **afc_dir_p = NULL;
    afc_error_t err = afc_read_directory(afc_conn_p, dir, &afc_dir_p);
    
    if (err != 0) {
        // Couldn't open dir - was probably a file
        // dquesada: Assume the entry is actually a file.
        if (err == AFC_E_READ_ERROR) {
            if (callback) (*callback)(afc_conn_p, dir, 1);
        }
        return;
    } else {
        if (callback) (*callback)(afc_conn_p, dir, not_dir);
    }

    char **dir_p = afc_dir_p;
    while(true) {
        dir_ent = *dir_p++;
        if (!dir_ent)
            break;
        
        if (strcmp(dir_ent, ".") == 0 || strcmp(dir_ent, "..") == 0)
            continue;
        
        char* dir_joined = (char*)malloc(strlen(dir) + strlen(dir_ent) + 2);
        strcpy(dir_joined, dir);
        if (dir_joined[strlen(dir)-1] != '/')
            strcat(dir_joined, "/");
        strcat(dir_joined, dir_ent);
        read_dir(afcFd, afc_conn_p, dir_joined, callback);
        free(dir_joined);
    }

    // It's not a dictionary, but it frees the same way.
    afc_dictionary_free(afc_dir_p);
}

#endif

// Used to send files to app-specific sandbox (Documents dir)
house_arrest_client_t start_house_arrest_service(idevice_t device) {
    house_arrest_client_t house = NULL;
    plist_t result = NULL;

    if (bundle_id == NULL) {
        printf("Bundle id is not specified\n");
        exit(1);
    }

    if (house_arrest_client_start_service(device, &house, "ios-deploy") != HOUSE_ARREST_E_SUCCESS) {
        printf("failed to start house_arrest service.\n");
        exit(1);
    }

    if (house_arrest_send_command(house, "VendDocuments", bundle_id) != HOUSE_ARREST_E_SUCCESS) {
//    if (house_arrest_send_command(house, "VendContainer", bundle_id) != HOUSE_ARREST_E_SUCCESS) {
        printf("failed to command VendDocuments.\n");
        exit(1);
    }

    if (house_arrest_get_result(house, &result) != HOUSE_ARREST_E_SUCCESS
        || !result) {
        printf("failed to get house_arrest result.\n");
        exit(1);
    }

    char *status, *error;
    status = get_plist_string_value(result, "Status");
    error = get_plist_string_value(result, "Error");

    if (error || !status || (status && strcmp(status, "Complete"))) {
        //if (error && !strcmp(error, "InstallationLookupFailed"))
        printf("Failed to find app with bundle id %s\n", bundle_id);
        exit(1);
    }

    if (result)
        plist_free(result);

    return house;
}

char* get_filename_from_path(char* path)
{
    char *ptr = path + strlen(path);
    while (ptr > path)
    {
        if (*ptr == '/')
            break;
        --ptr;
    }
    if (ptr+1 >= path+strlen(path))
        return NULL;
    if (ptr == path)
        return ptr;
    return ptr+1;
}

void* read_file_to_memory(char * path, size_t* file_size)
{
    struct stat buf;
    int err = stat(path, &buf);
    if (err < 0)
    {
        return NULL;
    }
    
    *file_size = buf.st_size;
    FILE* fd = fopen(path, "r");
    char* content = (char*)malloc(*file_size);
    if (fread(content, *file_size, 1, fd) != 1)
    {
        fclose(fd);
        return NULL;
    }
    fclose(fd);
    return content;
}

void list_files(idevice_t device)
{
    house_arrest_client_t house = NULL;
    afc_client_t afc = NULL;

    house = start_house_arrest_service(device);

    if (afc_client_new_from_house_arrest_client(house, &afc) == AFC_E_SUCCESS) {
        read_dir(house, afc, list_root?list_root:"/", NULL);
        afc_client_free(afc);        
    }

    if (house)
        house_arrest_client_free(house);
}

void copy_file_callback(afc_client_t afc_conn_p, const char *name,int file)
{
    typedef uint64_t afc_file_ref;

    const char *local_name=name;

    if (*local_name=='/') local_name++;

    if (*local_name=='\0') return;

    if (file) {
	afc_file_ref fref;
	int err = afc_file_open(afc_conn_p,name,AFC_FOPEN_RDONLY,&fref);

	if (err) {
	    fprintf(stderr,"afc_file_open(\"%s\") failed: %d\n",name,err);
	    return;
	}

	FILE *fp = fopen(local_name,"w");

	if (fp==NULL) {
	    fprintf(stderr,"fopen(\"%s\",\"w\") failer: %s\n",local_name,strerror(errno));
	    afc_file_close(afc_conn_p,fref);
	    return;
	}

	char buf[4096];
	uint32_t sz=sizeof(buf);

	while (afc_file_read(afc_conn_p,fref,buf,sz,&sz)==AFC_E_SUCCESS && sz) {
	    fwrite(buf,(size_t)sz,1,fp);
	    sz = sizeof(buf);
	}

	afc_file_close(afc_conn_p,fref);
	fclose(fp);
    } else {
	if (mkdir(local_name,0777) && errno!=EEXIST)
	    fprintf(stderr,"mkdir(\"%s\") failed: %s\n",local_name,strerror(errno));
    }
}

void mkdirhier(char *path)
{
    char *slash;
    struct stat buf;

    if (path[0]=='.' && path[1]=='/') path+=2;

    if ((slash = strrchr(path,'/'))) {
	*slash = '\0';
	if (stat(path,&buf)==0) {
	    *slash = '/';
	    return;
	}
	mkdirhier(path);
	mkdir (path,0777);
	*slash = '/';
    }

    return;
}

void download_tree(idevice_t device)
{
    house_arrest_client_t house = start_house_arrest_service(device);
    afc_client_t afc_conn_p = NULL;
    char *dirname = NULL;

    if (house && afc_client_new_from_house_arrest_client(house, &afc_conn_p) == 0)  do {

	if (target_filename) {
	    dirname = strdup(target_filename);
	    mkdirhier(dirname);
	    if (mkdir(dirname,0777) && errno!=EEXIST) {
		fprintf(stderr,"mkdir(\"%s\") failed: %s\n",dirname,strerror(errno));
		break;
	    }
	    if (chdir(dirname)) {
		fprintf(stderr,"chdir(\"%s\") failed: %s\n",dirname,strerror(errno));
		break;
	    }
	}

	read_dir(house, afc_conn_p, list_root?list_root:"/", copy_file_callback);

    } while(0);

    if (dirname) free(dirname);
    if (afc_conn_p) afc_client_free(afc_conn_p);
    if (house) house_arrest_client_free(house);
}

void upload_file(idevice_t device) {
    typedef uint64_t afc_file_ref;

    house_arrest_client_t house = start_house_arrest_service(device);
    
    afc_file_ref file_ref = 0;
    
    afc_client_t afc_conn_p = NULL;
    afc_client_new_from_house_arrest_client(house, &afc_conn_p);
    
    //        read_dir(houseFd, NULL, "/", NULL);
    
    if (!target_filename)
    {
        target_filename = get_filename_from_path(upload_pathname);
    }

    uint32_t file_size, size_written = 0;
    void* file_content = read_file_to_memory(upload_pathname, (size_t*)&file_size);
    
    if (!file_content)
    {
        printf("Could not open file: %s\n", upload_pathname);
        exit(-1);
    }

    // Make sure the directory was created
    {
        char *dirpath = strdup(target_filename);
        char *c = dirpath, *lastSlash = dirpath;
        while(*c) {
            if(*c == '/') {
                lastSlash = c;
            }
            c++;
        }
        *lastSlash = '\0';
        assert(afc_make_directory(afc_conn_p, dirpath) == AFC_E_SUCCESS);
    }
    

    int ret = afc_file_open(afc_conn_p, target_filename, (afc_file_mode_t)3, &file_ref);
    if (ret == AFC_E_PERM_DENIED) {
        printf("Cannot write to %s. Permission error.\n", target_filename);
        exit(1);
    }
    if (ret == AFC_E_OBJECT_IS_DIR) {
        printf("Target %s is a directory.\n", target_filename);
        exit(1);
    }
    assert(ret == AFC_E_SUCCESS);
    assert(afc_file_write(afc_conn_p, file_ref, (char *)file_content, file_size, &size_written) == AFC_E_SUCCESS);
    assert(size_written == file_size);
    assert(afc_file_close(afc_conn_p, file_ref) == AFC_E_SUCCESS);
    assert(afc_client_free(afc_conn_p) == AFC_E_SUCCESS);
    assert(house_arrest_client_free(house) == HOUSE_ARREST_E_SUCCESS);
    
    free(file_content);
}

void handle_device(idevice_t device) {
    //if (found_device) 
    //    return; // handle one device only

    idevice_error_t err;
    char *found_device_id = NULL;
    const char *device_full_name = get_device_full_name(device),
               *device_interface_name = get_device_interface_name(device);

    idevice_get_udid(device, &found_device_id);

    if (detect_only) {
        printf("[....] Found %s connected through %s.\n", device_full_name, device_interface_name);
        found_device = true;
        return;
    }


    if (device_id != NULL) {
        if(strcmp(device_id, found_device_id) == 0) {
            found_device = true;
        } else {
            printf("Skipping %s.\n", device_full_name);
            return;
        }
    } else {
        device_id = found_device_id;
        found_device = true;
    }

    printf("[....] Using %s (%s).\n", device_full_name, found_device_id);
    if ((err = idevice_new(&device, found_device_id)) != IDEVICE_E_SUCCESS)
        printf("Failed to create device, error code %d\n", err);
    
    if (command_only) {
        if (strcmp("list", command) == 0) {
            list_files(device);
        } else if (strcmp("upload", command) == 0) {
            upload_file(device);
        } else if (strcmp("download", command) == 0) {
            download_tree(device);
        }
        exit(0);
    }


//    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);
    char *path = strdup(app_path);
    //CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    char *relative_url = path;
    char *url = realpath(relative_url, NULL);

    if (uninstall) {
        printf("------ Uninstall phase ------\n");

        char *bundle_id = get_bundle_id(url);
        if (bundle_id == NULL) {
            printf("Error: Unable to get bundle id from package %s\n Uninstall failed\n", app_path);
        } else {            
            char *uninstallCommand = NULL;
            int code;

            asprintf(&uninstallCommand, "ideviceinstaller --udid %s --uninstall %s", device_id, bundle_id);

            if (verbose)
                printf("system: %s\n", uninstallCommand);

            code = system(uninstallCommand);//AMDeviceSecureUninstallApplication(0, device, bundle_id, 0, NULL, 0);

            free(uninstallCommand);

            if (code == 0) {
                printf("[ OK ] Uninstalled package with bundle id %s\n", bundle_id);
            } else {
                printf("[ ERROR ] Could not uninstall package with bundle id %s\n", bundle_id);
            }
        }
    }

    if(install) {
        printf("------ Install phase ------\n");
        printf("[  0%%] Found %s connected through %s, beginning install\n", device_full_name, device_interface_name);

        char *installCommand = NULL;
        int installStatus;

        // Outsource the work to ideviceinstaller, rather than porting the
        // code to do this from Apple's private API to libimobiledevice's.
        asprintf(&installCommand, "ideviceinstaller --udid %s --install %s", device_id, url);
        if (verbose)
            printf("system: %s\n", installCommand);

        installStatus = system(installCommand);

        if (installStatus)
            exit(installStatus);
    
        free(installCommand);

        printf("[100%%] Installed package %s\n", app_path);
    }

    if (!debug) 
        exit(0); // no debug phase
    
    if (justlaunch)
        launch_debugger_and_exit(device, url);
    else
        launch_debugger(device, url);
}

void device_callback(const idevice_event_t *event, void *) {
    idevice_t device = NULL;
    switch (event->event) {
        case IDEVICE_DEVICE_ADD:
            if(device_id != NULL || !debug) {
                idevice_new(&device, event->udid);
                //handle_device(event->udid);
                handle_device(device);
                idevice_free(device);
            } else if(best_device_match == NULL) {
                best_device_match = strdup(event->udid);
            }
        default:
            break;
    }
}

void timeout_callback() {
    idevice_t device = NULL;
    if ((!found_device) && (!detect_only))  {
        if(best_device_match != NULL) {
            //handle_device(best_device_match);
            idevice_new(&device, best_device_match);
            handle_device(device);
            idevice_free(device);

            free(best_device_match);
            best_device_match = NULL;
        }

        if(!found_device) {
            printf("[....] Timed out waiting for device.\n");
            exit(exitcode_error);
        }
    }
    else
    {
      if (!debug) {
          printf("[....] No more devices found.\n");
      }
      
      if (detect_only && !found_device) {
          exit(exitcode_error);
          return;
      } else {
          int mypid = getpid();
          if ((parent != 0) && (parent == mypid) && (child != 0))
          {
              if (verbose)
              {
                  printf("Timeout. Killing child (%d) tree\n", child);
              }
              kill_ptree(child, SIGHUP);
          }
      }
      exit(0);
    }
}

void usage(const char* app) {
    printf(
        "Usage: %s [OPTION]...\n"
        "  -d, --debug                  launch the app in lldb after installation\n"
        "  -i, --id <device_id>         the id of the device to connect to\n"
        "  -c, --detect                 only detect if the device is connected\n"
        "  -b, --bundle <bundle.app>    the path to the app bundle to be installed\n"
        "  -a, --args <args>            command line arguments to pass to the app when launching it\n"
        "  -t, --timeout <timeout>      number of seconds to wait for a device to be connected\n"
        "  -u, --unbuffered             don't buffer stdout\n"
        "  -n, --nostart                do not start the app when debugging\n"
        "  -I, --noninteractive         start in non interactive mode (quit when app crashes or exits)\n"
        "  -L, --justlaunch             just launch the app and exit lldb\n"
        "  -v, --verbose                enable verbose output\n"
        "  -m, --noinstall              directly start debugging without app install (-d not required)\n"
        "  -p, --port <number>          port used for device, default: 12345 \n"
        "  -r, --uninstall              uninstall the app before install (do not use with -m; app cache and data are cleared) \n"
        "  -1, --bundle_id <bundle id>  specify bundle id for list and upload\n"
        "  -l, --list                   list files\n"
        "  -o, --upload <file>          upload file\n"
        "  -w, --download               download app tree\n"
        "  -2, --to <target pathname>   use together with up/download file/tree. specify target\n"
        "  -V, --version                print the executable version \n",
        app);
}

void show_version() {
	printf("%s\n", APP_VERSION);
}

int main(int argc, char *argv[]) {
    static struct option longopts[] = {
        { "debug", no_argument, NULL, 'd' },
        { "id", required_argument, NULL, 'i' },
        { "bundle", required_argument, NULL, 'b' },
        { "args", required_argument, NULL, 'a' },
        { "verbose", no_argument, NULL, 'v' },
        { "timeout", required_argument, NULL, 't' },
        { "unbuffered", no_argument, NULL, 'u' },
        { "nostart", no_argument, NULL, 'n' },
        { "noninteractive", no_argument, NULL, 'I' },
        { "justlaunch", no_argument, NULL, 'L' },
        { "detect", no_argument, NULL, 'c' },
        { "version", no_argument, NULL, 'V' },
        { "noinstall", no_argument, NULL, 'm' },
        { "port", required_argument, NULL, 'p' },
        { "uninstall", no_argument, NULL, 'r' },
        { "list", optional_argument, NULL, 'l' },
        { "bundle_id", required_argument, NULL, '1'},
        { "upload", required_argument, NULL, 'o'},
        { "download", optional_argument, NULL, 'w'},
        { "to", required_argument, NULL, '2'},
        { NULL, 0, NULL, 0 },
    };
    char ch;

    while ((ch = getopt_long(argc, argv, "VmcdvunrILi:b:a:t:g:x:p:1:2:o:l::w::", longopts, NULL)) != -1)
    {
        switch (ch) {
        case 'm':
            install = 0;
            debug = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'i':
            device_id = optarg;
            break;
        case 'b':
            app_path = optarg;
            break;
        case 'a':
            args = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'u':
            unbuffered = 1;
            break;
        case 'n':
            nostart = 1;
            break;
        case 'I':
            interactive = false;
            break;
        case 'L':
            interactive = false;
            justlaunch = true;
            break;
        case 'c':
            detect_only = true;
            debug = 1;
            break;
        case 'V':
            show_version();
            return 0;
        case 'p':
            port = atoi(optarg);
            break;
        case 'r':
            uninstall = 1;
            break;
        case '1':
            bundle_id = optarg;
            break;
        case '2':
            target_filename = optarg;
            break;
        case 'o':
            command_only = true;
            upload_pathname = optarg;
            command = "upload";
            break;
        case 'l':
            command_only = true;
            command = "list";
            list_root = optarg;
            break;
        case 'w':
            command_only = true;
            command = "download";
            list_root = optarg;
            break;
        default:
            usage(argv[0]);
            return exitcode_error;
        }
    }

    if (!app_path && !detect_only && !command_only) {
        usage(argv[0]);
        exit(exitcode_error);
    }

    if (unbuffered) {
        setbuf(stdout, NULL);
        setbuf(stderr, NULL);
    }

    if (detect_only && timeout == 0) {
        timeout = 5;
    }

    if (app_path) {
        assert(access(app_path, F_OK) == 0);
    }

//    if (timeout > 0)
//    {
//        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + timeout, 0, 0, 0, timeout_callback, NULL);
//        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
//        printf("[....] Waiting up to %d seconds for iOS device to be connected\n", timeout);
//    }
//    else
//    {
//        printf("[....] Waiting for iOS device to be connected\n");
//   }
//
//    struct am_device_notification *notify;
//    AMDeviceNotificationSubscribe(&device_callback, 0, 0, NULL, &notify);
//    CFRunLoopRun();

    // TODO: Handle timeout w/ libimobiledevice.

    char **devlist = NULL;
    int devcount = 0;
    if (idevice_get_device_list(&devlist, &devcount) < 0)
        printf("Failed to get devlist\n");

    for (int i = 0; i < devcount; ++i) {
        idevice_event_t fake_event;
        fake_event.event = IDEVICE_DEVICE_ADD;
        fake_event.udid = devlist[i];
        fake_event.conn_type = CONNECTION_USBMUXD;
        device_callback(&fake_event, NULL);
    }
    if (!found_device)
        timeout_callback();
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <cutils/properties.h>
#include <sys/system_properties.h>

#define USER_GPS_TYPE_PROP "persist.user.gps.type"
bool isBdsSelected()
{
    char user_gps_type[PROPERTY_VALUE_MAX];

    property_get(USER_GPS_TYPE_PROP, user_gps_type, "");
    if (!strncmp("hd8020gps", user_gps_type, sizeof("hd8020gps"))){
        return true;
    } else {
        return false;
    }
}

int main (){
	char gpsd_params[PROP_VALUE_MAX];
	char boot_mode[PROP_VALUE_MAX];
	char cmd[1024];
	int i = 0;

	property_get("ro.bootmode", boot_mode, "");
	if(strstr(boot_mode, "ffbm") || !isBdsSelected())
	{
		__android_log_print(ANDROID_LOG_DEBUG, "gpsd_wrapper", "checked in ffbm mode or user not select bds chip, exit...");
		return 0;
	}else{
		__android_log_print(ANDROID_LOG_DEBUG, "gpsd_wrapper", "boot mode : ", boot_mode);
	}

	property_get("persist.gpsd.parameters", gpsd_params, "-Nn,-D1,/dev/ttyHSL1");
	while (gpsd_params[i] != 0){
		if (gpsd_params[i] == ',') gpsd_params[i] = ' ';
		i++;
	}

	sprintf(cmd, "/system/bin/logwrapper /vendor/bin/gpsd %s", gpsd_params);

	__android_log_print(ANDROID_LOG_DEBUG, "gpsd_wrapper", "Starting gpsd: %s", cmd);

	system(cmd);
	return 0;
}


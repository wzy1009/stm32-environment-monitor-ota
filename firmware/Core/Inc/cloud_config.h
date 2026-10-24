#ifndef CLOUD_CONFIG_H
#define CLOUD_CONFIG_H

#ifndef CLOUD_USE_PRIVATE_CONFIG
#define CLOUD_USE_PRIVATE_CONFIG 0
#endif
#if CLOUD_USE_PRIVATE_CONFIG
#include "cloud_private.h"
#else
/* Empty credentials keep the public build in local monitoring mode. */
#define CLOUD_WIFI_SSID ""
#define CLOUD_WIFI_PASSWORD ""
#define CLOUD_BEMFA_UID ""
#define CLOUD_STATE_TOPIC "envState"
#define CLOUD_COMMAND_TOPIC "envCmd"
#endif
#endif

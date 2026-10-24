/* Fake credentials only; this header is selected exclusively by host tests. */
extern char test_ssid[40], test_password[80], test_uid[40], test_state_topic[48], test_command_topic[48];
#define CLOUD_WIFI_SSID test_ssid
#define CLOUD_WIFI_PASSWORD test_password
#define CLOUD_BEMFA_UID test_uid
#define CLOUD_STATE_TOPIC test_state_topic
#define CLOUD_COMMAND_TOPIC test_command_topic

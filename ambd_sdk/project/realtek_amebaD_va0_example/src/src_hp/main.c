#include <platform_opts.h>
#include "FreeRTOS.h"
#include "task.h"
#include <platform/platform_stdlib.h>
#include <lwip_netconf.h>
#include "wifi_constants.h"
#include "wifi_structures.h"
#include "lwip_netconf.h"
#include "wifi_conf.h"
#include "dhcp/dhcps.h"

#include "device.h"
#include "gpio_api.h" // mbed

#if CONFIG_LWIP_LAYER
extern struct netif xnetif[NET_IF_NUM];
#endif

#define MAC_ARG(x) ((u8 *)(x))[0], ((u8 *)(x))[1], ((u8 *)(x))[2], ((u8 *)(x))[3], ((u8 *)(x))[4], ((u8 *)(x))[5]
static rtw_result_t scan_result_handler(rtw_scan_handler_result_t *malloced_scan_result);

static char *ssid;
static char *password;

extern int error_flag;

#define GPIO_LED_PIN PA_14

gpio_t gpio_led;
static void blink_led(void *pvParameters);

/**
 * @brief  Scan network
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Scan network
 */
static void scan_network(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example for network scan...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/**********************************************************************************
	 *	2. Scan network
	 **********************************************************************************/
	// Scan network and print them out.
	// Can refer to fATWS() in atcmd_wifi.c and scan_result_handler() below.
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Scan network\n");
	if (wifi_scan_networks(scan_result_handler, NULL) != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_scan_networks() failed\n");
		return;
	}
	// Wait for scan finished.
	vTaskDelay(5000);
}

/**
 * @brief  Wi-Fi example for scenario case 3.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Scan network
 *		- Connect to AP use STA mode (If failed, re-connect one time.)
 *		- Enable Wi-Fi Direct GO
 *			(It will re-enable WiFi, the original connection to AP would be broken.)
 */
static void scenario_3(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 3...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/**********************************************************************************
	 *	2. Scan network
	 **********************************************************************************/
	// Scan network and print them out.
	// Can refer to fATWS() in atcmd_wifi.c.
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Scan network\n");
	if (wifi_scan_networks(scan_result_handler, NULL) != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_scan_networks() failed\n");
		return;
	}
	// Wait for scan finished.
	vTaskDelay(5000);

	/**********************************************************************************
	 *	3. Connect to AP use STA mode (If failed, re-connect one time.)
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP use STA mode\n");

	// Set the auto-reconnect mode with retry 1 time(limit is 2) and timeout 5 seconds.
	// This command need to be set before invoke wifi_connect() to make reconnection work.
	wifi_config_autoreconnect(1, 2, 5);

	// Connect to AP with PSK-WPA2-AES.
	ssid = "Nha5G";
	password = "23456789";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

	// Show Wi-Fi info.
	rtw_wifi_setting_t setting;
	wifi_get_setting(WLAN0_NAME, &setting);
	wifi_show_setting(WLAN0_NAME, &setting);

#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	/**********************************************************************************
	 *	4. Enable Wi-Fi Direct GO
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi Direct GO\n");
	// Start Wi-Fi Direct mode.
	// cmd_wifi_p2p_start() will re-enable the Wi-Fi with P2P mode and initialize P2P data.
	cmd_wifi_p2p_start(NULL, NULL);
	// Start Wi-Fi Direct Group Owner mode.
	// The GO related parameters can be set in cmd_wifi_p2p_auto_go_start() function declaration.
	if (cmd_wifi_p2p_auto_go_start(NULL, NULL) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: cmd_wifi_p2p_auto_go_start() failed\n");
		return;
	}
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Wi-Fi Direct Group Owner mode enabled\n");

	// Show the Wi-Fi Direct Info.
	cmd_p2p_info(NULL, NULL);
#else
	DiagPrintf("Please set CONFIG_ENABLE_P2P 1 in platform_opts.h to enable P2P\n");
#endif
}

// For processing the scanned result -> just output them.
// Can refer to fATWS() in atcmd_wifi.c.
static rtw_result_t scan_result_handler(rtw_scan_handler_result_t *malloced_scan_result)
{
	static int ApNum = 0;
	if (malloced_scan_result->scan_complete != RTW_TRUE)
	{
		rtw_scan_result_t *record = &malloced_scan_result->ap_details;
		record->SSID.val[record->SSID.len] = 0; /* Ensure the SSID is null terminated */

		DiagPrintf("%d\t", ++ApNum);
		DiagPrintf("%s\t ", (record->bss_type == RTW_BSS_TYPE_ADHOC) ? "Adhoc" : "Infra");
		DiagPrintf("%02x:%02x:%02x:%02x:%02x:%02x", MAC_ARG(record->BSSID.octet));
		DiagPrintf(" %d\t ", record->signal_strength);
		DiagPrintf(" %d\t  ", record->channel);
		DiagPrintf(" %d\t  ", record->wps_type);
		DiagPrintf("%s\t\t ", (record->security == RTW_SECURITY_OPEN) ? "Open" : (record->security == RTW_SECURITY_WEP_PSK)			 ? "WEP"
																			 : (record->security == RTW_SECURITY_WPA_TKIP_PSK)		 ? "WPA TKIP"
																			 : (record->security == RTW_SECURITY_WPA_AES_PSK)		 ? "WPA AES"
																			 : (record->security == RTW_SECURITY_WPA_MIXED_PSK)		 ? "WPA Mixed"
																			 : (record->security == RTW_SECURITY_WPA2_AES_PSK)		 ? "WPA2 AES"
																			 : (record->security == RTW_SECURITY_WPA2_TKIP_PSK)		 ? "WPA2 TKIP"
																			 : (record->security == RTW_SECURITY_WPA2_MIXED_PSK)	 ? "WPA2 Mixed"
																			 : (record->security == RTW_SECURITY_WPA_WPA2_TKIP_PSK)	 ? "WPA/WPA2 TKIP"
																			 : (record->security == RTW_SECURITY_WPA_WPA2_AES_PSK)	 ? "WPA/WPA2 AES"
																			 : (record->security == RTW_SECURITY_WPA_WPA2_MIXED_PSK) ? "WPA/WPA2 Mixed"
																			 : (record->security == RTW_SECURITY_WPA3_AES_PSK)		 ? "WPA3 AES"
																			 : (record->security == RTW_SECURITY_WPA2_WPA3_MIXED)	 ? "WPA2/WPA3-SAE AES"
																																	 : "Unknown");
		DiagPrintf(" %s ", record->SSID.val);
		DiagPrintf("\r\n");
	}
	return RTW_SUCCESS;
}

static void example_wlan_scenario_thread()
{
	// Wait for other task stable.
	vTaskDelay(4000);

	// Scan network.
	scan_network();

	scenario_3();

	vTaskDelete(NULL);
}

void main()
{
	// Init LED control pin
	gpio_init(&gpio_led, GPIO_LED_PIN);
	gpio_dir(&gpio_led, PIN_OUTPUT); // Direction: Output
	gpio_mode(&gpio_led, PullNone);	 // No pull

	xTaskCreate(blink_led,	 /* The function that implements the task. */
				"blink_led", /* Just a text name for the task to aid debugging. */
				1024,		 /* The stack size is defined in FreeRTOSIPConfig.h. */
				NULL,		 /* The task parameter, not used in this case. */
				7,			 /* The priority assigned to the task is defined in FreeRTOSConfig.h. */
				NULL);		 /* The task handle is not used. */

	if (xTaskCreate(example_wlan_scenario_thread, ((const char *)"example_wlan_scenario_thread"), 1024, "S3", tskIDLE_PRIORITY + 1, NULL) != pdPASS)
		DiagPrintf("\n\r%s xTaskCreate failed\n", __FUNCTION__);
	vTaskStartScheduler();
}

static void blink_led(void *pvParameters)
{
	for (;;)
	{
		// turn off LED
		gpio_write(&gpio_led, 0);
		vTaskDelay(pdMS_TO_TICKS(200));
		// turn on LED
		gpio_write(&gpio_led, 1);
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}

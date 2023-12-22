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
#include <example_wlan_scenario.h>

#include <example_mqtt.h>
#define MQTT_TASK
#define MQTT_SELECT_TIMEOUT 1
bool flag_connected;
extern TaskHandle_t xHandle;
static const char *const msg_types_str[] =
	{
		"Reserved",
		"CONNECT",
		"CONNACK",
		"PUBLISH",
		"PUBACK",
		"PUBREC",
		"PUBREL",
		"PUBCOMP",
		"SUBSCRIBE",
		"SUBACK",
		"UNSUBSCRIBE",
		"UNSUBACK",
		"PINGREQ",
		"PINGRESP",
		"DISCONNECT",
		"Reserved"};
static const char *const mqtt_status_str[] =
	{
		"MQTT_START",
		"MQTT_CONNECT",
		"MQTT_SUBTOPIC",
		"MQTT_RUNNING"};

static int decodePacket(MQTTClient *c, int *value, int timeout)
{
	unsigned char i;
	int multiplier = 1;
	int len = 0;
	const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

	*value = 0;
	do
	{
		int rc = MQTTPACKET_READ_ERROR;

		if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES)
		{
			rc = MQTTPACKET_READ_ERROR; /* bad data */
			goto exit;
		}
		rc = c->ipstack->mqttread(c->ipstack, &i, 1, timeout);
		if (rc != 1)
			goto exit;
		*value += (i & 127) * multiplier;
		multiplier *= 128;
	} while ((i & 128) != 0);
exit:
	return len;
}
static int sendPacket(MQTTClient *c, int length, Timer *timer)
{
	int rc = FAILURE,
		sent = 0;

	while (sent < length && !TimerIsExpired(timer))
	{
		rc = c->ipstack->mqttwrite(c->ipstack, &c->buf[sent], length, TimerLeftMS(timer));
		if (rc < 0) // there was an error writing the data
			break;
		sent += rc;
	}
	if (sent == length)
	{
		TimerCountdown(&c->ping_timer, c->keepAliveInterval); // record the fact that we have successfully sent the packet
		rc = SUCCESS;
	}
	else
	{
		rc = FAILURE;
		mqtt_printf(MQTT_DEBUG, "Send packet failed");
	}

	if (c->ipstack->my_socket < 0)
	{
		c->isconnected = 0;
	}

	return rc;
}
static int readPacket(MQTTClient *c, Timer *timer)
{
	int rc = FAILURE;
	MQTTHeader header = {0};
	int len = 0;
	int rem_len = 0;

	/* 1. read the header byte.  This has the packet type in it */
	if (c->ipstack->mqttread(c->ipstack, c->readbuf, 1, TimerLeftMS(timer)) != 1)
	{
		mqtt_printf(MQTT_MSGDUMP, "read packet header failed");
		goto exit;
	}
	len = 1;
	/* 2. read the remaining length.  This is variable in itself */
	decodePacket(c, &rem_len, TimerLeftMS(timer));
	len += MQTTPacket_encode(c->readbuf + 1, rem_len); /* put the original remaining length back into the buffer */

	if (len + rem_len > c->readbuf_size)
	{
		mqtt_printf(MQTT_WARNING, "rem_len = %d, read buffer will overflow", rem_len);
		rc = BUFFER_OVERFLOW;
		goto exit;
	}
	/* 3. read the rest of the buffer using a callback to supply the rest of the data */
	if (rem_len > 0 && (c->ipstack->mqttread(c->ipstack, c->readbuf + len, rem_len, TimerLeftMS(timer)) != rem_len))
	{
		mqtt_printf(MQTT_MSGDUMP, "read the rest of the data failed");
		goto exit;
	}
	header.byte = c->readbuf[0];
	rc = header.bits.type;
exit:
	if (c->ipstack->my_socket < 0)
	{
		c->isconnected = 0;
	}
	return rc;
}
static void messageArrived(MessageData *data)
{
	mqtt_printf(MQTT_INFO, "Message arrived on topic %s: %s\n", data->topicName->lenstring.data, (char *)data->message->payload);
}
void MQTTSetStatus(MQTTClient *c, int mqttstatus)
{
	c->mqttstatus = mqttstatus;
	mqtt_printf(MQTT_INFO, "Set mqtt status to %s", mqtt_status_str[mqttstatus]);
}
static int MQTTDataHandle(MQTTClient *c, fd_set *readfd, MQTTPacket_connectData *connectData, messageHandler messageHandler, char *address, char *topic)
{
	short packet_type = 0;
	int rc = 0;
	int mqttstatus = c->mqttstatus;
	int mqtt_rxevent = 0;
	int mqtt_fd = c->ipstack->my_socket;

	mqtt_rxevent = (mqtt_fd >= 0) ? FD_ISSET(mqtt_fd, readfd) : 0;

	if (mqttstatus == MQTT_START)
	{
		mqtt_printf(MQTT_INFO, "MQTT start");
		if (c->isconnected)
		{
			c->isconnected = 0;
		}
		mqtt_printf(MQTT_INFO, "Connect Network \"%s\"", address);
		if ((rc = NetworkConnect(c->ipstack, address, 1993)) != 0)
		{
			mqtt_printf(MQTT_INFO, "Return code from network connect is %d\n", rc);
			goto exit;
		}
		mqtt_printf(MQTT_INFO, "\"%s\" Connected", address);
		mqtt_printf(MQTT_INFO, "Start MQTT connection");
		TimerInit(&c->cmd_timer);
		TimerCountdownMS(&c->cmd_timer, c->command_timeout_ms);
		if ((rc = MQTTConnect(c, connectData)) != 0)
		{
			mqtt_printf(MQTT_INFO, "Return code from MQTT connect is %d\n", rc);
			goto exit;
		}
		MQTTSetStatus(c, MQTT_CONNECT);
		goto exit;
	}

	if (mqtt_rxevent)
	{
		c->ipstack->m2m_rxevent = 0;
		Timer timer;
		TimerInit(&timer);
		TimerCountdownMS(&timer, 1000);
		packet_type = readPacket(c, &timer);
		if (packet_type > 0 && packet_type < 15)
			mqtt_printf(MQTT_DEBUG, "Read packet type is %s", msg_types_str[packet_type]);
		else
		{
			mqtt_printf(MQTT_DEBUG, "Read packet type is %d", packet_type);
			MQTTSetStatus(c, MQTT_START);
			c->ipstack->disconnect(c->ipstack);
			rc = FAILURE;
			goto exit;
		}
	}
	switch (mqttstatus)
	{
	case MQTT_CONNECT:
		if (packet_type == CONNACK)
		{
			unsigned char connack_rc = 255;
			unsigned char sessionPresent = 0;
			if (MQTTDeserialize_connack(&sessionPresent, &connack_rc, c->readbuf, c->readbuf_size) == 1)
			{
				rc = connack_rc;
				mqtt_printf(MQTT_INFO, "MQTT Connected");
				TimerInit(&c->cmd_timer);
				TimerCountdownMS(&c->cmd_timer, c->command_timeout_ms);
				if ((rc = MQTTSubscribe(c, topic, QOS2, messageHandler)) != 0)
				{
					mqtt_printf(MQTT_INFO, "Return code from MQTT subscribe is %d\n", rc);
				}
				else
				{
					mqtt_printf(MQTT_INFO, "Subscribe to Topic: %s", topic);
					MQTTSetStatus(c, MQTT_SUBTOPIC);
				}
			}
			else
			{
				mqtt_printf(MQTT_DEBUG, "Deserialize CONNACK failed");
				rc = FAILURE;
			}
		}
		else if (TimerIsExpired(&c->cmd_timer))
		{
			mqtt_printf(MQTT_DEBUG, "Not received CONNACK");
			rc = FAILURE;
		}
		if (rc == FAILURE)
		{
			MQTTSetStatus(c, MQTT_START);
		}
		break;
	case MQTT_SUBTOPIC:
		if (packet_type == SUBACK)
		{
			int count = 0, grantedQoS = -1;
			unsigned short mypacketid;
			int isSubscribed = 0;
			if (MQTTDeserialize_suback(&mypacketid, 1, &count, &grantedQoS, c->readbuf, c->readbuf_size) == 1)
			{
				rc = grantedQoS; // 0, 1, 2 or 0x80
				mqtt_printf(MQTT_DEBUG, "grantedQoS: %d", grantedQoS);
			}
			if (rc != 0x80)
			{
				int i;
				for (i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
				{
					if (c->messageHandlers[i].topicFilter == topic)
					{
						isSubscribed = 1;
						break;
					}
				}
				if (!isSubscribed)
					for (i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
					{
						if (c->messageHandlers[i].topicFilter == 0)
						{
							c->messageHandlers[i].topicFilter = topic;
							c->messageHandlers[i].fp = messageHandler;
							break;
						}
					}
				rc = 0;
				MQTTSetStatus(c, MQTT_RUNNING);
			}
		}
		else if (TimerIsExpired(&c->cmd_timer))
		{
			mqtt_printf(MQTT_DEBUG, "Not received SUBACK");
			rc = FAILURE;
		}
		if (rc == FAILURE)
		{
			MQTTSetStatus(c, MQTT_START);
		}
		break;
	case MQTT_RUNNING:
		if (packet_type > 0)
		{
			int len = 0;
			Timer timer;
			TimerInit(&timer);
			TimerCountdownMS(&timer, 10000);
			switch (packet_type)
			{
			case CONNACK:
				break;
			case PUBACK:
			{
				unsigned short mypacketid;
				unsigned char dup, type;
				if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
					rc = FAILURE;
				break;
			}
			case SUBACK:
				break;
			case UNSUBACK:
				break;
			case PUBLISH:
			{
				MQTTString topicName;
				MQTTMessage msg;
				int intQoS;
				if (MQTTDeserialize_publish(&msg.dup, &intQoS, &msg.retained, &msg.id, &topicName,
											(unsigned char **)&msg.payload, (int *)&msg.payloadlen, c->readbuf, c->readbuf_size) != 1)
				{
					rc = FAILURE;
					mqtt_printf(MQTT_DEBUG, "Deserialize PUBLISH failed");
					goto exit;
				}

				msg.qos = (enum QoS)intQoS;
				deliverMessage(c, &topicName, &msg);
				if (msg.qos != QOS0)
				{
					if (msg.qos == QOS1)
					{
						len = MQTTSerialize_ack(c->buf, c->buf_size, PUBACK, 0, msg.id);
						mqtt_printf(MQTT_DEBUG, "send PUBACK");
					}
					else if (msg.qos == QOS2)
					{
						len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREC, 0, msg.id);
						mqtt_printf(MQTT_DEBUG, "send PUBREC");
					}
					else
					{
						mqtt_printf(MQTT_DEBUG, "invalid QoS: %d", msg.qos);
					}
					if (len <= 0)
					{
						rc = FAILURE;
						mqtt_printf(MQTT_DEBUG, "Serialize_ack failed");
						goto exit;
					}
					else
					{
						if ((rc = sendPacket(c, len, &timer)) == FAILURE)
						{
							MQTTSetStatus(c, MQTT_START);
							goto exit; // there was a problem
						}
					}
				}
				break;
			}
			case PUBREC:
			{
				unsigned short mypacketid;
				unsigned char dup, type;
				if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
				{
					mqtt_printf(MQTT_DEBUG, "Deserialize PUBREC failed");
					rc = FAILURE;
				}
				else if ((len = MQTTSerialize_ack(c->buf, c->buf_size, PUBREL, 0, mypacketid)) <= 0)
				{
					mqtt_printf(MQTT_DEBUG, "Serialize PUBREL failed");
					rc = FAILURE;
				}
				else if ((rc = sendPacket(c, len, &timer)) != SUCCESS)
				{				  // send the PUBREL packet
					rc = FAILURE; // there was a problem
					MQTTSetStatus(c, MQTT_START);
				}
				break;
			}
			case PUBREL:
			{
				unsigned short mypacketid;
				unsigned char dup, type;
				if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->readbuf, c->readbuf_size) != 1)
				{
					mqtt_printf(MQTT_DEBUG, "Deserialize PUBREL failed");
					rc = FAILURE;
				}
				else if ((len = MQTTSerialize_ack(c->buf, c->buf_size, PUBCOMP, 0, mypacketid)) <= 0)
				{
					mqtt_printf(MQTT_DEBUG, "Serialize PUBCOMP failed");
					rc = FAILURE;
				}
				else if ((rc = sendPacket(c, len, &timer)) != SUCCESS)
				{				  // send the PUBCOMP packet
					rc = FAILURE; // there was a problem
					MQTTSetStatus(c, MQTT_START);
				}
				break;
			}
			case PUBCOMP:
				break;
			case PINGRESP:
				c->ping_outstanding = 0;
				break;
			}
		}
		keepalive(c);
		break;
	default:
		break;
	}
exit:
	return rc;
}

#if CONFIG_EXAMPLE_WLAN_SCENARIO

#if CONFIG_LWIP_LAYER
extern struct netif xnetif[NET_IF_NUM];
#endif

#define MAC_ARG(x) ((u8 *)(x))[0], ((u8 *)(x))[1], ((u8 *)(x))[2], ((u8 *)(x))[3], ((u8 *)(x))[4], ((u8 *)(x))[5]
static rtw_result_t scan_result_handler(rtw_scan_handler_result_t *malloced_scan_result);
static rtw_result_t scan_result_RSSI_handler(rtw_scan_handler_result_t *malloced_scan_result);

char *ssid;
char *password;

extern int error_flag;

/**
 * @brief  Scan network
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Scan network
 */
void scan_network(void)
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
 * @brief  Authentication
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Connect to AP by different authentications
 *			- WPS-PBC
 *			- WPS-PIN static PIN
 *			- WPS-PIN dynamic PIN
 *			- open
 *			- WEP open (64 bit)
 *			- WEP open (128 bit)
 *			- WEP shared (64 bit)
 *			- WEP shared (128 bit)
 *			- WPA-PSK (TKIP)
 *			- WPA-PSK (AES)
 *			- WPA2-PSK (TKIP)
 *			- WPA2-PSK (AES)
 *		- Show Wi-Fi information
 */
static void authentication(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example for authentication...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	2. Connect to AP by different authentications
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP\n");
#if defined(CONFIG_ENABLE_WPS) && (CONFIG_ENABLE_WPS)
	// By WPS-PBC.
	///*
	char *argv[2];
	argv[1] = "pbc";
	cmd_wps(2, argv);
	//*/

	// By WPS-PIN static PIN. With specified PIN code 92402508 as example.
	/*
	char *argv[3];
	argv[1] = "pin";
	argv[2] = "92402508";
	cmd_wps(3, argv);
	*/

	// By WPS-PIN dynamic PIN.
	/*
	char *argv[2];
	argv[1] = "pin";
	cmd_wps(2, argv);
	*/
#else
	DiagPrintf("Please set CONFIG_ENABLE_WPS 1 in platform_opts.h to enable WPS\n");
#endif
	// By open.
	/*
	ssid = "Viethoang";
	if(wifi_connect(ssid, RTW_SECURITY_OPEN, NULL, strlen(ssid), 0, -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WEP open (64 bit).
	/*
	ssid = "Viethoang";
	password = "12345678";	// 5 ASCII
	//password = "AAAAAAAAAA";	// 10 HEX
	int key_id = 2; 	// value of key_id is 0, 1, 2, or 3.
	if(wifi_connect(ssid, RTW_SECURITY_WEP_PSK, password, strlen(ssid), strlen(password), key_id, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WEP open (128 bit).
	/*
	ssid = "Viethoang";
	password = "12345678";	// 13 ASCII
	//password = "AAAAABBBBBCCCCCDDDDD123456";	// 26 HEX
	int key_id = 2; 	// value of key_id is 0, 1, 2, or 3.
	if(wifi_connect(ssid, RTW_SECURITY_WEP_PSK, password, strlen(ssid), strlen(password), key_id, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WEP shared (64 bit).
	/*
	ssid = "Viethoang";
	password = "12345678";	// 5 ASCII
	//password = "AAAAAAAAAA";	// 10 HEX
	int key_id = 2; 	// value of key_id is 0, 1, 2, or 3.
	if(wifi_connect(ssid, RTW_SECURITY_WEP_SHARED, password, strlen(ssid), strlen(password), key_id, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WEP shared (128 bit).
	/*
	ssid = "Viethoang";
	password = "12345678";	// 13 ASCII
	//password = "AAAAABBBBBCCCCCDDDDD123456";	// 26 HEX
	int key_id = 2; 	// value of key_id is 0, 1, 2, or 3.
	if(wifi_connect(ssid, RTW_SECURITY_WEP_SHARED, password, strlen(ssid), strlen(password), key_id, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WPA-PSK (TKIP)
	/*
	ssid = "Viethoang";
	password = "12345678";
	if(wifi_connect(ssid, RTW_SECURITY_WPA_TKIP_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WPA-PSK (AES)
	/*
	ssid = "Viethoang";
	password = "12345678";
	if(wifi_connect(ssid, RTW_SECURITY_WPA_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WPA2-PSK (TKIP)
	/*
	ssid = "Viethoang";
	password = "12345678";
	if(wifi_connect(ssid, RTW_SECURITY_WPA2_TKIP_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	// By WPA2-PSK (AES)
	/*
	ssid = "Viethoang";
	password = "12345678";
	if(wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
	*/

	/*********************************************************************************
	 *	3. Show Wi-Fi information
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Show Wi-Fi information\n");
	rtw_wifi_setting_t setting;
	wifi_get_setting(WLAN0_NAME, &setting);
	if (wifi_get_setting(WLAN0_NAME, &setting) != -1)
	{
		setting.security_type = rltk_get_security_mode_full(WLAN0_NAME);
	}
	wifi_show_setting(WLAN0_NAME, &setting);
}

/**
 * @brief  Wi-Fi example for mode switch case 1: Switch to infrastructure AP mode.
 * @note  Process Flow:
 *		- Disable Wi-Fi
 *		- Enable Wi-Fi with AP mode
 *		- Start AP
 *		- Check AP running
 *		- Start DHCP server
 */
static void mode_switch_1(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 1...\n");

	/*********************************************************************************
	 *	1. Disable Wi-Fi
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi\n");
	wifi_off();
	vTaskDelay(20);

	/*********************************************************************************
	 *	2. Enable Wi-Fi with AP mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with AP mode\n");
	if (wifi_on(RTW_MODE_AP) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	3. Start AP
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start AP\n");
	ssid = "RTL_AP";
	rtw_security_t security_type = RTW_SECURITY_WPA2_AES_PSK;
	password = "";
	int channel = 6;
	if (wifi_start_ap(ssid, security_type, password, strlen(ssid), strlen(password), channel) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_start_ap failed\n");
		return;
	}

	/*********************************************************************************
	 *	4. Check AP running
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Check AP running\n");
	int timeout = 20;
	while (1)
	{
		char essid[33];
		if (wext_get_ssid(WLAN0_NAME, (unsigned char *)essid) > 0)
		{
			if (strcmp((const char *)essid, (const char *)ssid) == 0)
			{
				DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] %s started\n", ssid);
				break;
			}
		}
		if (timeout == 0)
		{
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: Start AP timeout\n");
			return;
		}
		vTaskDelay(1 * configTICK_RATE_HZ);
		timeout--;
	}

	/*********************************************************************************
	 *	5. Start DHCP server
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start DHCP server\n");
	// For more setting about DHCP, please reference fATWA in atcmd_wifi.c.
#if CONFIG_LWIP_LAYER
	dhcps_init(&xnetif[0]);
#endif
}

/**
 * @brief  Wi-Fi example for mode switch case 2: Switch to infrastructure STA mode.
 * @note  Process Flow:
 *		- Disable Wi-Fi
 *		- Enable Wi-Fi with STA mode
 *		- Connect to AP using STA mode
 *		- Show Wi-Fi information
 */
static void mode_switch_2(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 2...\n");

	/*********************************************************************************
	 *	1. Disable Wi-Fi
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi\n");
	wifi_off();
	vTaskDelay(20);

	/*********************************************************************************
	 *	2. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with STA mode\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	3. Connect to AP using STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP using STA mode\n");
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

	/*********************************************************************************
	 *	4. Show Wi-Fi information
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Show Wi-Fi information\n");
	rtw_wifi_setting_t setting;
	wifi_get_setting(WLAN0_NAME, &setting);
	wifi_show_setting(WLAN0_NAME, &setting);
}

/**
 * @brief  Wi-Fi example for mode switch case 3: Switch to infrastructure P2P Autonomous GO mode.
 * @note  Process Flow:
 *		- Enable Wi-Fi Direct mode
 *		- Enable Wi-Fi Direct Automonous GO
 *		- Show Wi-Fi Direct Information
 *		- Disable Wi-Fi Direct mode
 */
static void mode_switch_3(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 3...\n");
#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	/*********************************************************************************
	 *	1. Enable Wi-Fi Direct mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi Direct mode\n");
	// Start Wi-Fi Direct mode.
	// cmd_wifi_p2p_start() will re-enable the Wi-Fi with P2P mode and initialize P2P data.
	cmd_wifi_p2p_start(NULL, NULL);

	/*********************************************************************************
	 *	2. Enable Wi-Fi Direct Automonous GO
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi Direct Automonous GO\n");
	// Start Wi-Fi Direct Automonous GO.
	// The GO related parameters can be set in cmd_wifi_p2p_auto_go_start() function declaration.
	if (cmd_wifi_p2p_auto_go_start(NULL, NULL) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: cmd_wifi_p2p_auto_go_start() failed\n");
		return;
	}

	/*********************************************************************************
	 *	3. Show Wi-Fi Direct Information
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Show Wi-Fi Direct Information\n");
	// Show the Wi-Fi Direct Info.
	cmd_p2p_info(NULL, NULL);

	vTaskDelay(60000);

	/*********************************************************************************
	 *	4. Disable Wi-Fi Direct mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi Direct mode\n");
	// Disable Wi-Fi Direct GO. Will disable Wi-Fi either.
	// This command has to be invoked to release the P2P resource.
	cmd_wifi_p2p_stop(NULL, NULL);
#else
	DiagPrintf("Please set CONFIG_ENABLE_P2P 1 in platform_opts.h to enable P2P\n");
#endif
}

/**
 * @brief  Wi-Fi example for mode switch case 4: Mode switching time between AP and STA.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Disable STA mode and start AP (First measurement)
 *		- Stop AP and enable STA mode (Second measurement)
 */
static void mode_switch_4(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 4...\n");
	// First measurement.
	unsigned long tick_STA_to_AP_begin;
	unsigned long tick_STA_to_AP_end;
	// Second measurement.
	unsigned long tick_AP_to_STA_begin;
	unsigned long tick_AP_to_STA_end;

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with STA mode\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	2. Disable STA mode and start AP (First measurement)
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable STA mode and start AP (First measurement)\n");
	// Begin time.
	tick_STA_to_AP_begin = xTaskGetTickCount();

	wifi_off();

	if (wifi_on(RTW_MODE_AP) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	ssid = "RTL_AP";
	rtw_security_t security_type = RTW_SECURITY_WPA2_AES_PSK;
	password = "";
	int channel = 6;
	if (wifi_start_ap(ssid, security_type, password, strlen(ssid), strlen(password), channel) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_start_ap failed\n");
		return;
	}

	int timeout = 20;
	while (1)
	{
		char essid[33];
		if (wext_get_ssid(WLAN0_NAME, (unsigned char *)essid) > 0)
		{
			if (strcmp((const char *)essid, (const char *)ssid) == 0)
			{
				DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] %s started\n", ssid);
				break;
			}
		}
		if (timeout == 0)
		{
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: Start AP timeout\n");
			return;
		}
		vTaskDelay(1 * configTICK_RATE_HZ);
		timeout--;
	}

#if CONFIG_LWIP_LAYER
	dhcps_init(&xnetif[0]);
#endif
	// End time.
	tick_STA_to_AP_end = xTaskGetTickCount();
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Time diff switch from STA mode to start AP: %d ms\n",
			   (tick_STA_to_AP_end - tick_STA_to_AP_begin));

	vTaskDelay(60000);

	/*********************************************************************************
	 *	3. Stop AP and enable STA mode (Second measurement)
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Stop AP and enable STA mode (Second measurement)\n");
	// Begin time.
	tick_AP_to_STA_begin = xTaskGetTickCount();

	wifi_off();

	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	// Connect to AP by WPA2-AES security
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

	// End time.
	tick_AP_to_STA_end = xTaskGetTickCount();
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Time diff switch from stop AP to enable STA mode: %d ms\n",
			   (tick_AP_to_STA_end - tick_AP_to_STA_begin));
}

/**
 * @brief  Wi-Fi example for mode switch case 5: Mode switching time between P2P(autonomousGO) and STA.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Disable STA mode and start P2P GO (First measurement)
 *		- Stop P2P GO and enable STA mode (Second measurement)
 */
static void mode_switch_5(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 5...\n");
#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	// First measurement.
	unsigned long tick_STA_to_P2PGO_begin;
	unsigned long tick_STA_to_P2PGO_end;
	// Second measurement.
	unsigned long tick_P2PGO_to_STA_begin;
	unsigned long tick_P2PGO_to_STA_end;
#endif
	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with STA mode\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}
#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	/*********************************************************************************
	 *	2. Disable STA mode and start P2P GO (First measurement)
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable STA mode and start AP (First measurement)\n");
	// Begin time.
	tick_STA_to_P2PGO_begin = xTaskGetTickCount();

	wifi_off();

	cmd_wifi_p2p_start(NULL, NULL);

	if (cmd_wifi_p2p_auto_go_start(NULL, NULL) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: cmd_wifi_p2p_auto_go_start() failed\n");
		return;
	}

	// End time.
	tick_STA_to_P2PGO_end = xTaskGetTickCount();
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Time diff switch from STA mode to start P2P GO: %d ms\n",
			   (tick_STA_to_P2PGO_end - tick_STA_to_P2PGO_begin));

	vTaskDelay(60000);

	/*********************************************************************************
	 *	3. Stop P2P GO and enable STA mode (Second measurement)
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Stop P2P GO and enable STA mode (Second measurement)\n");
	// Begin time.
	tick_P2PGO_to_STA_begin = xTaskGetTickCount();

	cmd_wifi_p2p_stop(NULL, NULL);
#else
	DiagPrintf("Please set CONFIG_ENABLE_P2P 1 in platform_opts.h to enable P2P\n");
#endif
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	// Connect to AP by WPA2-AES security
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	// End time.
	tick_P2PGO_to_STA_end = xTaskGetTickCount();
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Time diff switch from stop P2P GO to enable STA mode: %d ms\n",
			   (tick_P2PGO_to_STA_end - tick_P2PGO_to_STA_begin));
#endif
}

/**
 * @brief  Wi-Fi example for mode switch case 6: Switch to infrastructure AP mode with hidden SSID.
 * @note  Process Flow:
 *		- Disable Wi-Fi
 *		- Enable Wi-Fi with AP mode
 *		- Start AP with hidden SSID
 *		- Check AP running
 *		- Start DHCP server
 */
static void mode_switch_6(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 6...\n");

	/*********************************************************************************
	 *	1. Disable Wi-Fi
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi\n");
	wifi_off();
	vTaskDelay(20);

	/*********************************************************************************
	 *	2. Enable Wi-Fi with AP mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with AP mode\n");
	if (wifi_on(RTW_MODE_AP) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	3. Start AP with hidden SSID
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start AP with hidden SSID\n");
	ssid = "RTL_AP";
	rtw_security_t security_type = RTW_SECURITY_WPA2_AES_PSK;
	password = "";
	int channel = 6;
	if (wifi_start_ap_with_hidden_ssid(ssid, security_type, password, strlen(ssid), strlen(password), channel) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_start_ap_with_hidden_ssid failed\n");
		return;
	}

	/*********************************************************************************
	 *	4. Check AP running
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Check AP running\n");
	int timeout = 20;
	while (1)
	{
		char essid[33];
		if (wext_get_ssid(WLAN0_NAME, (unsigned char *)essid) > 0)
		{
			if (strcmp((const char *)essid, (const char *)ssid) == 0)
			{
				DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] %s started\n", ssid);
				break;
			}
		}
		if (timeout == 0)
		{
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: Start AP timeout\n");
			return;
		}
		vTaskDelay(1 * configTICK_RATE_HZ);
		timeout--;
	}

	/*********************************************************************************
	 *	5. Start DHCP server
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start DHCP server\n");
	// For more setting about DHCP, please reference fATWA in atcmd_wifi.c.
#if CONFIG_LWIP_LAYER
	dhcps_init(&xnetif[0]);
#endif
}

/**
 * @brief  Wi-Fi example for mode switch case 7: Mode switching between concurrent mode and STA.
 * @note  Process Flow:
 *		- Enable Wi-Fi with concurrent (STA + AP) mode
 *		  - Disable Wi-Fi
 *		  - Start AP
 *		  - Check AP running
 *		  - Start DHCP server
 *		  - Connect to AP using STA mode
 *		- Disable concurrent (STA + AP) mode and start STA mode
 */
static void mode_switch_7(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example mode switch case 7...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with concurrent (STA + AP) mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with concurrent (STA + AP) mode\n");

	/*********************************************************************************
	 *	1-1. Disable Wi-Fi
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi\n");
	wifi_off();
	vTaskDelay(20);

	/*********************************************************************************
	 *	1-2. Enable Wi-Fi with STA + AP mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi with STA + AP mode\n");
	if (wifi_on(RTW_MODE_STA_AP) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	1-3. Start AP
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start AP\n");
	ssid = "RTL_AP";
	rtw_security_t security_type = RTW_SECURITY_WPA2_AES_PSK;
	password = "";
	int channel = 6;
	if (wifi_start_ap(ssid, security_type, password, strlen(ssid), strlen(password), channel) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_start_ap failed\n");
		return;
	}

	/*********************************************************************************
	 *	1-4. Check AP running
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Check AP running\n");
	int timeout = 20;
	while (1)
	{
		char essid[33];
		if (wext_get_ssid(WLAN1_NAME, (unsigned char *)essid) > 0)
		{
			if (strcmp((const char *)essid, (const char *)ssid) == 0)
			{
				DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] %s started\n", ssid);
				break;
			}
		}
		if (timeout == 0)
		{
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: Start AP timeout\n");
			return;
		}
		vTaskDelay(1 * configTICK_RATE_HZ);
		timeout--;
	}

	/*********************************************************************************
	 *	1-5. Start DHCP server
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Start DHCP server\n");
#if CONFIG_LWIP_LAYER
	dhcps_init(&xnetif[1]);
	vTaskDelay(1000);
#endif

	/*********************************************************************************
	 *	1-6. Connect to AP using STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP\n");
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

	/*********************************************************************************
	 *	2. Disable concurrent (STA + AP) mode and start STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable concurrent (STA + AP) mode and start STA mode\n");
	wifi_off();
	vTaskDelay(20);

	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);
}

/**
 * @brief  Wi-Fi example for scenario case 1.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Connect to AP by WPS enrollee static PIN mode (If failed, re-connect one time.)
 *		- Enable Wi-Fi Direct GO
 *			(It will re-enable WiFi, the original connection to AP would be broken.)
 */
static void scenario_1(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 1...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

#if defined(CONFIG_ENABLE_WPS) && (CONFIG_ENABLE_WPS)
	/*********************************************************************************
	 *	2. Connect to AP by WPS enrollee PIN mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP by WPS enrollee PIN mode with PIN code: 92402508\n");
	// Start static WPS-PIN enrollee with PIN code: 92402508.
	// As the process beginning, please enter the PIN code in AP side.
	// It will take at most 2 min to do the procedure.
	char *argv[3];
	argv[1] = "pin";
	argv[2] = "92402508";
	cmd_wps(3, argv);

	// If not connected, retry one time.
	if (wifi_is_connected_to_ap() != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] WPS enrollee failed, reconnect one time\n");
		cmd_wps(3, argv);
	}
#else
	DiagPrintf("Please set CONFIG_ENABLE_WPS 1 in platform_opts.h to enable WPS\n");
#endif
#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	/*********************************************************************************
	 *	3. Enable Wi-Fi Direct GO
	 *********************************************************************************/
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

/**
 * @brief  Wi-Fi example for scenario case 2.
 * @note  Process Flow:
 *		- Enable Wi-Fi Direct GO
 *		- Disable Wi-Fi Direct GO, and enable Wi-Fi with STA mode
 *			(Disable Wi-Fi Direct GO must be done to release P2P resource.)
 *		- Connect to AP by WPS enrollee PBC mode (If failed, re-connect one time.)
 */
static void scenario_2(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 2...\n");
#if defined(CONFIG_ENABLE_P2P) && (CONFIG_ENABLE_P2P)
	/*********************************************************************************
	 *	1. Enable Wi-Fi Direct GO
	 *********************************************************************************/
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

	/*********************************************************************************
	 *	2. Disable Wi-Fi Direct GO, and enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi Direct GO, and enable Wi-Fi\n");
	// Disable Wi-Fi Direct GO.
	// This command has to be invoked to release the P2P resource.
	cmd_wifi_p2p_stop(NULL, NULL);
	// Enable Wi-Fi with STA mode.
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on() failed\n");
		return;
	}
#else
	DiagPrintf("Please set CONFIG_ENABLE_P2P 1 in platform_opts.h to enable P2P\n");
#endif
#if defined(CONFIG_ENABLE_WPS) && (CONFIG_ENABLE_WPS)
	/*********************************************************************************
	 *	3. Connect to AP by WPS enrollee PBC mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP by WPS enrollee PBC mode\n");
	// Start WPS-PBC enrollee.
	// As the process beginning, please push the WPS button on AP.
	// It will take at most 2 min to do the procedure.
	char *argv[2];
	argv[1] = "pbc";
	cmd_wps(2, argv);

	// If not connected, retry one time.
	if (wifi_is_connected_to_ap() != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] WPS enrollee failed, reconnect one time\n");
		cmd_wps(2, argv);
	}
#else
	DiagPrintf("Please set CONFIG_ENABLE_WPS 1 in platform_opts.h to enable WPS\n");
#endif
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
void scenario_3(void)
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
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
	{
		// LwIP_DHCP(0, DHCP_START);
		MQTTClient client;
		Network network;
		static unsigned char sendbuf[MQTT_SENDBUF_LEN], readbuf[MQTT_READBUF_LEN];
		int rc = 0, mqtt_pub_count = 0;
		MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
		connectData.MQTTVersion = 3;
		connectData.clientID.cstring = "FT1_018";
		connectData.username.cstring = "VBeeHome";
		connectData.password.cstring = "123abcA@!";
		char *address = "61.28.238.97";
		char *sub_topic = "LASS/Test/Pm25Ameba/#";
		char *pub_topic = "LASS/Test/Pm25Ameba/FT1_018";

		NetworkInit(&network);
		MQTTClientInit(&client, &network, 30000, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

		while (1)
		{
			// while (wifi_is_ready_to_transceive(RTW_STA_INTERFACE) != RTW_SUCCESS)
			// {
			// 	mqtt_printf(MQTT_INFO, "Wait Wi-Fi to be connected.");
			// 	vTaskDelay(5000 / portTICK_PERIOD_MS);
			// }

			fd_set read_fds;
			fd_set except_fds;
			struct timeval timeout;

			FD_ZERO(&read_fds);
			FD_ZERO(&except_fds);
			timeout.tv_sec = MQTT_SELECT_TIMEOUT;
			timeout.tv_usec = 0;

			if (network.my_socket >= 0)
			{
				FD_SET(network.my_socket, &read_fds);
				FD_SET(network.my_socket, &except_fds);
				rc = FreeRTOS_Select(network.my_socket + 1, &read_fds, NULL, &except_fds, &timeout);

				if (FD_ISSET(network.my_socket, &except_fds))
				{
					mqtt_printf(MQTT_INFO, "except_fds is set");
					MQTTSetStatus(&client, MQTT_START); // my_socket will be close and reopen in MQTTDataHandle if STATUS set to MQTT_START
				}
				else if (rc == 0) // select timeout
				{
					if (++mqtt_pub_count == 5) // Send MQTT publish message every 5 seconds
					{
						MQTTPublishMessage(&client, pub_topic);
						mqtt_pub_count = 0;
					}
				}
			}
			MQTTDataHandle(&client, &read_fds, &connectData, messageArrived, address, sub_topic);
		}
		flag_connected = 1;
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Successful connected scenario 3!\n");
	}

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

/**
 * @brief  Wi-Fi example for scenario case 4.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Connect to AP by WPS enrollee PBC mode (If failed, re-connect one time.)
 *		- Disconnect from AP
 *		- Enable Wi-Fi Direct GO
 *		- Disable Wi-Fi Direct GO, and enable Wi-Fi with STA mode
 *			(Disable Wi-Fi Direct GO must be done to release P2P resource.)
 *		- Connect to AP use STA mode (If failed, re-connect one time.)
 *		- Disconnect from AP
 *		- Disable Wi-Fi
 */
static void scenario_4(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 4...\n");

	/**********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

#if defined(CONFIG_ENABLE_WPS) && (CONFIG_ENABLE_WPS)
	/**********************************************************************************
	 *	2. Connect to AP by WPS enrollee PBC mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP by WPS enrollee PBC mode\n");
	// Start WPS-PBC enrollee.
	// As the process beginning, please push the WPS button on AP.
	// It will take at most 2 min to do the procedure.
	char *argv[2];
	argv[1] = "pbc";
	cmd_wps(2, argv);

	// If not connected, retry one time.
	if (wifi_is_connected_to_ap() != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] WPS enrollee failed, reconnect one time\n");
		cmd_wps(2, argv);
	}
#else
	DiagPrintf("Please set CONFIG_ENABLE_WPS 1 in platform_opts.h to enable WPS\n");
#endif

	/**********************************************************************************
	 *	3. Disconnect from AP
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disconnect from AP\n");
	if (wifi_disconnect() < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_disconnect() failed\n");
		return;
	}

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

	/**********************************************************************************
	 *	5. Disable Wi-Fi Direct GO, and enable Wi-Fi with STA mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi Direct GO, and enable Wi-Fi\n");
	// Disable Wi-Fi Direct GO.
	// This command has to be invoked to release the P2P resource.
	cmd_wifi_p2p_stop(NULL, NULL);
#else
	DiagPrintf("Please set CONFIG_ENABLE_P2P 1 in platform_opts.h to enable P2P\n");
#endif
	// Enable Wi-Fi on STA mode.
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on() failed\n");
		return;
	}

	/**********************************************************************************
	 *	6. Connect to AP use STA mode
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP use STA mode\n");

	// Set the auto-reconnect mode with retry 1 time(limit is 2) and timeout 5 seconds.
	// This command need to be set before invoke wifi_connect() to make reconnection work.
	wifi_config_autoreconnect(1, 2, 5);

	// Connect to AP with Open mode.
	ssid = "Viethoang";
	if (wifi_connect(ssid, RTW_SECURITY_OPEN, NULL, strlen(ssid), 0, -1, NULL) == RTW_SUCCESS)
		LwIP_DHCP(0, DHCP_START);

	// Show Wi-Fi info.
	wifi_get_setting(WLAN0_NAME, &setting);
	wifi_show_setting(WLAN0_NAME, &setting);

	/**********************************************************************************
	 *	7. Disconnect from AP
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disconnect from AP\n");
	if (wifi_disconnect() < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_disconnect() failed\n");
		return;
	}

	// Show Wi-Fi info.
	wifi_get_setting(WLAN0_NAME, &setting);
	wifi_show_setting(WLAN0_NAME, &setting);

	/**********************************************************************************
	 *	8. Disable Wi-Fi
	 **********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Disable Wi-Fi\n");
	if (wifi_off() != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_off() failed\n");
		return;
	}
}

/**
 * @brief  Wi-Fi example for scenario case 5.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Connect to AP using STA mode, check the connection result based on error_flag
 *		- Show Wi-Fi information
 *		- Get AP's RSSI
 */
static void scenario_5(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 5...\n");

	/*********************************************************************************
	 *	1. Enable Wi-Fi with STA mode
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Enable Wi-Fi\n");
	if (wifi_on(RTW_MODE_STA) < 0)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_on failed\n");
		return;
	}

	/*********************************************************************************
	 *	2. Connect to AP using STA mode, check the connection result
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connect to AP using STA mode\n");
	ssid = "Viethoang";
	password = "12345678";
	if (wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password, strlen(ssid), strlen(password), -1, NULL) == RTW_SUCCESS)
	{
		LwIP_DHCP(0, DHCP_START);
		flag_connected = 1;
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Successful connected scenario 5!\n");
		// DHCP success
		if (error_flag == RTW_NO_ERROR)
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] DHCP success\n");
		// DHCP fail, might caused by timeout or the AP did not enable DHCP server
		else if (error_flag == RTW_DHCP_FAIL)
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] DHCP fail, might caused by timeout or the AP did not enable DHCP server\n");
	}
	else
	{
		switch (error_flag)
		{
		// Cannot scan AP
		case RTW_NONE_NETWORK:
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Cannot scan AP: %s\n", ssid);
			break;
		// Connection fail caused by auth/assoc failure
		// E.g. if power off the AP during the connection would tigger this scenario
		case RTW_CONNECT_FAIL:
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Connection fail caused by auth/assoc failure. Please check the AP and connect again\n");
			break;
		// Password length incorrect or not the same password as AP used
		case RTW_WRONG_PASSWORD:
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Password length incorrect or not the same password as AP used\n");
			break;
		// Unexpected error
		default:
			DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Unexpected connection failure\n");
		}
	}

	/*********************************************************************************
	 *	3. Show Wi-Fi information
	 *********************************************************************************/
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Show Wi-Fi information\n");
	rtw_wifi_setting_t setting;
	wifi_get_setting(WLAN0_NAME, &setting);
	wifi_show_setting(WLAN0_NAME, &setting);

	/*********************************************************************************
	 *	4. Get AP's RSSI
	 *********************************************************************************/
	int rssi = 0;
	wifi_get_rssi(&rssi);
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Get AP RSSI: %d\n", rssi);
}

/**
 * @brief  Wi-Fi example for scenario case 6.
 * @note  Process Flow:
 *		- Enable Wi-Fi with STA mode
 *		- Scan network and handle the RSSI value (in dBm)
 */
static void scenario_6(void)
{
	DiagPrintf("\n\n[WLAN_SCENARIO_EXAMPLE] Wi-Fi example scenario case 6...\n");

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
	 *	2. Scan network and handle the RSSI value
	 **********************************************************************************/
	// Scan network and print the RSSI & SSID out.
	// Can refer to fATWS() in atcmd_wifi.c and scan_result_RSSI_handler() below.
	DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] Scan network\n");
	if (wifi_scan_networks(scan_result_RSSI_handler, NULL) != RTW_SUCCESS)
	{
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: wifi_scan_networks() failed\n");
		return;
	}
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

// For processing the scanned result -> output RSSI & SSID.
// Can refer to fATWS() in atcmd_wifi.c.
static rtw_result_t scan_result_RSSI_handler(rtw_scan_handler_result_t *malloced_scan_result)
{
	static int ApNum = 0;
	if (malloced_scan_result->scan_complete != RTW_TRUE)
	{
		rtw_scan_result_t *record = &malloced_scan_result->ap_details;
		record->SSID.val[record->SSID.len] = 0; /* Ensure the SSID is null terminated */

		DiagPrintf("%d\t", ++ApNum);
		DiagPrintf(" RSSI: %d\t", record->signal_strength);
		DiagPrintf(" SSID: %s", record->SSID.val);
		DiagPrintf("\r\n");
	}
	return RTW_SUCCESS;
}

static void example_wlan_scenario_thread(void *in_id)
{
	char *id = in_id;

	// Wait for other task stable.
	vTaskDelay(4000);
	scenario_3();
#if (0)
	if (strcmp(id, "S") == 0)
	{
		// Scan network.
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] SCAN WIFI\n");
		scan_network();
	}

	else if (strcmp(id, "A") == 0)
		// Authentication example.
		authentication();
	else if (strcmp(id, "M1") == 0)
		// Mode switch case 1.
		mode_switch_1();
	else if (strcmp(id, "M2") == 0)
		// Mode switch case 2.
		mode_switch_2();
	else if (strcmp(id, "M3") == 0)
		// Mode switch case 3.
		mode_switch_3();
	else if (strcmp(id, "M4") == 0)
		// Mode switch case 4.
		mode_switch_4();
	else if (strcmp(id, "M5") == 0)
		// Mode switch case 5.
		mode_switch_5();
	else if (strcmp(id, "M6") == 0)
		// Mode switch case 6.
		mode_switch_6();
	else if (strcmp(id, "M7") == 0)
		// Mode switch case 7.
		mode_switch_7();
	else if (strcmp(id, "S1") == 0)
		// Scenario case 1.
		scenario_1();
	else if (strcmp(id, "S2") == 0)
		// Scenario case 2.
		scenario_2();
	else if (strcmp(id, "S3") == 0)
	// Scenario case 3.
	{
		// Scan network.
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] SCENARIO 3\n");
		scenario_3();
	}

	else if (strcmp(id, "S4") == 0)
		// Scenario case 4.
		scenario_4();
	else if (strcmp(id, "S5") == 0)
		// Scenario case 5.
		scenario_5();
	else if (strcmp(id, "S6") == 0)
		// Scenario case 6.
		scenario_6();
	else
		DiagPrintf("\n\r[WLAN_SCENARIO_EXAMPLE] ERROR: Invalid case identity\n");
#endif
	vTaskDelete(NULL);
}

void example_wlan_scenario(char *id)
{
	if (xTaskCreate(example_wlan_scenario_thread, ((const char *)"example_wlan_scenario_thread"), 1024, (void *const)id, tskIDLE_PRIORITY + 1, &xHandle) != pdPASS)
		DiagPrintf("\n\r%s xTaskCreate failed\n", __FUNCTION__);
}

#endif /* CONFIG_EXAMPLE_WLAN_SCENARIO */

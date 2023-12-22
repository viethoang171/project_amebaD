#include <platform_opts.h>
#include "FreeRTOS.h"
#include "task.h"
#include <platform/platform_stdlib.h>

#include "device.h"
#include "gpio_api.h" // mbed
#include <example_wlan_scenario.h>
#include <example_mqtt.h>

#define GPIO_LED_PIN PA_14

gpio_t gpio_led;
extern bool flag_connected;
TaskHandle_t xHandle;
static bool flag = 0;
static void check_task()
{
    for (;;)
    {
        if (flag == 0 && xHandle == NULL)
        {
            flag = 1;
            xTaskCreate(prvMQTTEchoTask, /* The function that implements the task. */
                        "MQTTEcho0",     /* Just a text name for the task to aid debugging. */
                        16384,           /* The stack size is defined in FreeRTOSIPConfig.h. */
                        NULL,            /* The task parameter, not used in this case. */
                        6,               /* The priority assigned to the task is defined in FreeRTOSConfig.h. */
                        NULL);           /* The task handle is not used. */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void main()
{
    // Init LED control pin
    gpio_init(&gpio_led, GPIO_LED_PIN);
    gpio_dir(&gpio_led, PIN_OUTPUT); // Direction: Output
    gpio_mode(&gpio_led, PullNone);  // No pull

    // xTaskCreate(check_task, "check_task", 1024, NULL, 8, NULL);
    example_wlan_scenario("S3");

    vTaskStartScheduler();
}
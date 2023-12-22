#include <platform_opts.h>
#include "FreeRTOS.h"
#include "task.h"
static void love_you()
{
    for (;;)
    {
        DiagPrintf("\n\rHu hu I'm so sorry\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        DiagPrintf("\n\rI have something for you! Look!!!\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        DiagPrintf("\n\rSorry, this screen only supports English\n");
        for (int i = 1; i <= 10; i++)
        {
            DiagPrintf(".");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        DiagPrintf("\n\r ******           ***  ***        ***   ***\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        DiagPrintf("  \r   **            **********       ***   ***\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        DiagPrintf("  \r   **            **********       ***   ***\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        DiagPrintf("  \r   **              ******         ***   ***\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        DiagPrintf("  \r ******             ***            *******\n");
        DiagPrintf("\n\r#yeuHoaiThunhieulam\n");
        vTaskDelete(NULL);
    }
}
int main()
{
    xTaskCreate(love_you,   /* The function that implements the task. */
                "love_you", /* Just a text name for the task to aid debugging. */
                1024,       /* The stack size is defined in FreeRTOSIPConfig.h. */
                NULL,       /* The task parameter, not used in this case. */
                7,          /* The priority assigned to the task is defined in FreeRTOSConfig.h. */
                NULL);      /* The task handle is not used. */
    vTaskStartScheduler();
    return 0;
}
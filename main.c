#include <stdio.h>
#include <stdbool.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#ifdef RUN_FS_TESTS
#include "fs_tests.h"
#endif

const uint LED_PIN = 25;

int main(void) {
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    sleep_ms(2500);

#ifdef RUN_FS_TESTS
    (void)fs_run_tests();
#endif

    uint32_t heartbeat = 0u;
    while (true) {
        gpio_put(LED_PIN, 1);
        sleep_ms(250);
        gpio_put(LED_PIN, 0);
        sleep_ms(750);

        ++heartbeat;
        if ((heartbeat % 5u) == 0u) {
            printf("FS_TEST_IDLE\n");
        }
    }
}

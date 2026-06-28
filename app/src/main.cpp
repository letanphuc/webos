#include <zephyr/kernel.h>

int main(void)
{
	printk("WebOS hello from Zephyr on ESP32-S3\n");

	while (1) {
		printk("WebOS heartbeat\n");
		k_sleep(K_SECONDS(1));
	}
}

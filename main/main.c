#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "mdns.h"

#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_sleep.h"

#include "lwip/err.h"
#include "lwip/sys.h"

RTC_DATA_ATTR int bootCount = 0;

/* The examples use WiFi configuration that you can set via project configuration menu
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "first-test";

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

//FIXME
#define LWIP_LOCAL_HOSTNAME "esp32player1"

//#define GO_SLEEP_URL "http://nuc.lan/api/bose/Bose-Bureau/notify/shut"
//#define MOVEMENT_URL "http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/Mais%20qui%20ose%20passer%20devant%20moi"
#define MOVEMENT_URL "http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/H%C3%A9%20h%C3%A9%2C%20je%20vous%20ai%20vu%20%21"
//#define HTTP_URL "http://nuc.lan/api/bose/ALL/notify/shut"
#define PING_URL "http://nuc.lan/ping"

// GPIO 0 (where BOOT Button is plugged)
#define GPIO_BUTTON_BOOT     0 	

// GPIO 32 (where to plug a PIR: passive infra red detector, XXX: I choose 32 because it allow device wake up notification)
#define GPIO_MOVEMENT_DETECTOR     32
#define GPIO_INPUT_PIN_SEL  (1ULL<<GPIO_BUTTON_BOOT | 1ULL<<GPIO_MOVEMENT_DETECTOR)

//#define GPIO_OUTPUT_IO_0     0
//#define GPIO_OUTPUT_PIN_SEL  (1ULL<<GPIO_OUTPUT_IO_0)

#define ESP_INTR_FLAG_DEFAULT 0

#define MAX_HTTP_OUTPUT_BUFFER 2048

static xQueueHandle gpio_evt_queue = NULL;
time_t last_alarm = 0; 
// wait ALARM_TRIGGER second before sending a new one
#define ALARM_FLODDING_PROTECTION 30 
#define WATCHDOG_PING 3600

void start_mdns_service()
{
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    //set hostname
    mdns_hostname_set( LWIP_LOCAL_HOSTNAME);
    //set default instance
    mdns_instance_name_set("Ed's ESP32");
}

void http_request(const char* url)
{
    //char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */

    esp_http_client_config_t config = {
        .url = url,
        //.event_handler = _http_event_handler,
        //.user_data = local_response_buffer,        // Pass address of local buffer to get response
    };
    esp_http_client_handle_t client = esp_http_client_init( &config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    //ESP_LOG_BUFFER_HEX(TAG, local_response_buffer, strlen(local_response_buffer));
    
    esp_http_client_cleanup(client);
}

void got_movement() {
	    time_t now; 
	    time( &now);

	    if ((last_alarm == 0) || ((now - last_alarm) > ALARM_FLODDING_PROTECTION)) {
		printf("time to launch alarm\n");
		last_alarm = now; 
		http_request( MOVEMENT_URL);
	    }

}
void go_to_bed() {
	    printf( "Stopping wifi\n");
	    esp_wifi_stop();
	    esp_wifi_deinit(); // stop wifi

	    printf( "Starting low consumption mode (deep sleep)\n");

	    
	    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

	    //or will wake up after to start a ping (watchdog)
	    esp_sleep_enable_timer_wakeup( (uint64_t)WATCHDOG_PING * (uint64_t)1000000); 

	    //Warning: can only take in consideration RTC GPIO: 0,2,4,12-15,25-27,32-39
	    //esp_sleep_enable_ext1_wakeup( 1ULL<<GPIO_BUTTON_BOOT, ESP_EXT1_WAKEUP_ALL_LOW);
	    
	    //will wake up on any movement (GPIO going UP)
	    esp_sleep_enable_ext1_wakeup( 1ULL<<GPIO_MOVEMENT_DETECTOR, ESP_EXT1_WAKEUP_ANY_HIGH);
	    esp_sleep_enable_gpio_wakeup();

	    esp_deep_sleep_start(); //deep sleep

}
void ping() {
	printf("ping\n");
	http_request( PING_URL);
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {

	    uint32_t level = gpio_get_level( io_num);
            printf("debug: GPIO[%d] intr, val: %d\n", io_num, level);

	    switch ( io_num) {

		case GPIO_BUTTON_BOOT:
		    printf( "Button BOOT released\n");
		    ping();
		    go_to_bed();
		    break;

		case GPIO_MOVEMENT_DETECTOR:
		    printf( "Got movement\n");
		    got_movement();
		    break;

		default:
		    printf("Strange case, should not occurs\n");

	    }

        }
    }
}


static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

	start_mdns_service();
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, "...");
                 //EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, "...");
                 //EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void init_gpio(void)
{

    gpio_config_t io_conf;

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

     //change gpio intrrupt type for one pin
     // GPIO_INTR_POSEDGE  = rising up, GPIO_INTR_NEGEDGE = falling down, GPIO_INTR_ANYEDGE = both
    gpio_set_intr_type(GPIO_BUTTON_BOOT, GPIO_INTR_POSEDGE);       // on rising up = when button is released
    gpio_set_intr_type(GPIO_MOVEMENT_DETECTOR, GPIO_INTR_POSEDGE); // when movement is detected

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_BUTTON_BOOT, gpio_isr_handler, (void*) GPIO_BUTTON_BOOT);
    gpio_isr_handler_add(GPIO_MOVEMENT_DETECTOR, gpio_isr_handler, (void*) GPIO_MOVEMENT_DETECTOR);

}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();


    init_gpio();

    printf("Boot number: %d\n", ++bootCount);


    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {

        case ESP_SLEEP_WAKEUP_UNDEFINED:
	    printf("normal startup\n");
	    break;

	case ESP_SLEEP_WAKEUP_EXT1:
	    printf("wakeup from EXT1, meaning movement, calling web hook\n");
    	    got_movement();
	    break;

	case ESP_SLEEP_WAKEUP_TIMER:
	    printf("wakeup from timer, calling ping\n");
	    ping();
	    break;

        default:
	    printf("Strange, got another wakup cause: %d\n", cause);
	    break;

    }

    time_t now; 

    int cnt = 0;

    while(1) {
	time( &now);
        printf("cnt: %d at %ld s\n", ++cnt, now);
        vTaskDelay(10000 / portTICK_RATE_MS);

	if (cnt > 6) {
		//after 1min, go in low consumption
		go_to_bed();
	}
    }
}

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
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include "driver/gpio.h"

#include "esp_netif.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "parse_config.h"

// ---------------------------------------------------------------------------------------------------------------

static const char *TAG = "first-test";

static char *movement_url = NULL;

#define LWIP_LOCAL_HOSTNAME "esp32player1"

//#define GO_SLEEP_URL "http://nuc.lan/api/bose/Bose-Bureau/notify/shut"
//#define MOVEMENT_URL "http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/Mais%20qui%20ose%20passer%20devant%20moi"
//#define MOVEMENT_URL "http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/H%C3%A9%20h%C3%A9%2C%20je%20vous%20ai%20vu%20%21"
//#define HTTP_URL "http://nuc.lan/api/bose/ALL/notify/shut"

//FIXME: move it to config from HTTP ?
#define PING_URL "http://nuc.lan/ping"

//FIXME: find a smarter way (from DHCP ?, from menuconfig ?)
#define CONFIG_URL "http://nuc.lan/provisionning/first-test.conf"

// GPIO 0 (where BOOT Button is plugged)
#define GPIO_BUTTON_BOOT     0 	

// GPIO 32 (where to plug a PIR: passive infra red detector, XXX: I choose 32 because it allow device wake up notification)
#define GPIO_MOVEMENT_DETECTOR     32





#define BUFFSIZE 1024

//  ----------------------------------------------------------------------------------- helpers

#define MAX_HTTP_RECV_BUFFER 512

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d (full size: %d)", evt->data_len, esp_http_client_get_content_length(evt->client));
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (output_buffer == NULL) {
                    output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client)+1);
                    output_len = 0;
                    if (output_buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
		    if (evt->user_data) {
		    	*((char **)evt->user_data) = output_buffer;
		    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {

                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
		output_buffer[output_len]='\0'; //close buffer, consider it as a string

		if ( evt->user_data == NULL ) {
			evt->user_data = output_buffer;
		}
                //ESP_LOGD(TAG, "answer: %s", output_buffer);

	        if (evt->user_data == NULL) {
                	free(output_buffer); 
		}
                output_buffer = NULL;
                output_len = 0;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                if (output_buffer != NULL) {
                    free(output_buffer);
                    output_buffer = NULL;
                    output_len = 0;
                }
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

//FIXME: find a way to output buffer
static int lazy_http_request(const char* url, http_event_handle_cb event_handler, char **buffer)
{
    int status = 900;

    ESP_LOGI(TAG, "HTTP GET start query %s", url);
    esp_http_client_config_t config = {
        .url           = url,
	.event_handler = event_handler,
	.user_data     = buffer 
    };
    esp_http_client_handle_t client = esp_http_client_init( &config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
	status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));

	
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    	
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return status;
}


// -------------------------------------------------------------------------------- OTA functions

/*an ota data write buffer ready to write to the flash*/
static char ota_write_data[BUFFSIZE + 1] = { 0 };
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

#define OTA_URL_SIZE 256

#define HASH_LEN 32 /* SHA-256 digest length */
static void print_sha256 (const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}

static void __attribute__((noreturn)) ota_task_fatal_error(void)
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    (void)vTaskDelete(NULL);

    while (1) {
        ;
    }
}

static void __attribute__((noreturn)) ota_stop_task(void)
{
    ESP_LOGI(TAG, "When a new firmware is available on the server, press the reset button to download it");
    (void)vTaskDelete(NULL);

    while(1) {
	    ;
    }
}

static void ota_task(void *pvParameter)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    esp_http_client_config_t config = {
        .url = CONFIG_ESP_FIRMWARE_UPG_URL,
        .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = CONFIG_ESP_OTA_RECV_TIMEOUT,
    };

//#ifdef CONFIG_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
//#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        ota_task_fatal_error();
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        ota_task_fatal_error();
    }
    esp_http_client_fetch_headers(client);

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);

    int binary_file_length = 0;
    /*deal with all receive packet*/
    bool image_header_was_checked = false;
    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            ota_task_fatal_error();
        } else if (data_read > 0) {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    // check current version with downloading
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                            ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                            http_cleanup(client);
                            ota_stop_task();
                        }
                    }
#ifndef CONFIG_SKIP_VERSION_CHECK
                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
                        ESP_LOGI(TAG, "Current running version is the same as a new. We will not continue the update.");
                        http_cleanup(client);
                        ota_stop_task();
                    }
#endif

                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        http_cleanup(client);
                        ota_task_fatal_error();
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                } else {
                    ESP_LOGE(TAG, "received package is not fit len");
                    http_cleanup(client);
                    ota_task_fatal_error();
                }
            }
            err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                http_cleanup(client);
                ota_task_fatal_error();
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
           /*
            * As esp_http_client_read never returns negative error code, we rely on
            * `errno` to check for underlying transport connectivity closure if any
            */
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                break;
            }
            if (esp_http_client_is_complete_data_received(client) == true) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);
    if (esp_http_client_is_complete_data_received(client) != true) {
        ESP_LOGE(TAG, "Error in receiving complete file");
        http_cleanup(client);
        ota_task_fatal_error();
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        ota_task_fatal_error();
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        ota_task_fatal_error();
    }
    ESP_LOGI(TAG, "Prepare to restart system!");

    fflush(stdout);
    esp_restart();
    return ;
}

/*
 * called after OTA image loaded at first boot to validate new image (will rollback otherwise)
 */
static bool image_diagnostic(void)
{
    ESP_LOGI(TAG, "Diagnostics (return true, no test for now)");
    return 1;
	/*
    gpio_config_t io_conf;
    io_conf.intr_type    = GPIO_PIN_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << CONFIG_GPIO_DIAGNOSTIC);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Diagnostics (5 sec)...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    bool diagnostic_is_ok = gpio_get_level(CONFIG_GPIO_DIAGNOSTIC);

    gpio_reset_pin(CONFIG_GPIO_DIAGNOSTIC);
    return diagnostic_is_ok;
    */
}

static void check_image() 
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // get sha256 digest for the partition table
    partition.address   = ESP_PARTITION_TABLE_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type      = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // run diagnostic function ...
            bool diagnostic_is_ok = image_diagnostic();
            if (diagnostic_is_ok) {
                ESP_LOGI(TAG, "Diagnostics completed successfully! Continuing execution ...");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE(TAG, "Diagnostics failed! Start rollback to the previous version ...");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
}

// -------------------------------------------------------------------------- wifi

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_stop() {
	esp_wifi_stop();
	esp_wifi_deinit();
}


void wifi_start(void)
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
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
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
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", CONFIG_ESP_WIFI_SSID, "...");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", CONFIG_ESP_WIFI_SSID, "...");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

// -------------------------------------------------------------------------- main functions

time_t last_alarm = 0; 

void got_movement() {
	    time_t now; 
	    time( &now);
	    ESP_LOGI( TAG, "Movement detected");

	    if ((last_alarm == 0) || ((now - last_alarm) > CONFIG_ESP_ALARM_FLODDING_PROTECTION)) {
		last_alarm = now; 
		if (movement_url != NULL) {
			ESP_LOGI( TAG, "Movement: call webhook");
			lazy_http_request( movement_url, NULL, NULL);
		}
		else {
			ESP_LOGW( TAG, "Movement: webhook not defined !");
		}
	    }

}

void go_to_bed() {

	ESP_LOGI( TAG, "Starting low consumption mode (deep sleep)");

	//release Wifi driver
	wifi_stop();

	esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

	//scheddule a wake up for watchdog + periodic check
	esp_sleep_enable_timer_wakeup( (uint64_t)CONFIG_ESP_WATCHDOG_WAKEUP * (uint64_t)1000000); 

	//Warning: can only take in consideration RTC GPIO: 0,2,4,12-15,25-27,32-39
	//esp_sleep_enable_ext1_wakeup( 1ULL<<GPIO_BUTTON_BOOT, ESP_EXT1_WAKEUP_ALL_LOW);

	//will wake up on any movement (GPIO going UP)
	esp_sleep_enable_ext1_wakeup( 1ULL<<GPIO_MOVEMENT_DETECTOR, ESP_EXT1_WAKEUP_ANY_HIGH);
	esp_sleep_enable_gpio_wakeup();


	ESP_LOGI( TAG, "See you on next wake-up");
    	fflush(stdout);

	esp_deep_sleep_start(); //deep sleep

}

void ping() {
	ESP_LOGI( TAG, "ping (calling webhook)");
	lazy_http_request( PING_URL, _http_event_handler, NULL);
}


//XXX Take care that keys must be < 16 chars due to NVS restriction
struct_pc_keymap config_definition[]= {
	{ .name="movement_url", .target=&movement_url, .type=TYPE_STRING, .defined=0, .mandatory=1  },
	//{ .name="watchdog_wakeup",  .target=&watchdog_wakeup,  .type=TYPE_INT,    .defined=0, .mandatory=0  },
	{ .name=NULL,                                                                                      } //mark end of array
};


void read_config() {

	char *config_buffer = NULL;

	//FIXME: free buffers if was already defined

	//FIXME: chould use FAT partition to store config rather NVS which is not recommended for strings, will be simplier

	nvs_handle_t nvs_handle;
    	esp_err_t ret;

	if (nvs_open(CONFIG_ESP_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
		ESP_LOGW(TAG, "Enable to open NVS %s", CONFIG_ESP_STORAGE_NAMESPACE);
	}

	int status = lazy_http_request( CONFIG_URL, _http_event_handler, &config_buffer);
	if( (status>=200) && (status<300) && (config_buffer != NULL)) {
		//got config  buffer from http request
		ESP_LOGD( TAG, "config from http: %s", config_buffer);

		//parse it
		//FIXME: read status
		parse_config_buffer( config_definition, config_buffer);
		free( config_buffer);

		//store values to NVS
		for (int i=0; config_definition[i].name != NULL; i++) {
			ret = ESP_FAIL;
			switch( config_definition[i].type) {

				case TYPE_STRING:
					ret = nvs_set_str(nvs_handle, config_definition[i].name, *((char **)( config_definition[i].target)) );
					break;

				case TYPE_INT:
					ret = nvs_set_i64(nvs_handle, config_definition[i].name, *((int64_t *)( config_definition[i].target)) );
					break;
			
			}
			if (ret != ESP_OK) {
				ESP_LOGW(TAG, "unable to write %s key from NVS: %s", config_definition[i].name, esp_err_to_name( ret));
			}
		}
		
		if (nvs_commit( nvs_handle) != ESP_OK) {
			ESP_LOGW(TAG, "unable to commit NVS");
		}

	}
	else {
		ESP_LOGW( TAG, "config from http failed, try to fallback to NVS");

		//try to read previous values from NVS
		size_t required_size;

		//store values to NVS
		for (int i=0; config_definition[i].name != NULL; i++) {
			ret = ESP_FAIL;

			switch( config_definition[i].type) {

				case TYPE_STRING:
					ret = nvs_get_str(nvs_handle, config_definition[i].name, NULL, &required_size );
					if (ret != ESP_OK) break;
					char *buffer = malloc(required_size);
					if (buffer == NULL) { ret = ESP_ERR_NO_MEM; break; }
					ret = nvs_get_str(nvs_handle, config_definition[i].name, buffer, &required_size);
					if (ret == ESP_OK) {
						//point target to buffer
						*((char **) (config_definition[i].target)) = buffer;
					}
					break;

				case TYPE_INT:
					ret = nvs_get_i64(nvs_handle, config_definition[i].name, (int64_t *)( config_definition[i].target) );
					break;
			
			}
			if (ret != ESP_OK) {
				ESP_LOGW(TAG, "unable to read %s key from NVS: %s", config_definition[i].name, esp_err_to_name( ret));
			}
		}
	}

	nvs_close( nvs_handle);

	if (movement_url !=  NULL) {
		ESP_LOGI( TAG, "movement_url set to: %s", movement_url);
	}
}
// -------------------------------------------------------------------------------- gpio


#define GPIO_INPUT_PIN_SEL  (1ULL<<GPIO_BUTTON_BOOT | 1ULL<<GPIO_MOVEMENT_DETECTOR)
#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task(void* arg)
{
    uint32_t io_num;
    for(;;) {

        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {

	    //uint32_t level = gpio_get_level( io_num);
            //ESP_LOGI(TAG, "debug: GPIO[%d] intr, val: %d", io_num, level);

	    switch ( io_num) {

		case GPIO_BUTTON_BOOT:
		    ESP_LOGI( TAG, "Button BOOT released");
		    ping();
		    //go_to_bed();
		    break;

		case GPIO_MOVEMENT_DETECTOR:
		    ESP_LOGI( TAG, "Got movement");
		    got_movement();
		    break;

		default:
		    ESP_LOGI( TAG, "Strange case: got unexpected notification on GPIO %d", io_num);

	    }

        }
    }
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
    xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_BUTTON_BOOT, gpio_isr_handler, (void*) GPIO_BUTTON_BOOT);
    gpio_isr_handler_add(GPIO_MOVEMENT_DETECTOR, gpio_isr_handler, (void*) GPIO_MOVEMENT_DETECTOR);

}


// -------------------------------------------------------------------------- app_main

RTC_DATA_ATTR int bootCount = 0;

void app_main(void)
{

	esp_log_level_set("dhcpc", ESP_LOG_DEBUG); 
	esp_log_level_set(TAG, ESP_LOG_INFO); 
    //check image
    check_image();

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //Initialize Wifi
    wifi_start();

    //Initialize GPIO
    init_gpio();

    ESP_LOGI( TAG, "Boot number: %d", ++bootCount);


    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {

        case ESP_SLEEP_WAKEUP_UNDEFINED:
	    ESP_LOGI( TAG, "normal startup");
	    break;

	case ESP_SLEEP_WAKEUP_EXT1:
	    ESP_LOGI( TAG, "wakeup from EXT1, meaning movement, calling web hook");
    	    got_movement();
	    break;

	case ESP_SLEEP_WAKEUP_TIMER:
	    ESP_LOGI( TAG, "wakeup from timer, calling ping");
	    ping();
	    break;

        default:
	    ESP_LOGI( TAG, "Strange, got another wakup cause: %d", cause);
	    break;

    }

    time_t boot_time;
    time_t now; 

    time( &boot_time);

    int cnt = 0;
    read_config();


    //FIXME do we need to check OTA immediately ?
    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);

    while(1) {

	time( &now);
        ESP_LOGI( TAG, "main loop cnt: %d at %ld s", ++cnt, now);
        vTaskDelay(10000 / portTICK_RATE_MS);

	if ((now - boot_time) >= CONFIG_ESP_TIME_POWERSAVING) {
		//it's time for power saving
		go_to_bed();
	}
    }
}

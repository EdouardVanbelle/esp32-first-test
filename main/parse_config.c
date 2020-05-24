#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>

#include "parse_config.h"

static const char *TAG = "parse-config";

#ifdef ESP_PLATFORM

#include "esp_log.h"

#else

#define ESP_LOGE(tag, fmt, ...) printf( "E: " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf( "W: " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf( "I: " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf( "D: " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) printf( "V: " fmt "\n", ##__VA_ARGS__)

#endif

int parse_config_buffer( struct_pc_keymap definition[], const char *buffer_to_barse) {

	char * dup = strdup( buffer_to_barse);
	char * line = strtok( dup, "\n");
	int linenum = 0;
        int i=0;
	int success=1;

	while(line) {
	   linenum++;

	   //remote any beginning spaces
	   for( ; (*line == ' ') || (*line == '\t'); line++) {}

	   int found = 0;

	   //ignore comments and empty lines
           switch (*line) {
	   	case '#':
		case '\0':
			break;
		default:
			//check each definition
			for( i=0; definition[i].name != NULL; i++) {
				int label_len = strlen( definition[i].name);
				if( (strncmp( line, definition[i].name, label_len) == 0) && (line[label_len] == ':') ) {

					ESP_LOGI( TAG, "label %s recognized at line %d", definition[i].name, linenum);

					if (definition[i].defined)
						ESP_LOGW( TAG, "label %s already defined, will be overwritten", definition[i].name);

					//clean up leading spaces
					char *value;
					for( 
					      value = line+(label_len+1);
					      *value == ' ';
					      value++) {}

					//clean up trailing spaces
					char *end;
					for( end = value;
					     *end!= '\0';
					     end++ ) {
					      switch (*end) {
						case ' ':
						case '\t':
							*end = '\0'; //end of line
							break;	  
					      } 
					}

					if( strlen( value) == 0)
						ESP_LOGW( TAG, "empty value at line %d", linenum);

					//assign value to target
					switch ( definition[i].type) {
						case TYPE_STRING:
							//XXX: what about freeing previous entry if not null ?
							*((char **) (definition[i].target)) = strdup( value);
							break;

						case TYPE_INT:
							 *((int64_t *) (definition[i].target))    = atoi( value);
							break;
					}

					definition[i].defined = 1;
					found = 1;
					break; //no need to loop to other definition
				}
			}
			if (!found) {
				ESP_LOGW( TAG, "line %d not recognized: [%s]", linenum, line);
			}
	   }

	   //go to next line	
	   line  = strtok(NULL, "\n");
	}

	//check that all mandatory definition have been defined & cleanup
	for( i=0; definition[i].name != NULL; i++) {
		if (definition[i].mandatory && !(definition[i].defined)) {
			ESP_LOGW( TAG, "mandatory label %s not found", definition[i].name);
			success = 0;
		}

		definition[i].defined = 0; // reset for future call
	}

	//free memory
	free( dup);

	return success;

}

// --------------------------------------------------------- test

#ifdef TEST_PARSE_CONFIG

//to test: gcc -D TEST_PARSE_CONFIG=y parse_config.c -o parse_config_test

char *movement_webhook = NULL;
int64_t watchdog_wakeup = -1;

struct_pc_keymap label_definition[]= {
	{ .name="movement_webhook", .target=&movement_webhook, .type=TYPE_STRING, .defined=0, .mandatory=1  },
	{ .name="watchdog_wakeup",  .target=&watchdog_wakeup,  .type=TYPE_INT,    .defined=0, .mandatory=0  },
	{ .name=NULL,                                                                                      } //mark end of array
};

char* config = 
	"# config for nuc\n"
	"movement_webhook:     \n"
    	"     movement_webhook:      http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/H%C3%A9%20h%C3%A9%2C%20je%20vous%20ai%20vu%20%21         \n"
	"           \n"
    	"     movement_webhook:      http://nuc.lan/api/bose/Bose-Bureau/custom-notify/fr/H%C3%A9%20h%C3%A9%2C%20je%20vous%20ai%20vu%20%21         \n"
	"plop: plop\n"
	"watchdog_wakeup: 30\n"
	"           \n";


void main( void) {
	parse_config_buffer( label_definition, config);

	printf("webhook: [%s]\n", movement_webhook);
	printf("watchdog: [%d]\n", watchdog_wakeup);
}

#endif

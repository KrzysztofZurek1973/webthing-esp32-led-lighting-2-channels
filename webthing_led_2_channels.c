/* *********************************************************
 * LED 2 channel controller
 * Compatible with Web Thing API
 *
 *  Created on:		Jan 18, 2021
 * Last update:		Jan 18, 2021
 *      Author:		Krzysztof Zurek
 *		E-mail:		krzzurek@gmail.com
 		   www:		alfa46.com
 *
 ************************************************************/
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "simple_web_thing_server.h"
#include "webthing_led_2_channels.h"

typedef enum {CH_A = 0, CH_B = 1, CH_AB = 2} channel_t;
typedef enum {CHANNEL = 0, BRIGHTNESS = 1, FADE = 2} nvs_data_type_t;
#define APP_PERIOD 5000

//relays
#define GPIO_CH_A			(CONFIG_CHANNEL_A_GPIO)
#define GPIO_CH_B			(CONFIG_CHANNEL_B_GPIO)
#define GPIO_RELAY_MASK 	((1ULL << GPIO_CH_A) | (1ULL << GPIO_CH_B))
#define ESP_INTR_FLAG_DEFAULT 0
#define LEDC_CHANNEL_A		LEDC_CHANNEL_0
#define LEDC_CHANNEL_B		LEDC_CHANNEL_1

xSemaphoreHandle led_mux;
xTaskHandle led_task;

static bool DRAM_ATTR fade_is_running = false;
//static int32_t DRAM_ATTR fade_counter = 0;
static bool init_data_sent = false;
static bool timer_is_running = false;
static channel_t current_channel, prev_current_channel;

//THINGS AND PROPERTIES
//------------------------------------------------------------
//--- Thing
thing_t *leds = NULL;

char leds_id_str[] = "2 leds";
char leds_attype_str[] = "Light";
char leds_disc[] = "Dimmable leds, 2 channels";
at_type_t leds_type;

//------  property "on" - ON/OFF state
static bool device_is_on = false;
property_t *prop_on;
at_type_t on_prop_type;
int16_t set_on_off(char *new_value_str); //switch ON/OFF
char on_prop_id[] = "on";
char on_prop_disc[] = "on-off state";
char on_prop_attype_str[] = "OnOffProperty";
char on_prop_title[] = "ON/OFF";

//------  property "channel" - list of channels: A, B or AB
property_t *prop_channel;
at_type_t channel_prop_type;
enum_item_t enum_ch_A, enum_ch_B, enum_ch_AB;
int16_t set_channel(char *new_value_str);
char channel_prop_id[] = "channel";
char channel_prop_disc[] = "Channel";
char channel_prop_attype_str[] = "ChannelProperty";
char channel_prop_title[] = "Channel";
char channel_tab[3][4] = {"A", "B", "A+B"}; 

//------  property "daily_on" - daily on time
property_t *prop_daily_on_time;
at_type_t daily_on_prop_type;
static int daily_on_time_min = 0, daily_on_time_sec = 0;
static time_t on_time_last_update = 0;
void update_on_time(bool);
char daily_on_prop_id[] = "daily-on";
char daily_on_prop_disc[] = "amount of time device is ON";
char daily_on_prop_attype_str[] = "LevelProperty";
char daily_on_prop_unit[] = "min";
char daily_on_prop_title[] = "ON minutes";

//------  property "brightness"
static TimerHandle_t fade_timer = NULL;
void fade_timer_fun(TimerHandle_t xTimer);
static int32_t brightness; //0..100 in percent
property_t *prop_brgh;
at_type_t brgh_prop_type;
char brgh_id[] = "brightness";
char brgh_prop_disc[] = "Led brightness";
char brgh_prop_attype_str[] = "BrightnessProperty";
char brgh_prop_unit[] = "percent";
char brgh_prop_title[] = "Brightness";

//------  property "fade_time"
static int32_t fade_time; //10..10000 ms
property_t *prop_fade_time;
at_type_t fade_time_prop_type;
char fade_time_id[] = "fade-time";
char fade_time_prop_disc[] = "Fade time in ms";
char fade_time_prop_attype_str[] = "LevelProperty";
char fade_time_prop_unit[] = "ms";
char fade_time_prop_title[] = "Fade time";

//------ action "timer"
static TimerHandle_t timer = NULL;
action_t *timer_action;
int8_t timer_run(char *inputs);
char timer_id[] = "timer";
char timer_title[] = "Timer";
char timer_desc[] = "Turn ON device for specified period of time";
char timer_input_attype_str[] = "ToggleAction";
char timer_prop_dur_id[] = "duration";
action_input_prop_t *timer_duration;
char timer_duration_unit[] = "min";
double timer_duration_min = 1; //minutes
double timer_duration_max = 600;
at_type_t timer_input_attype;

//task function
void leds_fun(void *param); //thread function

//other functions
void read_nvs_data(bool read_default);
void write_nvs_data(void);


/***********************************************************
*
* fade up one channel
* inputs"
*	- ch - channel number
*	- brgh - brightness [0 .. 100]
*	- ft - fade time [miliseconds]
*
************************************************************/
int8_t fade_up_channel(ledc_channel_t ch, int32_t brgh, int32_t ft){
	int32_t duty;
	
	if (brgh != 0){
		duty = (brgh * 8191)/100;
	}
	else{
		duty = 0;
	}
	//fade_counter++;
	ledc_set_fade_with_time(LEDC_HIGH_SPEED_MODE, ch, duty, (uint32_t)ft);
    ledc_fade_start(LEDC_HIGH_SPEED_MODE, ch, LEDC_FADE_NO_WAIT);
    
    if (fade_is_running == false){
    	fade_is_running = true;
    	//unblock "fade_is_ruuning" after fade finished + 1 sec
   		fade_timer = xTimerCreate("fade_timer",
								pdMS_TO_TICKS(ft) + 5,
								pdFALSE,
								pdFALSE,
								fade_timer_fun);

	
		if (xTimerStart(fade_timer, 0) == pdFAIL){
			printf("fade timer failed\n");
		}
	}
    
    return 1;
}


/*****************************************
 *
 * fade timer finished
 *
 ******************************************/
void fade_timer_fun(TimerHandle_t xTimer){
	
	xSemaphoreTake(led_mux, portMAX_DELAY);
	fade_is_running = false;
	xSemaphoreGive(led_mux);
	
	xTimerDelete(xTimer, 10); //delete timer
}


/* ****************************************************************
 *
 * set fading time in milisecond, range 100 .. 10000 msec
 *
 * ****************************************************************/
int16_t fade_time_set(char *new_value_str){
	int32_t ft;
	int16_t result = 0;
	
	xSemaphoreTake(led_mux, portMAX_DELAY);
	if (fade_is_running == true){
		xSemaphoreGive(led_mux);
		return -1;
	}

	ft = atoi(new_value_str);
	if (ft > 10000){
		ft = 10000;
	}
	else if (ft < 100){
		ft = 100;
	}

	if (fade_time != ft){
		fade_time = ft;
		result = 1;
	}
	xSemaphoreGive(led_mux);

	return result;
}


/* ****************************************************************
 *
 * set brightness
 *
 * output:
 *		0 - value not changed
 *		1 - new value set, all clients will be informed
 *	   -1 - error
 *
 * ****************************************************************/
int16_t brightness_set(char *new_value_str){
	int32_t brgh;
	int16_t result = 0;
	
	xSemaphoreTake(led_mux, portMAX_DELAY);
	
	if (fade_is_running == true){
		xSemaphoreGive(led_mux);
		return -1;
	}

	brgh = atoi(new_value_str);
	if (brgh > 100){
		brgh = 100;
	}
	else if (brgh < 0){
		brgh = 0;
	}

	if (brightness != brgh){
		brightness = brgh;
		result = 1;
	}
	
	//set new brightness if device is on
	if (device_is_on == true){
		switch (current_channel){
			case CH_A:
				//set channel A
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				break;
				
			case CH_B:
				//set channel B
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
				break;
				
			case CH_AB:
			default:
				//set channel A and B
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				//wait a bit
				vTaskDelay(20 / portTICK_PERIOD_MS);
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
		}
	}
	//fade_counter = 0;
	xSemaphoreGive(led_mux);

	return result;
}


/* *****************************************************************
 *
 * turn the device ON or OFF
 *
 * output:
 *		0 - value not changed
 *		1 - new value set, all clients will be informed
 *	   -1 - error
 *
 * *****************************************************************/
int16_t set_on_off(char *new_value_str){
	int32_t brgh = 0;
	bool state_change = false;
	int16_t result = 0;

	xSemaphoreTake(led_mux, portMAX_DELAY);
	if (fade_is_running == true){
		//fade action is running
		xSemaphoreGive(led_mux);
		return -1;
	}
	
	if (strcmp(new_value_str, "true") == 0){
		//switch ON
		if (device_is_on == false){
			device_is_on = true;
			brgh = brightness;
			state_change = true;
		}
	}
	else if (strcmp(new_value_str, "false") == 0){
		//switch OFF
		if (device_is_on == true){
			device_is_on = false;
			brgh = 0;
			state_change = true;
		}
	}
	else{
		//error
		xSemaphoreGive(led_mux);
		return -1;
	}
	
	if (state_change == true){
		//turn channel ON/OFF
		switch (current_channel){
			case CH_A:
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				break;
					
			case CH_B:
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
				break;
				
			case CH_AB:
			default:
				//start channel A
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				//wait a bit
				vTaskDelay(20 / portTICK_PERIOD_MS);
				//start channel B
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
		}
		//TODO: stop can be executed after fade up finished
		//if (brgh == 0){	
		//	ledc_stop(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_A, 0);
		//	ledc_stop(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_B, 0);
		//}
		if (device_is_on == false){
			write_nvs_data();
		}
		result = 1;
	}
	else{
		result = 0;
	}
	
	xSemaphoreGive(led_mux);	
	
	return result;
}


/******************************************************
 *
 * timer is finished, turn all channels OFF
 *
 * *****************************************************/
void timer_fun(TimerHandle_t xTimer){
	bool state_changed = false;
	
	complete_action(0, "timer", ACT_COMPLETED);
	
	xSemaphoreTake(led_mux, portMAX_DELAY);

	if (device_is_on == true){
		//switch OFF both channels
		device_is_on = false;
		switch (current_channel){
			case CH_A:
				fade_up_channel(LEDC_CHANNEL_A, 0, fade_time);
				break;
				
			case CH_B:
				fade_up_channel(LEDC_CHANNEL_B, 0, fade_time);
				break;
				
			case CH_AB:
			default:
				//start channel A
				fade_up_channel(LEDC_CHANNEL_A, 0, fade_time);
				//wait a bit
				vTaskDelay(20 / portTICK_PERIOD_MS);
				//start channel B
				fade_up_channel(LEDC_CHANNEL_B, 0, fade_time);
		}
		state_changed = true;
	}
	xSemaphoreGive(led_mux);
	
	//fade_counter = 0;
	
	xTimerDelete(xTimer, 100); //delete timer
	timer_is_running = false;
	
	if (state_changed == true){
		inform_all_subscribers_prop(prop_on);
		//copy current poropeties values
		channel_t prev_cc = current_channel;
		int32_t prev_fade_time = fade_time;
		int32_t prev_brgh = brightness;
		read_nvs_data(false);
		//if any of the properties is changed inform clients
		if (prev_cc != current_channel){
			prop_channel -> value = channel_tab[current_channel];
			inform_all_subscribers_prop(prop_channel);
		}
		if (prev_fade_time != fade_time){
			inform_all_subscribers_prop(prop_fade_time);
		}
		if (prev_brgh != brightness){
			inform_all_subscribers_prop(prop_brgh);
		}
	}
}


/**********************************************************
 *
 * timer action
 * inputs:
 * 		- minutes of turn ON in json, e.g.: "duration":10
 *
 * *******************************************************/
int8_t timer_run(char *inputs){
	int duration = 0, len;
	char *p1, buff[6];
	bool switched_on = false;

	if (timer_is_running == true){
		goto inputs_error;
	}
	
	//get duration value
	p1 = strstr(inputs, "duration");
	if (p1 == NULL){
		goto inputs_error;
	}
	p1 = strchr(p1, ':');
	if (p1 == NULL){
		goto inputs_error;
	}
	len = strlen(inputs) - (p1 + 1 - inputs);
	if (len > 5){
		goto inputs_error;
	}
	memset(buff, 0, 6);
	memcpy(buff, p1 + 1, len);
	duration = atoi(buff);
	if ((duration > 600) || (duration == 0)){
		goto inputs_error;
	}
	
	xSemaphoreTake(led_mux, portMAX_DELAY);
	if (device_is_on == false){
		device_is_on = true; //if device is OFF switch it ON now
		switched_on = true;
		int32_t brgh = brightness;
	
		//check current channel
		switch (current_channel){
			case CH_A:
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				break;
				
			case CH_B:
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
				break;
				
			case CH_AB:
			default:
				//start channel A
				fade_up_channel(LEDC_CHANNEL_A, brgh, fade_time);
				//wait a bit
				vTaskDelay(20 / portTICK_PERIOD_MS);
				//start channel B
				fade_up_channel(LEDC_CHANNEL_B, brgh, fade_time);
		}
		//fade_counter = 0;
	}
	//start timer
	timer = xTimerCreate("timer",
						pdMS_TO_TICKS(duration * 60 * 1000),
						pdFALSE,
						pdFALSE,
						timer_fun);

	xSemaphoreGive(led_mux);
	
	if (xTimerStart(timer, 5) == pdFAIL){
		printf("timer failed\n");
	}
	else{
		timer_is_running = true;
		if (switched_on == true){
			inform_all_subscribers_prop(prop_on);
		}
	}

	return 0;

	inputs_error:
		printf("timer ERROR\n");
	return -1;
}


/*******************************************************************
*
* set channel, called after http PUT method
* output:
*	0 - value is ok, but not changed (the same as previous one)
*	1 - value is changed, subscribers will be informed
*  -1 - error
*
*******************************************************************/
int16_t set_channel(char *new_value_str){
	bool channel_is_changed = false;
	char *buff = NULL;
	int16_t result = 0;
	
	//in websocket quotation mark is not removed
	//(in http should be the same but is not)
	buff = malloc(strlen(new_value_str));
	if (new_value_str[0] == '"'){
		memset(buff, 0, strlen(new_value_str));
		char *ptr = strchr(new_value_str + 1, '"');
		int len = ptr - new_value_str - 1;
		memcpy(buff, new_value_str + 1, len);
	}
	else{
		strcpy(buff, new_value_str);
	}
	
	//set channel
	if (prop_channel -> enum_list != NULL){
		int i = 0;
		enum_item_t *enum_item = prop_channel -> enum_list;
		while (enum_item != NULL){
			if (strcmp(buff, enum_item -> value.str_addr) == 0){
				prop_channel -> value = enum_item -> value.str_addr;
				if (i != current_channel){
					prev_current_channel = current_channel;
					current_channel = i;
					channel_is_changed = true;
				}
				else{
					channel_is_changed = false;
					result = 1;
				}
				break;
			}
			else{
				enum_item = enum_item -> next;
				i++;
			}
		}
	}

	//if channel is changed when device is ON then switch OFF previous channel
	//and switch ON new channel
	if ((channel_is_changed == true) && (device_is_on == true) && 
		(fade_is_running == false)){
		switch (prev_current_channel){
			case CH_A:
				if (current_channel == CH_B){
					fade_up_channel(LEDC_CHANNEL_A, 0, fade_time);
					vTaskDelay(20 / portTICK_PERIOD_MS);
					fade_up_channel(LEDC_CHANNEL_B, brightness, fade_time);
				}
				else{
					fade_up_channel(LEDC_CHANNEL_B, brightness, fade_time);
				}
				break;
				
			case CH_B:
				if (current_channel == CH_A){
					fade_up_channel(LEDC_CHANNEL_B, 0, fade_time);
					vTaskDelay(20 / portTICK_PERIOD_MS);
					fade_up_channel(LEDC_CHANNEL_A, brightness, fade_time);
				}
				else{
					fade_up_channel(LEDC_CHANNEL_A, brightness, fade_time);
				}
				break;
				
			case CH_AB:
			default:
				if (current_channel == CH_A){
					fade_up_channel(LEDC_CHANNEL_B, 0, fade_time);
				}
				else{
					fade_up_channel(LEDC_CHANNEL_A, 0, fade_time);
				}									
		}
	}
	free(buff);
	
	return result;
}


/*********************************************************************
 *
 * main task
 *
 * ******************************************************************/
void leds_fun(void *param){
	
	TickType_t last_wake_time = xTaskGetTickCount();
	for (;;){
		last_wake_time = xTaskGetTickCount();
		
		update_on_time(false);
		
		if (init_data_sent == false){
			int8_t s1 = inform_all_subscribers_prop(prop_channel);
			int8_t s2 = inform_all_subscribers_prop(prop_on);
			int8_t s3 = inform_all_subscribers_prop(prop_daily_on_time);
			int8_t s4 = inform_all_subscribers_prop(prop_brgh);
			int8_t s5 = inform_all_subscribers_prop(prop_fade_time);
			if ((s1 == 0) && (s2 == 0) && (s3 == 0) && (s4 == 0) && (s5 == 0)){
				init_data_sent = true;
			}
		}
	
		vTaskDelayUntil(&last_wake_time, APP_PERIOD / portTICK_PERIOD_MS);
	}
}


/***************************************************************
*
* daily ON time update and inform subscribers if necessary
*
****************************************************************/
void update_on_time(bool reset){
	struct tm timeinfo;
	int delta_t = 0, prev_minutes = 0, new_minutes = 0;
	time_t current_time, prev_time;
	bool send_data = false;

	prev_time = on_time_last_update;
	time(&current_time);
	localtime_r(&current_time, &timeinfo);
	if (timeinfo.tm_year > (2018 - 1900)) {
		//time is correct
		xSemaphoreTake(led_mux, portMAX_DELAY);
		if (device_is_on == true){
			prev_minutes = daily_on_time_min;
			new_minutes = prev_minutes;
			delta_t = current_time - prev_time;
			if (delta_t > 0){
				daily_on_time_sec += delta_t;
				daily_on_time_min = daily_on_time_sec / 60;
				new_minutes = daily_on_time_min;
			}	
		}
		on_time_last_update = current_time;
		xSemaphoreGive(led_mux);
		
		if (new_minutes != prev_minutes){
			send_data = true;
		}
		
		if (reset == true){
			xSemaphoreTake(led_mux, portMAX_DELAY);
			daily_on_time_sec = 0;
			daily_on_time_min = 0;
			xSemaphoreGive(led_mux);
			send_data = true;
		}
		
		if (send_data == true){
			inform_all_subscribers_prop(prop_daily_on_time);
		}
    }
}


/*************************************************************
*
* at the beginning of the day reset minuts and seconds counters
* and inform subscribers if necessary
*
**************************************************************/
void daily_on_time_reset(void){
	update_on_time(true);
}


/*******************************************************************
 *
 * initialize GPIOs for channel A and B, both switch OFF
 *
 * ******************************************************************/
void init_ledc(void){
	
	//timer configuration
    ledc_timer_config_t ledc_timer = {
        	.duty_resolution = LEDC_TIMER_13_BIT, 	// resolution of PWM duty
	        .freq_hz = 1000,                      	// frequency of PWM signal
    	    .speed_mode = LEDC_HIGH_SPEED_MODE,		// timer mode
    	    .timer_num = LEDC_TIMER_0,            	// timer index
    	    .clk_cfg = LEDC_AUTO_CLK,              	// Auto select the source clock
    };
    // Set configuration of timer0 for high speed channels
    ledc_timer_config(&ledc_timer);
    
    //channel configuration
    //---- channel A
    ledc_channel_config_t channel_A = {
            .channel    = LEDC_CHANNEL_0,
            .duty       = 0,
            .gpio_num   = CONFIG_CHANNEL_A_GPIO,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER_0,
            //.intr_type	= LEDC_INTR_FADE_END,
            .intr_type	= LEDC_INTR_DISABLE,
    };
    // Set LED Controller with previously prepared configuration
    ledc_channel_config(&channel_A);
    
    //---- channel B
    ledc_channel_config_t channel_B = {
            .channel    = LEDC_CHANNEL_1,
            .duty       = 0,
            .gpio_num   = CONFIG_CHANNEL_B_GPIO,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER_0,
            //.intr_type	= LEDC_INTR_FADE_END,
            .intr_type	= LEDC_INTR_DISABLE,
    };
    // Set LED Controller with previously prepared configuration
    ledc_channel_config(&channel_B);
    
    // Initialize fade service.
    ledc_fade_func_install(ESP_INTR_FLAG_IRAM);
}


/*****************************************************************
 *
 * Initialization of dual light thing and all it's properties
 *
 * ****************************************************************/
thing_t *init_led_2_channels(void){

	read_nvs_data(true);
	prev_current_channel = current_channel;
	
	init_ledc();
	
	//start thing
	led_mux = xSemaphoreCreateMutex();
	//create thing 1, thermostat ---------------------------------
	leds = thing_init();

	leds -> id = leds_id_str;
	leds -> at_context = things_context;
	leds -> model_len = 2300;
	//set @type
	leds_type.at_type = leds_attype_str;
	leds_type.next = NULL;
	set_thing_type(leds, &leds_type);
	leds -> description = leds_disc;
	
	//property: ON/OFF
	prop_on = property_init(NULL, NULL);
	prop_on -> id = "on";
	prop_on -> description = "ON/OFF";
	on_prop_type.at_type = "OnOffProperty";
	on_prop_type.next = NULL;
	prop_on -> at_type = &on_prop_type;
	prop_on -> type = VAL_BOOLEAN;
	prop_on -> value = &device_is_on;
	prop_on -> title = "ON/OFF";
	prop_on -> read_only = false;
	prop_on -> set = set_on_off;
	prop_on -> mux = led_mux;
	add_property(leds, prop_on); //add property to thing
	
	//create "channel" property ------------------------------------
	//pop-up list to choose channel
	prop_channel = property_init(NULL, NULL);
	prop_channel -> id = channel_prop_id;
	prop_channel -> description = channel_prop_disc;
	channel_prop_type.at_type = channel_prop_attype_str;
	channel_prop_type.next = NULL;
	prop_channel -> at_type = &channel_prop_type;
	prop_channel -> type = VAL_STRING;
	prop_channel -> value = channel_tab[current_channel];
	prop_channel -> title = channel_prop_title;
	prop_channel -> read_only = false;
	prop_channel -> enum_prop = true;
	prop_channel -> enum_list = &enum_ch_A;
	enum_ch_A.value.str_addr = channel_tab[0];
	enum_ch_A.next = &enum_ch_B;
	enum_ch_B.value.str_addr = channel_tab[1];
	enum_ch_B.next = &enum_ch_AB;
	enum_ch_AB.value.str_addr = channel_tab[2];
	enum_ch_AB.next = NULL;
	prop_channel -> set = &set_channel;
	prop_channel -> mux = led_mux;

	add_property(leds, prop_channel); //add property to thing	
	
	//create "daily on time" property -------------------------------------------
	prop_daily_on_time = property_init(NULL, NULL);
	prop_daily_on_time -> id = daily_on_prop_id;
	prop_daily_on_time -> description = daily_on_prop_disc;
	daily_on_prop_type.at_type = daily_on_prop_attype_str;
	daily_on_prop_type.next = NULL;
	prop_daily_on_time -> at_type = &daily_on_prop_type;
	prop_daily_on_time -> type = VAL_INTEGER;
	prop_daily_on_time -> value = &daily_on_time_min;
	prop_daily_on_time -> unit = daily_on_prop_unit;
	prop_daily_on_time -> max_value.int_val = 1440;
	prop_daily_on_time -> min_value.int_val = 0;
	prop_daily_on_time -> title = daily_on_prop_title;
	prop_daily_on_time -> read_only = true;
	prop_daily_on_time -> enum_prop = false;
	prop_daily_on_time -> set = NULL;
	prop_daily_on_time -> mux = led_mux;
	
	add_property(leds, prop_daily_on_time); //add property to thing
	
	//property: brightness
	prop_brgh = property_init(NULL, NULL);
	prop_brgh -> id = brgh_id;
	prop_brgh -> description = brgh_prop_disc;
	brgh_prop_type.at_type = brgh_prop_attype_str;
	brgh_prop_type.next = NULL;
	prop_brgh -> at_type = &brgh_prop_type;
	prop_brgh -> type = VAL_INTEGER;
	prop_brgh -> value = &brightness;
	prop_brgh -> max_value.int_val = 100;
	prop_brgh -> min_value.int_val = 0;
	prop_brgh -> unit = brgh_prop_unit;
	prop_brgh -> title = brgh_prop_title;
	prop_brgh -> read_only = false;
	prop_brgh -> set = brightness_set;
	prop_brgh -> mux = led_mux;
	add_property(leds, prop_brgh);
	
	//property: fade_time
	prop_fade_time = property_init(NULL, NULL);
	prop_fade_time -> id = fade_time_id;
	prop_fade_time -> description = fade_time_prop_disc;
	fade_time_prop_type.at_type = fade_time_prop_attype_str;
	fade_time_prop_type.next = NULL;
	prop_fade_time -> at_type = &fade_time_prop_type;
	prop_fade_time -> type = VAL_INTEGER;
	prop_fade_time -> value = &fade_time;
	prop_fade_time -> max_value.int_val = 10000;
	prop_fade_time -> min_value.int_val = 100;
	prop_fade_time -> unit = fade_time_prop_unit;
	prop_fade_time -> title = fade_time_prop_title;
	prop_fade_time -> read_only = false;
	prop_fade_time -> set = fade_time_set;
	prop_fade_time -> mux = led_mux;
	add_property(leds, prop_fade_time);
	
	//create action "timer", turn on lights (device) for specified minutes
	timer_action = action_init();
	timer_action -> id = timer_id;
	timer_action -> title = timer_title;
	timer_action -> description = timer_desc;
	timer_action -> run = timer_run;
	timer_input_attype.at_type = timer_input_attype_str;
	timer_input_attype.next = NULL;
	timer_action -> input_at_type = &timer_input_attype;
	timer_duration = action_input_prop_init(timer_prop_dur_id,
											VAL_INTEGER, true,
											&timer_duration_min,
											&timer_duration_max,
											timer_duration_unit);
	add_action_input_prop(timer_action, timer_duration);
	add_action(leds, timer_action);

	//start thread	
	xTaskCreate(&leds_fun, "leds", configMINIMAL_STACK_SIZE * 4, NULL, 5, &led_task);

	return leds;
}


/****************************************************************
 *
 * read dual light data written in NVS memory:
 *  - current channel
 *
 * **************************************************************/
void read_nvs_data(bool read_default){
	esp_err_t err;
	nvs_handle storage = 0;

	if (read_default == true){
		//default values
		current_channel = CH_AB;
		brightness = 20;
		fade_time = 2000;
	}

	// Open
	//printf("Reading NVS data... ");

	err = nvs_open("storage", NVS_READONLY, &storage);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		int8_t d8;
		int32_t d32;
		// Read data
		if (nvs_get_i8(storage, "curr_channel", &d8) != ESP_OK){
			printf("current channel not found in NVS\n");
		}
		else{
			current_channel = d8;
		}
		
		if (nvs_get_i32(storage, "brightness", &d32) != ESP_OK){
			printf("brightness not found in NVS\n");
		}
		else{
			brightness = d32;
		}
		
		if (nvs_get_i32(storage, "fade_time", &d32) != ESP_OK){
			printf("fade time not found in NVS\n");
		}
		else{
			fade_time = d32;
		}
		// Close
		nvs_close(storage);
	}
}

/****************************************************************
 *
 * write NVS data into flash memory
 * input:
 *	- data type: channel, brightness or fade time
 *  - data to be written
 *
 * **************************************************************/
void write_nvs_data(void){
	esp_err_t err;
	nvs_handle storage = 0;
	
	//open NVS falsh memory
	err = nvs_open("storage", NVS_READWRITE, &storage);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		int32_t brgh, ft = 0;
		int8_t ch = 0;
		
		if (nvs_get_i8(storage, "curr_channel", &ch) == ESP_OK){
			if (ch != current_channel){
				nvs_set_i8(storage, "curr_channel", current_channel);
			}
		}
		if (nvs_get_i32(storage, "brightness", &brgh) == ESP_OK){
			if (brgh != brightness){
				nvs_set_i32(storage, "brightness", brightness);
			}
		}
		if (nvs_get_i32(storage, "fade_time", &ft) == ESP_OK){
			if (ft != fade_time){
				nvs_set_i32(storage, "fade_time", fade_time);
			}
		}
		err = nvs_commit(storage);
		// Close
		nvs_close(storage);
	}
}

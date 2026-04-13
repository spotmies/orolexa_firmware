/******************** INCLUDES ********************/
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi_remote.h"
#include "esp_http_server.h"
#include "mdns.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"

#include "esp_timer.h"

#include "usb/usb_host.h"
#include "usb/uvc_host.h"

/******************** CONFIG ********************/
#define TAG "ESP32P4"

/* HTTP PORTS */
#define PROVISION_HTTP_PORT 80
#define STREAM_HTTP_PORT    81
#define WIFI_SAVE_NAMESPACE  "wifi"
#define WIFI_SAVE_KEY_SSID   "ssid"
#define WIFI_SAVE_KEY_PASS   "pass"
#define WIFI_CONNECT_DELAY_MS 12000   /* 12 s before connecting after provisioning */

/* mDNS: hostname and service for app discovery (e.g. react-native-zeroconf) */
#define MDNS_HOSTNAME     "dentals3"
#define MDNS_INSTANCE     "DentalS3 Camera"

/* USB camera: backoff for repeated plug/unplug so host can recover */
#define UVC_RETRY_AFTER_DISCONNECT_MS 15000   /* 15 s after close before reopen */
#define UVC_RETRY_AFTER_OPEN_FAIL_MS  20000   /* 20 s when no camera / USB busy */
#define UVC_SETTLE_AFTER_DISCONNECT_MS 300    /* 300 ms after disconnect before stop/close */

/* USB Power */
#define USB_PWR_CTRL_GPIO  1 

/* Buttons: 2-user + Boot. Pressure=pressure detection; Multi=capture (USB) or duty cycle (motor) */
#define BTN_PRESSURE_GPIO  6   /* Pressure detection */
#define BTN_MULTI_GPIO     4   /* Camera: capture. Motor: cycle Off→50%→80%→100%→Off */
#define BTN_BOOT_GPIO      0   /* Long-press: force AP */

#define DEBOUNCE_US 250000

/* MOTOR (L298N) - Motor A: OUT1 & OUT2
 * ENA=GPIO21 (PWM), IN1=GPIO32, IN2=GPIO33. L298N: ENA->GPIO21, IN1->GPIO32, IN2->GPIO33; OUT1/OUT2 to motor. */
#define MOTOR_ENA_GPIO   21
#define MOTOR_IN1_GPIO   32   /* Left header GPIO32 - if 47 didn't drive high, use 32 */
#define MOTOR_IN2_GPIO   33   /* Right header GPIO33 - alternate: GPIO36 (left) */

#define LED_PWM_CH     LEDC_CHANNEL_0
#define LED_PWM_TIMER  LEDC_TIMER_0

/* MPU6050 */
#define I2C_PORT   I2C_NUM_0
#define SDA_PIN    7
#define SCL_PIN    8
#define MPU_ADDR   0x68
#define MPU_PWR    0x6B
#define MPU_AX     0x3B

/******************** MODES ********************/
typedef enum {
    MODE_CAMERA = 0,
    MODE_MOTOR
} system_mode_t;

static volatile system_mode_t current_mode = MODE_MOTOR;  /* USB connected -> CAMERA, else MOTOR */

/******************** GLOBALS ********************/
static QueueHandle_t frame_queue;
static httpd_handle_t provision_httpd = NULL;
static httpd_handle_t stream_httpd = NULL;

static bool uvc_connected = false;
static bool sta_has_ip = false;
static volatile bool force_ap = false;
static bool provisioning_done = false;

static char saved_ssid[32];
static char saved_pass[64];

/* Camera: set when physical button pressed; mobile app polls GET /api/capture_request */
static volatile bool capture_requested = false;

/* Capture buffer: one JPEG frame, no re-encode (full quality). 256 KB for 1080p MJPEG. */
#define CAPTURE_BUF_SIZE  (256 * 1024)
static uint8_t capture_buf[CAPTURE_BUF_SIZE];
static size_t capture_len;
static volatile bool capture_ready;
static SemaphoreHandle_t capture_mutex;

/******************** MOTOR SPEED ********************/
/* Cycle: 0=off, 1=50%, 2=80%, 3=100% (then back to 0). 13-bit max 8191 */
static volatile int motor_cycle_index = 0;
static const uint32_t speed_duty[3] = {4096, 6553, 8191};  /* 50%, 80%, 100% */
static volatile bool motor_running = false;
static TaskHandle_t motor_log_task_handle = NULL;
static QueueHandle_t motor_cmd_queue = NULL;
#define MOTOR_CMD_QUEUE_LEN 8

/* Task to log motor state changes (from task context, not ISR) */
static void motor_log_task(void *arg)
{
    uint32_t last_duty = 0;
    bool last_running = false;
    
    while (1) {
        if (motor_running != last_running || 
            (motor_running && ledc_get_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH) != last_duty)) {
            uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
            int in1_level = gpio_get_level(MOTOR_IN1_GPIO);
            int in2_level = gpio_get_level(MOTOR_IN2_GPIO);
            
            ESP_LOGI(TAG, "Motor: running=%d, duty=%lu, IN1=%d, IN2=%d, ENA(PWM)=%lu",
                     motor_running, current_duty, in1_level, in2_level, current_duty);
            
            last_duty = current_duty;
            last_running = motor_running;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/******************** BUTTON DEBOUNCE ********************/
static int64_t last_btn_time[3];  /* 0=multi, 1=pressure, 2=boot */
static int64_t boot_press_time;

/******************** UVC ********************/
static uvc_host_stream_hdl_t uvc_stream = NULL;

/* =====================================================
 * MPU6050
 * ===================================================== */
static void mpu_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000
    };
    i2c_param_config(I2C_PORT, &cfg);
    i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);

    uint8_t cmd[2] = {MPU_PWR, 0x00};
    i2c_master_write_to_device(I2C_PORT, MPU_ADDR, cmd, 2, pdMS_TO_TICKS(100));
}

static void mpu_task(void *arg)
{
    int16_t ax, ay, az;
    uint8_t reg = MPU_AX;
    uint8_t buf[6];
    const int16_t thresh = 12000;

    while (1) {
        if (sta_has_ip &&
            i2c_master_write_read_device(I2C_PORT, MPU_ADDR,
                                         &reg, 1, buf, 6,
                                         pdMS_TO_TICKS(100)) == ESP_OK) {

            ax = (buf[0] << 8) | buf[1];
            ay = (buf[2] << 8) | buf[3];
            az = (buf[4] << 8) | buf[5];

            /* Position of teeth from all three axes */
            const char *pos;
            if (az > thresh)       pos = "FRONT";
            else if (az < -thresh) pos = "BACK";
            else if (ax > thresh)  pos = "RIGHT";
            else if (ax < -thresh) pos = "LEFT";
            else if (ay > thresh)  pos = "UP";
            else if (ay < -thresh) pos = "DOWN";
            else                   pos = "NEUTRAL";

            ESP_LOGI("MPU", "%s  ax=%d ay=%d az=%d", pos, (int)ax, (int)ay, (int)az);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =====================================================
 * PWM MOTOR
 * ===================================================== */
static void pwm_init(void)
{
    esp_err_t ret;
    
    /* Reset all motor GPIOs to clear any previous configuration (e.g. ADC on GPIO21) */
    gpio_reset_pin(MOTOR_ENA_GPIO);
    gpio_reset_pin(MOTOR_IN1_GPIO);
    gpio_reset_pin(MOTOR_IN2_GPIO);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Configure GPIOs as outputs with explicit pull settings */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MOTOR_IN1_GPIO) | (1ULL << MOTOR_IN2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ret = gpio_config(&io_conf);
    ESP_LOGI(TAG, "Motor GPIOs (IN1/IN2) config: %s", ret == ESP_OK ? "OK" : "FAILED");

    /* Initialize LEDC timer */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LED_PWM_TIMER,
        .freq_hz = 5000,
        .duty_resolution = LEDC_TIMER_13_BIT
    };
    ret = ledc_timer_config(&timer);
    ESP_LOGI(TAG, "LEDC timer config: %s", ret == ESP_OK ? "OK" : "FAILED");

    /* Initialize LEDC channel for ENA (GPIO21) */
    ledc_channel_config_t ch = {
        .gpio_num = MOTOR_ENA_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LED_PWM_CH,
        .timer_sel = LED_PWM_TIMER,
        .duty = 0
    };
    ret = ledc_channel_config(&ch);
    ESP_LOGI(TAG, "GPIO%d (ENA/PWM) LEDC channel: %s", MOTOR_ENA_GPIO, 
             ret == ESP_OK ? "OK" : "FAILED");

    /* Multimeter test: 50% duty for 5s so DC voltage on GPIO21 ≈ 1.65V (3.3V/2) */
    ESP_LOGI(TAG, "GPIO%d (ENA): 50%% PWM for 5s - multimeter DC should show ~1.65V", MOTOR_ENA_GPIO);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH, 4096);  /* 50% of 8192 (13-bit) */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    vTaskDelay(pdMS_TO_TICKS(5000));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    ESP_LOGI(TAG, "GPIO%d (ENA): PWM test done, duty=0", MOTOR_ENA_GPIO);
    
    /* Initial state: IN1,IN2 must be (1,0) or (0,1) only */
    gpio_set_level(MOTOR_IN1_GPIO, 0);
    gpio_set_level(MOTOR_IN2_GPIO, 1);
    ESP_LOGI(TAG, "Motor GPIOs initialized: IN1=0, IN2=1, ENA=0");
}

/* Called only from motor_control_task (task context) so IN1/IN2 and PWM apply correctly */
static void led_set_speed(uint32_t duty)
{
    /* Force output mode in case another driver reconfigures these pins (e.g. on ESP32-P4) */
    gpio_set_direction(MOTOR_IN1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(MOTOR_IN2_GPIO, GPIO_MODE_OUTPUT);

    if (duty == 0) {
        motor_running = false;
        /* Stopped: IN1=0, IN2=1 (coast/ready) */
        gpio_set_level(MOTOR_IN1_GPIO, 0);
        gpio_set_level(MOTOR_IN2_GPIO, 1);
    } else {
        motor_running = true;
        /* Forward: IN1=1, IN2=0 */
        gpio_set_level(MOTOR_IN1_GPIO, 1);
        gpio_set_level(MOTOR_IN2_GPIO, 0);
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
}

/* Task: apply motor commands from queue (task context so GPIO/LEDC work on ESP32-P4) */
static void motor_control_task(void *arg)
{
    uint32_t duty;
    while (1) {
        if (xQueueReceive(motor_cmd_queue, &duty, portMAX_DELAY) == pdTRUE) {
            led_set_speed(duty);
            /* Diagnostic: read back IN1/IN2 right after set (if still 0, try different GPIOs e.g. 18,19) */
            if (duty != 0) {
                int i1 = gpio_get_level(MOTOR_IN1_GPIO);
                int i2 = gpio_get_level(MOTOR_IN2_GPIO);
                if (i1 != 1 || i2 != 0)
                    ESP_LOGW(TAG, "IN1/IN2 read back %d/%d (expected 1/0) - check wiring to GPIO %d,%d", i1, i2, MOTOR_IN1_GPIO, MOTOR_IN2_GPIO);
            }
        }
    }
}

/* Test function to verify GPIO signals - call from task context */
static void test_motor_gpios(void)
{
    ESP_LOGI(TAG, "=== MOTOR GPIO TEST ===");
    
    /* Test IN1 */
    ESP_LOGI(TAG, "Testing GPIO%d (IN1): Setting HIGH", MOTOR_IN1_GPIO);
    gpio_set_level(MOTOR_IN1_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    int level = gpio_get_level(MOTOR_IN1_GPIO);
    ESP_LOGI(TAG, "GPIO%d (IN1) read back: %d (expected: 1)", MOTOR_IN1_GPIO, level);
    
    ESP_LOGI(TAG, "Testing GPIO%d (IN1): Setting LOW", MOTOR_IN1_GPIO);
    gpio_set_level(MOTOR_IN1_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    level = gpio_get_level(MOTOR_IN1_GPIO);
    ESP_LOGI(TAG, "GPIO%d (IN1) read back: %d (expected: 0)", MOTOR_IN1_GPIO, level);
    
    /* Test IN2 */
    ESP_LOGI(TAG, "Testing GPIO%d (IN2): Setting HIGH", MOTOR_IN2_GPIO);
    gpio_set_level(MOTOR_IN2_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    level = gpio_get_level(MOTOR_IN2_GPIO);
    ESP_LOGI(TAG, "GPIO%d (IN2) read back: %d (expected: 1)", MOTOR_IN2_GPIO, level);
    
    ESP_LOGI(TAG, "Testing GPIO%d (IN2): Setting LOW", MOTOR_IN2_GPIO);
    gpio_set_level(MOTOR_IN2_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    level = gpio_get_level(MOTOR_IN2_GPIO);
    ESP_LOGI(TAG, "GPIO%d (IN2) read back: %d (expected: 0)", MOTOR_IN2_GPIO, level);
    
    /* Test ENA (GPIO21) PWM */
    ESP_LOGI(TAG, "Testing GPIO%d (ENA): Setting PWM duty=2048", MOTOR_ENA_GPIO);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH, 2048);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    ESP_LOGI(TAG, "GPIO%d (ENA) PWM duty read back: %lu (expected: 2048)", MOTOR_ENA_GPIO, duty);
    
    ESP_LOGI(TAG, "Testing GPIO%d (ENA): Setting PWM duty=0", MOTOR_ENA_GPIO);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    vTaskDelay(pdMS_TO_TICKS(500));
    duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CH);
    ESP_LOGI(TAG, "GPIO%d (ENA) PWM duty read back: %lu (expected: 0)", MOTOR_ENA_GPIO, duty);
    
    /* Leave IN1,IN2 as (0,1) - never same */
    gpio_set_level(MOTOR_IN1_GPIO, 0);
    gpio_set_level(MOTOR_IN2_GPIO, 1);
    ESP_LOGI(TAG, "=== MOTOR GPIO TEST COMPLETE ===");
}

/* =====================================================
 * BUTTONS
 * ===================================================== */
static void IRAM_ATTR button_isr(void *arg)
{
    int gpio = (int)arg;
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(gpio);

    int idx = (gpio == BTN_MULTI_GPIO ? 0 : gpio == BTN_PRESSURE_GPIO ? 1 : 2);

    if (gpio == BTN_BOOT_GPIO) {
        if (level == 0) {
            boot_press_time = now;
        } else {
            if ((now - boot_press_time) > 5000000)
                force_ap = true;
        }
        return;
    }

    if (level != 0) return;
    if (now - last_btn_time[idx] < DEBOUNCE_US) return;
    last_btn_time[idx] = now;

    /* Multi button: camera mode = capture; motor mode = cycle Off→50%→80%→100%→Off */
    if (gpio == BTN_MULTI_GPIO) {
        if (current_mode == MODE_CAMERA) {
            capture_requested = true;
            ESP_EARLY_LOGI(TAG, "Capture image");
        } else {
            /* Motor: cycle 0→50%→80%→100%→0 */
            if (motor_cmd_queue) {
                motor_cycle_index = (motor_cycle_index + 1) % 4;
                uint32_t duty = (motor_cycle_index == 0) ? 0 : speed_duty[motor_cycle_index - 1];
                xQueueSendFromISR(motor_cmd_queue, &duty, NULL);
            }
            ESP_EARLY_LOGI(TAG, "Motor cycle=%d (duty=%s)", motor_cycle_index,
                motor_cycle_index == 0 ? "OFF" : motor_cycle_index == 1 ? "50%" :
                motor_cycle_index == 2 ? "80%" : "100%");
        }
    }

    /* Pressure button: show pressure status (detected when motor is running) */
    if (gpio == BTN_PRESSURE_GPIO) {
        ESP_EARLY_LOGI(TAG, "Pressure %s", motor_running ? "detected" : "not detected");
    }
}

static void buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask =
            (1ULL << BTN_PRESSURE_GPIO) |
            (1ULL << BTN_MULTI_GPIO) |
            (1ULL << BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io);
    gpio_install_isr_service(0);

    gpio_isr_handler_add(BTN_PRESSURE_GPIO, button_isr, (void *)BTN_PRESSURE_GPIO);
    gpio_isr_handler_add(BTN_MULTI_GPIO, button_isr, (void *)BTN_MULTI_GPIO);
    gpio_isr_handler_add(BTN_BOOT_GPIO, button_isr, (void *)BTN_BOOT_GPIO);
}

/* =====================================================
 * USB CAMERA
 * ===================================================== */
static bool frame_cb(const uvc_host_frame_t *frame, void *arg)
{
    if (frame) {
        if (capture_requested && capture_mutex != NULL && frame->data_len <= CAPTURE_BUF_SIZE) {
            if (xSemaphoreTake(capture_mutex, 0) == pdTRUE) {
                memcpy(capture_buf, frame->data, frame->data_len);
                capture_len = frame->data_len;
                capture_ready = true;
                capture_requested = false;
                xSemaphoreGive(capture_mutex);
            }
        }
        xQueueSend(frame_queue, &frame, 0);
    }
    return false;
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *arg)
{
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        uvc_connected = false;
        current_mode = MODE_MOTOR;  /* USB unplugged -> motor mode */
        motor_cycle_index = 0;     /* Next multi press starts from Off */
        capture_ready = false;     /* Invalidate captured image */
        if (motor_cmd_queue) {
            uint32_t stop = 0;
            xQueueSend(motor_cmd_queue, &stop, 0);
        }
    }
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

/* Drain frame queue and return frames to UVC (reduces "Not all frames returned" on disconnect) */
static void drain_frame_queue(void)
{
    const uvc_host_frame_t *frame;
    while (uvc_stream != NULL && xQueueReceive(frame_queue, &frame, 0) == pdPASS)
        uvc_host_frame_return(uvc_stream, (uvc_host_frame_t *)frame);
}

static void camera_task(void *arg)
{
    /* 1080p MJPEG @ 30 fps. With PSRAM: larger buffers; without: smaller to fit internal RAM. */
    uvc_host_stream_config_t cfg = {
        .frame_cb = frame_cb,
        .event_cb = stream_event_cb,
        .usb = {.vid = UVC_HOST_ANY_VID, .pid = UVC_HOST_ANY_PID},
        .vs_format = {
            .format = UVC_VS_FORMAT_MJPEG,
            .h_res = 1920,
            .v_res = 1080,
            .fps = 30
        },
        .advanced = {
#if CONFIG_SPIRAM
            .number_of_frame_buffers = 5,
            .frame_size = 512 * 1024,    /* 512 KB per buffer for 1080p MJPEG */
            .frame_heap_caps = MALLOC_CAP_SPIRAM,
#else
            .number_of_frame_buffers = 2,
            .frame_size = 256 * 1024,   /* 256 KB per buffer; 1080p may need PSRAM for smooth streaming */
            .frame_heap_caps = 0,
#endif
            .number_of_urbs = 6,
            .urb_size = 32 * 1024,       /* 32 KB for 1080p payloads */
        }
    };

    while (1) {
        if (uvc_host_stream_open(&cfg, pdMS_TO_TICKS(8000), &uvc_stream) == ESP_OK) {
            uvc_connected = true;
            current_mode = MODE_CAMERA;
            if (motor_cmd_queue) {
                uint32_t stop = 0;
                xQueueSend(motor_cmd_queue, &stop, 0);
            }
            motor_cycle_index = 0;
            uvc_host_stream_start(uvc_stream);
            while (uvc_connected)
                vTaskDelay(pdMS_TO_TICKS(200));

            /* Let disconnect event settle before stop/close (handles repeated plug/unplug) */
            vTaskDelay(pdMS_TO_TICKS(UVC_SETTLE_AFTER_DISCONNECT_MS));
            drain_frame_queue();
            uvc_host_stream_stop(uvc_stream);
            uvc_host_stream_close(uvc_stream);
            uvc_stream = NULL;
            ESP_LOGI(TAG, "USB camera disconnected, waiting %d s before retry", (int)(UVC_RETRY_AFTER_DISCONNECT_MS / 1000));
            vTaskDelay(pdMS_TO_TICKS(UVC_RETRY_AFTER_DISCONNECT_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(UVC_RETRY_AFTER_OPEN_FAIL_MS));
        }
    }
}

/* =====================================================
 * HTTP STREAM
 * ===================================================== */
static esp_err_t stream_index_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>DentalS3 Camera</title>"
        "<style>"
        "body{font-family:sans-serif;margin:1rem;background:#1a1a1a;color:#eee;}"
        "h1{font-size:1.25rem;margin-bottom:0.5rem;}"
        ".stream-wrap{background:#000;border-radius:8px;overflow:hidden;max-width:100%;display:inline-block;}"
        ".stream-wrap img{display:block;max-width:100%;height:auto;}"
        ".note{font-size:0.85rem;color:#999;margin-top:0.5rem;}"
        "</style></head><body>"
        "<h1>Camera stream</h1>"
        "<div class=\"stream-wrap\"><img src=\"/stream\" alt=\"Live stream\" /></div>"
        "<p class=\"note\">Press device Multi button to capture. Mobile app: poll GET /api/capture_request for capture trigger.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,
        "multipart/x-mixed-replace; boundary=frame");

    while (sta_has_ip && current_mode == MODE_CAMERA && uvc_connected) {
        const uvc_host_frame_t *frame;
        if (xQueueReceive(frame_queue, &frame, pdMS_TO_TICKS(1500)) == pdPASS) {
            char h[128];
            int l = snprintf(h, sizeof(h),
                "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                frame->data_len);
            
            esp_err_t ret = httpd_resp_send_chunk(req, h, l);
            if (ret != ESP_OK) break;
            
            ret = httpd_resp_send_chunk(req, (const char *)frame->data, frame->data_len);
            if (ret != ESP_OK) break;
            
            ret = httpd_resp_send_chunk(req, "\r\n", 2);
            if (ret != ESP_OK) break;
            
            if (uvc_stream != NULL)
                uvc_host_frame_return(uvc_stream, (uvc_host_frame_t *)frame);
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/capture_request: mobile app polls this; returns {"capture":true} once after button press */
static esp_err_t capture_request_handler(httpd_req_t *req)
{
    bool trigger = capture_requested;
    if (trigger)
        capture_requested = false;
    const char *json = trigger ? "{\"capture\":true}\n" : "{\"capture\":false}\n";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* GET /api/capture_image: returns last captured JPEG (one-shot, full quality, no re-encode) */
static esp_err_t capture_image_handler(httpd_req_t *req)
{
    if (capture_mutex == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture not ready");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(capture_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
        return ESP_FAIL;
    }
    if (!capture_ready || capture_len == 0) {
        xSemaphoreGive(capture_mutex);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image. Press capture button first.");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t ret = httpd_resp_send(req, (const char *)capture_buf, capture_len);
    capture_ready = false;
    xSemaphoreGive(capture_mutex);
    return ret;
}

static void start_stream_server(void)
{
    if (stream_httpd) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = STREAM_HTTP_PORT;
    cfg.ctrl_port = STREAM_HTTP_PORT + 1;

    httpd_start(&stream_httpd, &cfg);

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = stream_index_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_capture_req = {
        .uri = "/api/capture_request",
        .method = HTTP_GET,
        .handler = capture_request_handler,
        .user_ctx = NULL
    };
    httpd_uri_t uri_capture_img = {
        .uri = "/api/capture_image",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &uri_root);
    httpd_register_uri_handler(stream_httpd, &uri_stream);
    httpd_register_uri_handler(stream_httpd, &uri_capture_req);
    httpd_register_uri_handler(stream_httpd, &uri_capture_img);
}

static void stop_stream_server(void)
{
    if (stream_httpd) {
        httpd_stop(stream_httpd);
        stream_httpd = NULL;
    }
}

/* =====================================================
 * mDNS (for mobile app discovery: react-native-zeroconf, etc.)
 * ===================================================== */
static bool mdns_started = false;

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_started = true;
    err = mdns_hostname_set(MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        mdns_free();
        mdns_started = false;
        return;
    }
    mdns_instance_name_set(MDNS_INSTANCE);
    /* Advertise HTTP stream so apps can discover IP + port via _http._tcp */
    mdns_txt_item_t txt[] = {{ "path", "/stream" }};
    err = mdns_service_add(NULL, "_http", "_tcp", (uint16_t)STREAM_HTTP_PORT, txt, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mDNS: %s.local:%d (discoverable by apps)", MDNS_HOSTNAME, (int)STREAM_HTTP_PORT);
    }
}

static void stop_mdns(void)
{
    if (mdns_started) {
        mdns_free();
        mdns_started = false;
    }
}

/* =====================================================
 * WIFI CREDENTIALS (NVS)
 * ===================================================== */
static bool load_wifi_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_SAVE_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = sizeof(saved_ssid);
    if (nvs_get_str(h, WIFI_SAVE_KEY_SSID, saved_ssid, &len) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    len = sizeof(saved_pass);
    if (nvs_get_str(h, WIFI_SAVE_KEY_PASS, saved_pass, &len) != ESP_OK)
        saved_pass[0] = '\0';
    nvs_close(h);
    return true;
}

static void save_wifi_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_SAVE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_str(h, WIFI_SAVE_KEY_SSID, saved_ssid);
    nvs_set_str(h, WIFI_SAVE_KEY_PASS, saved_pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS (use Boot long-press to re-provision)");
}

/* =====================================================
 * PROVISION SERVER
 * ===================================================== */
static const char provision_html[] =
"<html><body><h3>ESP32 WiFi Setup</h3>"
"<input id='s'><br><input id='p' type='password'><br>"
"<button onclick='go()'>Connect</button>"
"<script>function go(){fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:s.value,pass:p.value})})}</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_send(req, provision_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No body");
        return ESP_FAIL;
    }
    buf[n] = '\0';
    int got = sscanf(buf,
                    "{\"ssid\":\"%31[^\"]\",\"pass\":\"%63[^\"]\"}",
                    saved_ssid, saved_pass);
    if (got < 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    if (got == 1)
        saved_pass[0] = '\0';
    provisioning_done = true;
    save_wifi_to_nvs();
    ESP_LOGI(TAG, "Provisioning received: SSID=%s, will switch to STA after %d s", saved_ssid, WIFI_CONNECT_DELAY_MS / 1000);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static void start_provision_server(void)
{
    if (provision_httpd) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = PROVISION_HTTP_PORT;
    cfg.ctrl_port = PROVISION_HTTP_PORT + 1;

    httpd_start(&provision_httpd, &cfg);

    httpd_uri_t root = {"/", HTTP_GET, root_handler, .user_ctx = NULL};
    httpd_uri_t wifi = {"/wifi", HTTP_POST, wifi_post_handler, .user_ctx = NULL};

    httpd_register_uri_handler(provision_httpd, &root);
    httpd_register_uri_handler(provision_httpd, &wifi);
}

static void stop_provision_server(void)
{
    if (provision_httpd) {
        httpd_stop(provision_httpd);
        provision_httpd = NULL;
    }
}

/* =====================================================
 * WIFI
 * ===================================================== */
static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        sta_has_ip = true;
        stop_provision_server();
        start_stream_server();
        start_mdns();
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Camera stream: http://" IPSTR ":%d/stream",
                 IP2STR(&event->ip_info.ip), (int)STREAM_HTTP_PORT);
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_has_ip = false;
        stop_mdns();
        stop_stream_server();
        force_ap = true;
    }
}

static void wifi_task(void *arg)
{
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT,
                               ESP_EVENT_ANY_ID,
                               wifi_event_handler,
                               NULL);
    esp_event_handler_register(IP_EVENT,
                               IP_EVENT_STA_GOT_IP,
                               wifi_event_handler,
                               NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_remote_init(&cfg);

    /* AP with no password so phone/PC can connect for provisioning (avoids "denied access") */
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, "DentalS3_Provision", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    /* Try saved credentials first; only start AP if none or Boot forced later */
    bool have_saved = load_wifi_from_nvs();
    if (have_saved && saved_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Saved WiFi found: %s, connecting (Boot long-press to re-provision)", saved_ssid);
        esp_wifi_remote_set_mode(WIFI_MODE_STA);
        wifi_config_t sta_cfg = {0};
        strcpy((char *)sta_cfg.sta.ssid, saved_ssid);
        strcpy((char *)sta_cfg.sta.password, saved_pass);
        esp_wifi_remote_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_remote_start();
        esp_wifi_remote_connect();
    } else {
        esp_wifi_remote_set_mode(WIFI_MODE_AP);
        esp_err_t ap_ret = esp_wifi_remote_set_config(WIFI_IF_AP, &ap_cfg);
        if (ap_ret != ESP_OK)
            ESP_LOGW(TAG, "AP set_config: %s (co-proc may use default)", esp_err_to_name(ap_ret));
        esp_wifi_remote_start();
        ESP_LOGI(TAG, "AP started: SSID=DentalS3_Provision, open. Connect and open http://192.168.4.1");
        start_provision_server();
    }

    while (1) {
        if (provisioning_done) {
            provisioning_done = false;
            stop_provision_server();

            ESP_LOGI(TAG, "Provisioning complete. Waiting %d seconds before connecting to %s...",
                     (int)(WIFI_CONNECT_DELAY_MS / 1000), saved_ssid);
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_DELAY_MS));

            wifi_config_t sta_cfg = {0};
            strcpy((char *)sta_cfg.sta.ssid, saved_ssid);
            strcpy((char *)sta_cfg.sta.password, saved_pass);

            ESP_LOGI(TAG, "Connecting to WiFi: %s", saved_ssid);
            esp_wifi_remote_stop();
            esp_wifi_remote_set_mode(WIFI_MODE_STA);
            esp_wifi_remote_set_config(WIFI_IF_STA, &sta_cfg);
            esp_wifi_remote_start();
            esp_wifi_remote_connect();
        }

        if (force_ap) {
            force_ap = false;
            stop_stream_server();
            esp_wifi_remote_stop();
            esp_wifi_remote_set_mode(WIFI_MODE_AP);
            esp_wifi_remote_set_config(WIFI_IF_AP, &ap_cfg);
            esp_wifi_remote_start();
            start_provision_server();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/******************** MAIN ********************/
void app_main(void)
{
    nvs_flash_init();

    /* Reduce UVC/USB log spam (frame errors, EP Alloc on disconnect) for clearer serial output */
    esp_log_level_set("uvc-isoc", ESP_LOG_ERROR);
    esp_log_level_set("uvc",     ESP_LOG_WARN);
    esp_log_level_set("USB HOST", ESP_LOG_WARN);
    esp_log_level_set("USBH",    ESP_LOG_WARN);

    gpio_set_direction(USB_PWR_CTRL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(USB_PWR_CTRL_GPIO, 1);

    buttons_init();
    pwm_init();

    motor_cmd_queue = xQueueCreate(MOTOR_CMD_QUEUE_LEN, sizeof(uint32_t));
    xTaskCreate(motor_control_task, "motor_ctrl", 2048, NULL, 8, NULL);
    
    /* Test motor GPIOs on startup to verify signals */
    vTaskDelay(pdMS_TO_TICKS(1000));  /* Wait for system to stabilize */
    test_motor_gpios();
    
    mpu_init();

    frame_queue = xQueueCreate(16,
                              sizeof(uvc_host_frame_t *));
    capture_mutex = xSemaphoreCreateMutex();

    usb_host_config_t host_cfg = {.skip_phy_setup = false};
    usb_host_install(&host_cfg);

    uvc_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 8192,
        .create_background_task = true
    };
    uvc_host_install(&drv_cfg);

    xTaskCreate(usb_lib_task, "usb", 4096, NULL, 15, NULL);
    xTaskCreate(camera_task, "camera", 12000, NULL, 10, NULL);
    xTaskCreate(wifi_task, "wifi", 4096, NULL, 6, NULL);
    xTaskCreate(mpu_task, "mpu", 4096, NULL, 5, NULL);
    xTaskCreate(motor_log_task, "motor_log", 2048, NULL, 3, &motor_log_task_handle);

    ESP_LOGI(TAG, "SYSTEM READY");
    ESP_LOGI(TAG, "2-button: Pressure=GPIO%d, Multi=GPIO%d (Camera: capture | Motor: Off→50%%→80%%→100%%→Off)", BTN_PRESSURE_GPIO, BTN_MULTI_GPIO);
    ESP_LOGI(TAG, "Motor GPIOs: ENA=GPIO%d, IN1=GPIO%d, IN2=GPIO%d", MOTOR_ENA_GPIO, MOTOR_IN1_GPIO, MOTOR_IN2_GPIO);
}
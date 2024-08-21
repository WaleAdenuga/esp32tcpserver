#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include "mdns.h"
// #include "mdns.h"
/* define configuration data, it can also be changed via menuconfig for project configuration */

#define WIFI_SSID "Oluochi"
#define WIFI_PASS ""
#define MAX_STATION_CONNECTIONS 5

#define PORT 5541

static const char *TAG_AP = "ap_wifi_tag";
static const char *TAG_STA = "sta_wifi_tag";
static const char *TAG_TCP = "tcp_wifi_tag";
static const char *TAG_APP = "esp_task_tag";

typedef struct {
    esp_ip4_addr_t addr;
    uint8_t mac[6];
}peer_info_t;

char *received_ip_from_ap;

#define MAX_PEERS 2
peer_info_t peer_list[MAX_PEERS];
int peer_count = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0

static int s_retry_num = 0;

// hybrid approach - app scans for network and provides ssid:password 
// we scan here - perform a quick scan to validate if the network is available and send connection status back to android app
int wifi_scan(const char *ssid_to_find, const char *password) {

    ESP_ERROR_CHECK(esp_wifi_stop());

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // scan for 10 devices
    bool ssid_found = false;
    uint16_t number = 10;
    wifi_ap_record_t ap_info[number];
    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(ap_info));

    // start the scan and block until it's done
    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
    ESP_LOGI(TAG_APP, "Found %d APs. Actual AP number ap_info holds = %u", ap_count, number);

    // check if the desired SSID is found in the scan results
    for (int i = 0; i< ap_count; i++) {
        if (strcmp((char *) ap_info[i].ssid, ssid_to_find) == 0) {
            ESP_LOGI(TAG_APP, "Was able to notice %s", ssid_to_find);
            ssid_found = true;
            break;
        }
    }

    if (ssid_found) {
        ESP_LOGI(TAG_APP, "SSID %s found, attempting to connect...", ssid_to_find);

        // Configure and connect to the network using the provided SSID and password coming from the app
        wifi_config_t wifi_config = {
            .sta = {
                .ssid = "",
                .password = "",
             },
        };
        strcpy((char *)wifi_config.sta.ssid, ssid_to_find);
        strcpy((char *)wifi_config.sta.password, password);
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.failure_retry_cnt = 10; 
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        ESP_LOGI(TAG_APP, "sta ssid: %s", wifi_config.sta.ssid);
        ESP_LOGI(TAG_APP, "sta password: %s", wifi_config.sta.password);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               portMAX_DELAY);

        /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
         * happened. */
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG_APP, "connected to ap SSID:%s password:%s",
                     ssid_to_find, password);
            return 0;
        } else {
            ESP_LOGE(TAG_APP, "Failed to connect to ap SSID:%s", ssid_to_find);
        }
    } else {
        ESP_LOGI(TAG_APP, "SSID %s not found", ssid_to_find);
        return -1;
        // Send a failure message back to the client if needed
    }
    return 0;
}

static void discover_devices(void) {
    ESP_LOGI(TAG_APP, "in discover devices");
    vTaskDelay(5000/portTICK_PERIOD_MS);
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_http", "_tcp", 3000, 20, &results);
    if (err) {
        ESP_LOGE(TAG_APP, "Query Failed: %d", err);
        return;
    }

    if (results) {
        ESP_LOGI(TAG_APP, "in discover devices results");
        mdns_result_t *r = results;
        while (r) {
            ESP_LOGI(TAG_APP, "Found service: %s", r->instance_name);
            ESP_LOGI(TAG_APP, "Hostname: %s", r->hostname);
            // ESP_LOGI(TAG_APP, "IP: " IPSTR, IP2STR(&r->addr->addr));
            ESP_LOGI(TAG_APP, "Port: %d", r->port);

            if (r->txt_count)
            {
                ESP_LOGI(TAG_APP, "TXT records:");
                for (size_t i = 0; i < r->txt_count; i++)
                {
                    ESP_LOGI(TAG_APP, "  %s=%s", r->txt[i].key, r->txt[i].value);
                }
            }

            char *ip_str;
            // inet_ntop(AF_INET, &r->addr.addr.u_addr.ip4, ip_str, sizeof(ip_str));
            ip_str = ipaddr_ntoa((const ip_addr_t*) &(r->addr->addr));    
            ESP_LOGI(TAG_APP, "IP Address: %s", ip_str);
            r = r->next;
        }
        mdns_query_results_free(results);
    } else {
        ESP_LOGE(TAG_APP, "No services found");
    }
    ESP_LOGI(TAG_APP, "Finished discover devices");
}

/* static void mdns_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == MDNS_EVENT) {
        if (event_id == MDNS_EVENT_ANY) {
            ESP_LOGI("MDNS", "MDNS_EVENT_ANY occurred");
        } else if (event_id == MDNS_EVENT_ADD) {
            mdns_add_event_t *data = (mdns_add_event_t *)event_data;
            ESP_LOGI("MDNS", "Service added: %s", data->hostname);
        } else if (event_id == MDNS_EVENT_REMOVE) {
            mdns_remove_event_t *data = (mdns_remove_event_t *)event_data;
            ESP_LOGI("MDNS", "Service removed: %s", data->hostname);
        } else if (event_id == MDNS_EVENT_UPDATE) {
            mdns_update_event_t *data = (mdns_update_event_t *)event_data;
            ESP_LOGI("MDNS", "Service updated: %s", data->hostname);
        }
    }
} */

static void init_mdns() {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp32"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32-MDNS-AP"));

    // instance name - service type (http/ftp) - protocol(tcp/udp) - port - txt - num_items
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));

    ESP_LOGI(TAG_APP, "mDNS started with hostname: esp32");

    discover_devices();

    ESP_LOGI(TAG_APP, "after discover devices");
}


/* 
    Message format:
    MessageIdentifier:Message

    MessageIdentifiers:
    100 - Wifi ssid to connect to 
 */
static void handleClientResponse(char *rx_buf, int len, int sock_client) {
    rx_buf[len] = 0;
    ESP_LOGI(TAG_APP, "Received %d bytes: %s", len, rx_buf);

    // get the first token after the delimiters
    // this is the message identifier
    if (strlen(rx_buf) > 0) {
        char *token = strtok(rx_buf, ":");
        int message_identifier = atoi(token);
        ESP_LOGI(TAG_APP, "Message Identifier: %d", message_identifier);

        switch (message_identifier)
        {
        case 100:
            // send the ssid and password to the client
            char *ssid = strtok(NULL, ":");
            char *password = strtok(NULL, ":");

            ESP_LOGI(TAG_APP, "SSID: %s, Password: %s", ssid, password);

            if (wifi_scan(ssid, password) == 0)
            {
                ESP_LOGI(TAG_APP, "Connection to AP was successful");
                if (strlen(received_ip_from_ap) > 0)
                {
                    ESP_LOGI(TAG_APP, "before sending");
                    char send_buf[128];
                    sprintf(send_buf, "101:%s", received_ip_from_ap);
                    send(sock_client, send_buf, strlen(send_buf), 0);

                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    // send(sock_client, received_ip_from_ap, strlen(received_ip_from_ap), 0);
                    ESP_LOGI(TAG_APP, "after sending");
                }
            };
            break;
        default:
            ESP_LOGI(TAG_APP, "Unknown Message Identifier");
            break;
        }
    }
}

// Add tcp server and send it to the ip address maybe
static void tcp_server_task(void *pvParameters) {

    int keepAlive = 1;
    int keepIdle = 5;
    int keepInterval = 5;
    int keepCount = 3;

    char addr_str[128];

    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    
    // setting up socket for IPv4
    struct sockaddr_in dest_addr;
    // convert ip address from string to binary representation

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY); // socket should be able to accept connections on any of the IP addresses the device might have
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG_TCP, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // helps in manipulating options for the socket referred by the descriptor listen_sock, helps in reuse of address and port
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ESP_LOGI(TAG_TCP, "Socket created");

    // bind socket to the address and port specified in dest_addr
    int err = bind(listen_sock, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
    ESP_LOGI(TAG_TCP, "");
    if (err!= 0)  {
        ESP_LOGE(TAG_TCP, "Unable to bind socket: errno %d", errno);
        ESP_LOGE(TAG_TCP, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }

    ESP_LOGI(TAG_TCP, "Socket bound, port %d", PORT);

    // puts server in passive mode, where it waits for client to approach the server to make a connection, backlog defines maximum length to which the queue of pending connections may grow
    err = listen(listen_sock, 1);
    if (err!= 0) {
        ESP_LOGE(TAG_TCP, "Unable to listen: errno %d", errno);
        goto CLEAN_UP;
    }

    ESP_LOGI(TAG_TCP, "Socket listening");
    ESP_LOGI(TAG_TCP, "IP of server address: %s:%d", inet_ntoa(dest_addr.sin_addr), ntohs(dest_addr.sin_port));
    while(1) {
        struct sockaddr_in source_addr; // Client address
        socklen_t addr_len = sizeof(source_addr);
        int sock_client = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock_client < 0) {
            ESP_LOGE(TAG_TCP, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock_client, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock_client, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock_client, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock_client, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        ESP_LOGI(TAG_TCP, "Received connection from IP: %s", inet_ntoa(source_addr.sin_addr));
        // converts ipv4 address to a string
        inet_ntoa_r(((struct sockaddr_in*)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG_TCP, "Accepted connection from: %s:%d", addr_str, ntohs(source_addr.sin_port));

        ESP_LOGI(TAG_TCP, "before do_retransmit loop");
        int len;
        char rx_buf[128];

        // been a while since i did do-while, this runs at least once
        do
        {
            ESP_LOGI(TAG_TCP, "Inside do_retransmit loop");
            len = recv(sock_client, rx_buf, sizeof(rx_buf) - 1, 0);
            ESP_LOGI(TAG_TCP, "Inside do_retransmit loop, len is %d", len);
            if (len < 0)
            {
                ESP_LOGE(TAG_TCP, "recv failed: errno %d", errno);
            }
            else if (len == 0)
            {
                ESP_LOGI(TAG_TCP, "Connection closed");
            }
            else
            {
                handleClientResponse(rx_buf, len, sock_client);
                

                /*             int to_write = len;
                            while (to_write > 0) {
                                int written = send(listen_sock, rx_buf + (len - to_write), to_write, 0);
                                if (written < 0) {
                                    ESP_LOGE(TAG_AP, "Error occurred during sending: errno %d", errno);
                                    // Failed to retransmit, giving up
                                    return;
                                }
                                to_write -= written;
                            } */
            }
        } while (len > 0);

        ESP_LOGI(TAG_TCP, "after do_retransmit loop, before shutdown");


        shutdown(sock_client, 0);
        close(sock_client);
        ESP_LOGI(TAG_TCP, "closed and shut down socket");
        
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);

}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {

    switch (event_id){
    case WIFI_EVENT_AP_STACONNECTED:
        wifi_event_ap_staconnected_t *connect_event = (wifi_event_ap_staconnected_t *)event_data;
        // MAC addresses are unique, do not expire or change on their own
        ESP_LOGI(TAG_AP, "Station " MACSTR " connected, AID=%d", MAC2STR(connect_event->mac), connect_event->aid);

        for (int i = 0; i < peer_count; i++)
        {
            ESP_LOGI(TAG_AP, "Station " MACSTR " in peer list with ap staconnected: " IPSTR, MAC2STR(peer_list[i].mac), IP2STR(&peer_list[i].addr));
        }
    break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        wifi_event_ap_stadisconnected_t *disconnect_event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" disconnected, AID=%d", MAC2STR(disconnect_event->mac), disconnect_event->aid);

        for (int i = 0; i<peer_count; i++) {
            if (memcmp(peer_list[i].mac, disconnect_event->mac, sizeof(disconnect_event->mac)) == 0) {
                // remove the peer from the list
                memmove(&peer_list[i], &peer_list[i + 1], (peer_count - i - 1) * sizeof(peer_info_t));
                peer_count--;
                break;
            }
        }
    break;

    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG_STA, "esp32 Station started");
    break;
    
    case WIFI_EVENT_WIFI_READY:
        ESP_LOGI(TAG_STA, "esp32 WiFi ready");
    break;

    case WIFI_EVENT_SCAN_DONE:
        ESP_LOGI(TAG_STA, "esp32 Scan done");
    break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG_STA, "esp32 station Connected");
    break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_retry_num < 20) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_STA, "retry to connect to the AP");
        } else {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG_STA, "esp32 station disconnected");
    break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        ESP_LOGI(TAG_AP, "Assigned IP to client: " IPSTR, IP2STR(&event->ip));
        ESP_LOGI(TAG_AP, "Station "MACSTR" connected", MAC2STR(event->mac));

        if (peer_count < MAX_PEERS) {
            memcpy(peer_list[peer_count].mac, event->mac, sizeof(event->mac));
            peer_list[peer_count].addr = event->ip;
            peer_count++;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        // sprintf(received_ip_from_ap, IPSTR, IP2STR(&event->ip_info.ip));
        received_ip_from_ap = ipaddr_ntoa((const ip_addr_t*) &(event->ip_info.ip));
        ESP_LOGI(TAG_STA, "received_ip_from_ap: %s", received_ip_from_ap);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // initialize mdns
        init_mdns();
    }
}


void wifi_init_softap() {
    // initialize the underlying tcp stack
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    // register wifi handler to the default loop
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, netif, NULL));

    // register ip event handler to the desault loop
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    wifi_config_t config = {
        .ap = { 
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 5, //2.4ghz has channels 1 - 14, solo1 only supports 2.4ghz
            .password = WIFI_PASS,
            .max_connection = MAX_STATION_CONNECTIONS,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
// configuration for protected management frame
            .pmf_cfg = {
                    .required = true,
            },
        }
    };

    if (strlen(WIFI_PASS) == 0) {
        config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_AP, "Wi-Fi AP initialized with SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}



void app_main(void) {
    // Initialize NVS - non volatile storage - store data in flash memory that's retained across resets and power cycles
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG_APP, "Initializing Wi-Fi...");
    wifi_init_softap();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 1, NULL);
}
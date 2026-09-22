#include "wol.h"
#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "esp_log.h"

static const char *TAG = "wol";

static bool parse_mac(const char *mac_str, uint8_t *mac_bytes) {
    if (mac_str == NULL || mac_bytes == NULL) return false;

    unsigned int b[6];
    int count = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                       &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (count != 6) {
        count = sscanf(mac_str, "%x-%x-%x-%x-%x-%x",
                       &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
    if (count != 6) return false;

    for (int i = 0; i < 6; i++) {
        mac_bytes[i] = (uint8_t)b[i];
    }
    return true;
}

bool wol_send_magic_packet(const char *mac_address_str) {
    uint8_t target_mac[6];
    if (!parse_mac(mac_address_str, target_mac)) {
        ESP_LOGE(TAG, "Invalid MAC address format: '%s'", mac_address_str ? mac_address_str : "NULL");
        return false;
    }

    // Build 102-byte Magic Packet: 6 bytes 0xFF + 16x MAC
    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + (i * 6), target_mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create WoL socket: errno %d", errno);
        return false;
    }

    int broadcast_en = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_en, sizeof(broadcast_en));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(WOL_UDP_PORT);
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    int err = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    close(sock);

    if (err < 0) {
        ESP_LOGE(TAG, "Error sending WoL packet: errno %d", errno);
        return false;
    }

    ESP_LOGI(TAG, "Successfully broadcasted Wake-on-LAN Magic Packet for MAC: %s", mac_address_str);
    return true;
}

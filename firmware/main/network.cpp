#include "network.hpp"

#include <cstring>

#include "app.hpp"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

namespace fw {

namespace {

const char* TAG = "net";
int g_port = 7720;

void on_eth(void*, esp_event_base_t, int32_t id, void*) {
    RigLock lock;
    if (id == ETHERNET_EVENT_CONNECTED) {
        app().link.cable = true;
        app().log(leota::LogKind::Network, "Ethernet cable connected.");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        app().link.cable = false;
        app().log(leota::LogKind::Network, "Ethernet cable disconnected.");
    }
}

void on_ip(void*, esp_event_base_t, int32_t, void* data) {
    const auto* e = static_cast<const ip_event_got_ip_t*>(data);
    char buf[20];
    esp_ip4addr_ntoa(&e->ip_info.ip, buf, sizeof buf);
    ESP_LOGI(TAG, "Address %s, SCMP on UDP port %d.", buf, g_port);
    RigLock lock;
    app().link.own_address = buf;
    app().log(leota::LogKind::Network, std::string("Address ") + buf + ".");
}

void start_ethernet(const leota::Network& net) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&nc);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = CONFIG_LEOTA_ETH_MDC_PIN;
    emac.smi_gpio.mdio_num = CONFIG_LEOTA_ETH_MDIO_PIN;
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_LEOTA_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_LEOTA_ETH_PHY_RST_PIN;
    esp_eth_phy_t* phy = esp_eth_phy_new_ip101(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, nullptr));
    if (!net.dhcp) {
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
        esp_netif_ip_info_t ip = {};
        ip.ip.addr = esp_netif_htonl(net.address);
        ip.netmask.addr = esp_netif_htonl(net.netmask);
        ip.gw.addr = esp_netif_htonl(net.gateway);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));
        RigLock lock;
        app().link.own_address = leota::address_text(net.address);
    }
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

// SCMP over UDP: every datagram goes to the same ScmpServer the PC simulator uses. Status
// goes to whoever last sent a hello or a heartbeat.
void scmp_task(void*) {
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in bind_to = {};
    bind_to.sin_family = AF_INET;
    bind_to.sin_port = htons(static_cast<uint16_t>(g_port));
    bind_to.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, reinterpret_cast<sockaddr*>(&bind_to), sizeof bind_to) != 0) {
        ESP_LOGE(TAG, "Couldn't open UDP port %d for SCMP.", g_port);
        vTaskDelete(nullptr);
        return;
    }
    timeval tv = {0, 10'000};  // wake every 10 ms to send status when it's due
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in status_to = {};
    bool have_status_to = false;
    static char buf[2048];
    for (;;) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof from;
        const int n = recvfrom(s, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        std::string reply, status;
        {
            RigLock lock;
            App& a = app();
            const double t = now_s();
            if (n > 0) {
                reply = a.scmp->handle(std::string_view(buf, static_cast<size_t>(n)), t);
                const std::string& type = a.scmp->last_type();
                a.link.last_heard = t;
                if (type == "hello" || type == "heartbeat") {
                    char addr[24];
                    inet_ntoa_r(from.sin_addr, addr, sizeof addr);
                    const std::string who = std::string(addr) + ":" + std::to_string(ntohs(from.sin_port));
                    if (who != a.link.from) a.log(leota::LogKind::Network, "Show Controller at " + who + ".");
                    a.link.from = who;
                    status_to = from;
                    have_status_to = true;
                }
                if (type != "heartbeat" && type != "status") ++a.link.commands;
            }
            if (have_status_to && a.scmp->status_due(t)) status = a.scmp->status();
        }
        if (!reply.empty())
            sendto(s, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), from_len);
        if (!status.empty())
            sendto(s, status.data(), status.size(), 0, reinterpret_cast<sockaddr*>(&status_to), sizeof status_to);
    }
}

}  // namespace

void network_start(const leota::Network& net) {
    g_port = net.scmp_port;
    start_ethernet(net);
    xTaskCreatePinnedToCore(scmp_task, "scmp", 6144, nullptr, 8, nullptr, 0);
}

}  // namespace fw

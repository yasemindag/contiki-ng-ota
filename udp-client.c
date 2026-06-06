#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "lib/crc16.h"

#include "firmware_data.h"

#include "sys/node-id.h"
#include "sys/log.h"
#define LOG_MODULE "Client"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define SEND_INTERVAL (3 * CLOCK_SECOND)

#define CHUNK_SIZE 64
#define TOTAL_FW_SIZE (sizeof(new_firmware_z1))
#define TOTAL_CHUNKS ((TOTAL_FW_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE)

typedef struct {
  uint16_t seq_no;
  uint16_t length;
  uint16_t crc;
  uint8_t payload[CHUNK_SIZE];
} ota_packet_t;

typedef struct {
  uint16_t ack_seq_no;
  uint8_t status;
} ota_ack_t;

static struct simple_udp_connection udp_conn;
static uint16_t current_seq_no = 0;
static bool transfer_complete = false;
static uint16_t full_image_crc = 0;
static bool got_ack = false;

PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);

static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  if(datalen == sizeof(ota_ack_t)) {
    ota_ack_t *ack = (ota_ack_t *)data;

    if(ack->status == 1) {
        if(ack->ack_seq_no == current_seq_no && current_seq_no < TOTAL_CHUNKS) {
            LOG_INFO("ACK alindi: Blok %u basarili.\n", ack->ack_seq_no);
            current_seq_no++;
            got_ack = true;
            process_poll(&udp_client_process);

        } else if(ack->ack_seq_no == 0xFFFF && current_seq_no == TOTAL_CHUNKS) {
            LOG_INFO("EOF ACK alindi! Tum firmware basariyla gonderildi ve dogrulandi!\n");
            transfer_complete = true;
            got_ack = true;
            process_poll(&udp_client_process);
        }
    }
    else if(ack->status == 2) {
        LOG_INFO("ALICIDAN ISTEK: Blok %u eksik, aninda iletiliyor!\n", ack->ack_seq_no);
        current_seq_no = ack->ack_seq_no;
        got_ack = true;
        process_poll(&udp_client_process);
    }
    else if(ack->status == 0) {
        LOG_INFO("HATA/BOZUK PAKET: Blok %u reddedildi, aninda tekrar iletiliyor!\n", ack->ack_seq_no);
        current_seq_no = ack->ack_seq_no;
        got_ack = true;
        process_poll(&udp_client_process);
    }
    else if(ack->status == 3) {
        LOG_INFO("FATAL ERROR: Alici tam imajin bozuk oldugunu bildirdi. OTA iptal ediliyor!\n");
        transfer_complete = true;
        got_ack = true;
        process_poll(&udp_client_process);
    }
    else if(ack->status == 4) {
        LOG_INFO("SUNUCU HATASI: Depolama alani doldu! Aktarim sonlandiriliyor.\n");
        transfer_complete = true;
        got_ack = true;
        process_poll(&udp_client_process);
    }
  }
}

static uint16_t calculate_full_file_crc() {
    uint16_t total_crc = 0;
    for(uint16_t j = 0; j < TOTAL_CHUNKS; j++) {
        uint16_t len = (j == TOTAL_CHUNKS - 1) ? (TOTAL_FW_SIZE % CHUNK_SIZE) : CHUNK_SIZE;
        if(len == 0) len = CHUNK_SIZE;
        total_crc = crc16_data(&new_firmware_z1[j * CHUNK_SIZE], len, total_crc);
    }
    return total_crc;
}

PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  uip_ipaddr_t dest_ipaddr;
  static ota_packet_t packet;

  PROCESS_BEGIN();

  //Sadece ID=2 olan cihaz bu kodu calistirsin digerleri uyusun
  if(node_id != 2) {
      PROCESS_EXIT();
  }

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, udp_rx_callback);

  LOG_INFO("Gonderici dugum baslatildi. ROM'dan dogrudan aktarim hazir.\n");
  full_image_crc = calculate_full_file_crc();
  LOG_INFO("Tam Imaj CRC'si hesaplandi: 0x%04X (Boyut: %u Bayt)\n", full_image_crc, (uint16_t)TOTAL_FW_SIZE);

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);

  while(!transfer_complete) {

    // RPL Agi kuruldu mu kontrolu
    if(NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

        if(current_seq_no < TOTAL_CHUNKS) {
            packet.seq_no = current_seq_no;
            packet.length = (current_seq_no == TOTAL_CHUNKS - 1) ? (TOTAL_FW_SIZE % CHUNK_SIZE) : CHUNK_SIZE;
            if(packet.length == 0) packet.length = CHUNK_SIZE;

            // Veriyi ROM'dan kopyala
            memcpy(packet.payload, &new_firmware_z1[current_seq_no * CHUNK_SIZE], packet.length);
            packet.crc = crc16_data(packet.payload, packet.length, 0);

            LOG_INFO("Paket %u gonderiliyor. (Uzunluk: %u, CRC16: 0x%04X)\n", packet.seq_no, packet.length, packet.crc);

        } else if(current_seq_no == TOTAL_CHUNKS) {
            packet.seq_no = 0xFFFF;
            packet.length = 0;
            packet.crc = full_image_crc;

            uint32_t fw_size_meta = (uint32_t)TOTAL_FW_SIZE;
            memcpy(packet.payload, &fw_size_meta, sizeof(fw_size_meta));

            LOG_INFO("Tum parcalar bitti. EOF Sinyali ve Metadata (Boyut: %lu) gonderiliyor.\n", fw_size_meta);
        }

        simple_udp_sendto(&udp_conn, &packet, sizeof(ota_packet_t), &dest_ipaddr);

        got_ack = false;
        etimer_set(&periodic_timer, SEND_INTERVAL);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer) || got_ack);

        if(!got_ack && etimer_expired(&periodic_timer)) {
            LOG_INFO("TIMEOUT! Blok %u kayboldu. Yeniden iletiliyor...\n", current_seq_no);
        }

    } else {
      LOG_INFO("Ag hazir degil, RPL aginin (ID:1) kurulmasi bekleniyor...\n");
      etimer_set(&periodic_timer, SEND_INTERVAL);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
    }
  }

  PROCESS_END();
}
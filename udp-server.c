#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include <stdint.h>
#include <string.h>
#include "lib/crc16.h"

#include "sys/log.h"
#define LOG_MODULE "Server"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define CHUNK_SIZE 64

#define MAX_FW_SIZE 131072
#define MAX_CHUNKS ((MAX_FW_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE)

// Bitmask kapasitesi maksimuma göre ayrılır
#define BITMAP_SIZE ((MAX_CHUNKS / 8) + 1)
static uint8_t received_chunks[BITMAP_SIZE] = {0};

#define SET_CHUNK_RECEIVED(seq) (received_chunks[(seq) / 8] |= (1 << ((seq) % 8)))
#define IS_CHUNK_RECEIVED(seq)  (received_chunks[(seq) / 8] & (1 << ((seq) % 8)))

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
static int fw_fd = -1;
static uint16_t highest_seq_no = 0;
static bool transfer_aborted = false;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);

static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  if(transfer_aborted) return;

  if(datalen == sizeof(ota_packet_t)) {
    ota_packet_t *packet = (ota_packet_t *)data;
    ota_ack_t ack;

    // --- BÜTÜN İMAJ DOĞRULAMASI ---
    if(packet->seq_no == 0xFFFF) {

        //Client'ın payload içine gizlediği dosya boyutunu çıkar
        uint32_t actual_fw_size;
        memcpy(&actual_fw_size, packet->payload, sizeof(actual_fw_size));
        uint16_t actual_total_chunks = (actual_fw_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        if(actual_fw_size > MAX_FW_SIZE) {
            LOG_INFO("HATA: Gelen dosya sunucu kapasitesini (131KB) asiyor!\n");
            transfer_aborted = true;
            ack.ack_seq_no = 0xFFFF;
            ack.status = 4; // Disk kapasitesi yetersiz
            simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
            return;
        }

        //Eksik paket taramasını DİNAMİK CHUNK boyutuna göre yap
        uint16_t missing_block = 0xFFFF;
        for(uint16_t i = 0; i < actual_total_chunks; i++) {
            if(!IS_CHUNK_RECEIVED(i)) {
                missing_block = i;
                break;
            }
        }

        if(missing_block != 0xFFFF) {
            LOG_INFO("UYARI: EOF Sinyali geldi ama Blok %u eksik! Aktif NACK gonderiliyor.\n", missing_block);
            ack.ack_seq_no = missing_block;
            ack.status = 2;
            simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
            return;
        }

        LOG_INFO("Eksik blok yok (Boyut: %lu). Tam CRC dogrulamasi basliyor...\n", actual_fw_size);

        cfs_close(fw_fd);
        fw_fd = cfs_open("new_fw.bin", CFS_READ);

        uint16_t downloaded_file_crc = 0;
        uint8_t buf[CHUNK_SIZE];

        //CRC dinamik dosya boyutuna göre hesaplanıyor
        for(uint16_t j = 0; j < actual_total_chunks; j++) {
            int len = CHUNK_SIZE;
            if (j == actual_total_chunks - 1) {
                len = actual_fw_size % CHUNK_SIZE;
                if(len == 0) len = CHUNK_SIZE;
            }
            cfs_read(fw_fd, buf, len);
            downloaded_file_crc = crc16_data(buf, len, downloaded_file_crc);
        }

        if(downloaded_file_crc == packet->crc) {
            LOG_INFO("MUKEMMEL! Tam imaj dogrulamasi basarili (CRC: 0x%04X)\n", downloaded_file_crc);
            LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
            ack.ack_seq_no = 0xFFFF;
            ack.status = 1;
        } else {
            LOG_INFO("KRITIK HATA: Dosya butunlugu bozuk! (Beklenen: 0x%04X, Hesaplanan: 0x%04X)\n", packet->crc, downloaded_file_crc);
            LOG_INFO("Bozuk firmware siliniyor ve transfer iptal ediliyor.\n");

            cfs_close(fw_fd);
            cfs_remove("new_fw.bin");
            fw_fd = -1;

            ack.ack_seq_no = 0xFFFF;
            ack.status = 3;
            simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
            return;
        }

        cfs_close(fw_fd);
        fw_fd = -1;
        simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
        return;
    }

    if(packet->length > CHUNK_SIZE) {
        return;
    }

    uint16_t calc_crc = crc16_data(packet->payload, packet->length, 0);

    // Gelen paket bizim kapasitemiz dahilinde mi
    if(calc_crc == packet->crc && packet->seq_no < MAX_CHUNKS) {

        if(!IS_CHUNK_RECEIVED(packet->seq_no)) {

            if(fw_fd >= 0) {
                cfs_seek(fw_fd, packet->seq_no * CHUNK_SIZE, CFS_SEEK_SET);
                int written_bytes = cfs_write(fw_fd, packet->payload, packet->length);

                if(written_bytes != packet->length) {
                    transfer_aborted = true;
                    LOG_INFO("Depolama alani doldu, OTA guvenli sekilde sonlandirildi.\n");

                    uint16_t successful_blocks = packet->seq_no;
                    uint16_t partial_crc = 0;
                    uint8_t buf[CHUNK_SIZE];

                    cfs_seek(fw_fd, 0, CFS_SEEK_SET);
                    for(uint16_t i = 0; i < successful_blocks; i++) {
                        cfs_read(fw_fd, buf, CHUNK_SIZE);
                        partial_crc = crc16_data(buf, CHUNK_SIZE, partial_crc);
                    }

                    LOG_INFO("Basariyla yazilan blok sayisi: %u, Kismi CRC: 0x%04X\n", successful_blocks, partial_crc);

                    cfs_close(fw_fd);
                    fw_fd = -1;

                    ack.ack_seq_no = packet->seq_no;
                    ack.status = 4;
                    simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
                    return;
                }
            }

            SET_CHUNK_RECEIVED(packet->seq_no);

            if(packet->seq_no >= highest_seq_no) {
                highest_seq_no = packet->seq_no + 1;
            }
            LOG_INFO("Blok %u dogrulandi ve diske yazildi.\n", packet->seq_no);
        }

        uint16_t missing_block = 0xFFFF;
        for(uint16_t i = 0; i < highest_seq_no; i++) {
            if(!IS_CHUNK_RECEIVED(i)) {
                missing_block = i;
                break;
            }
        }

        if(missing_block != 0xFFFF) {
            LOG_INFO("UYARI: Arada kaybolan blok tespit edildi! Blok %u tekrar isteniyor.\n", missing_block);
            ack.ack_seq_no = missing_block;
            ack.status = 2;
        } else {
            ack.ack_seq_no = packet->seq_no;
            ack.status = 1;
        }

    } else {
        LOG_INFO("HATA: Blok %u CRC16 hatasi!\n", packet->seq_no);
        ack.ack_seq_no = packet->seq_no;
        ack.status = 0;
    }

    simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
  }
}

PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  cfs_remove("new_fw.bin");
  fw_fd = cfs_open("new_fw.bin", CFS_WRITE);

  if(fw_fd < 0) {
      LOG_INFO("HATA: Disk (CFS) erisimi saglanamadi!\n");
  } else {
      LOG_INFO("CFS hazir, yeni firmware bekleniyor...\n");
  }

  memset(received_chunks, 0, sizeof(received_chunks));

  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL, UDP_CLIENT_PORT, udp_rx_callback);

  while(1) {
      PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
# BIL304 İşletim Sistemleri — OTA Firmware Güncelleme Projesi
**Platform:** Contiki-NG v4.8, Cooja Simülatörü, TI Z1 Mote (MSP430F2617 + CC2420)

---
YouTube Linki:
---

## 1. Projeye Genel Bakış

Bu projede Cooja simülatörü üzerinde çalışan üç düğümlü bir telsiz duyarga ağında OTA (Over-the-Air / Havadan Güncelleme) mekanizması gerçeklenmiştir. Amaç, `new-firmware.z1` dosyasını ağ üzerinden güvenilir biçimde iletmek, alıcı düğümde bütünlük denetiminden geçirmek ve kalıcı depolama alanına yazmaktır.

**Düğüm rolleri:**

| Düğüm ID | Firmware | Rol |
|----------|---------|-----|
| 1 | `udp-server.z1` | OTA alıcı + RPL DODAG kök düğümü |
| 2 | `udp-client.z1` | OTA gönderici |
| 3 | `udp-client.z1` | Yönlendirici (relay) — mesaj göndermez |

**Neden 2 ve 3 numaralı düğüme aynı firmware yüklenmesine rağmen sadece düğüm 2 gönderim yapıyor?**

Her iki düğüme aynı `udp-client.z1` yüklenmesine rağmen, kod içinde `node_id` kütüphanesi kullanılarak cihazın ID değeri sorgulanır. Yalnızca ID değeri 2 olan düğüm OTA gönderimini başlatır; ID değeri 3 olan düğüm bu bloktan çıkarak yalnızca RPL yönlendirici olarak görev yapar.

```c
/* udp-client.c — satır 70 */
if(node_id != 2) {
    PROCESS_EXIT();   // ID:3 olan düğüm burada durur, relay görevi görür
}
```

Bu yaklaşım sayesinde her düğüm için ayrı firmware derleme zahmetinden kurtulunmuş olur.

---

## 2. Aktarım Mimarisi ve Protokol Tasarımı

### 2.1 Paket Yapısı

Firmware verisi sabit boyutlu bloklara bölünür. Her blok aşağıdaki yapıyla paketlenir:

```c
typedef struct {
  uint16_t seq_no;       // Blok sıra numarası (0'dan başlar)
  uint16_t length;       // Bu bloktaki gerçek veri uzunluğu (bayt)
  uint16_t crc;          // Bu bloğun CRC16 doğrulama değeri
  uint8_t  payload[64];  // Ham firmware verisi
} ota_packet_t;          // Toplam boyut: 6 + 64 = 70 bayt
```

ACK/NACK mesajları için ayrı küçük bir yapı kullanılır:

```c
typedef struct {
  uint16_t ack_seq_no;   // Hangi bloğa yanıt verildiği
  uint8_t  status;       // 0=CRC hata, 1=ACK, 2=NACK, 3=imaj bozuk, 4=disk dolu
} ota_ack_t;             // Toplam boyut: 3 bayt
```

### 2.2 Blok Boyutu ve Toplam Blok Sayısı

Blok boyutu 64 bayt olarak belirlenmiştir. Bu değer IEEE 802.15.4 çerçeve boyutu (127 bayt), 6LoWPAN başlık overhead'i ve OTA paket başlığı (6 bayt) gözetilerek seçilmiştir.

```c
#define CHUNK_SIZE 64
#define TOTAL_FW_SIZE (sizeof(new_firmware_z1))
#define TOTAL_CHUNKS  ((TOTAL_FW_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE)
```

`new-firmware.z1` boyutu yaklaşık 129.760 bayttır. Bu durumda:

```
TOTAL_CHUNKS = (129760 + 63) / 64 = 2028 blok
```

Son blok tam 64 bayt olmayabilir; bu durum aşağıdaki kodla ele alınır:

```c
packet.length = (current_seq_no == TOTAL_CHUNKS - 1)
                ? (TOTAL_FW_SIZE % CHUNK_SIZE)
                : CHUNK_SIZE;
if(packet.length == 0) packet.length = CHUNK_SIZE; // Tam bölünme durumu
```

### 2.3 Firmware Verisinin Kaynağı

`new-firmware.z1` dosyası, `firmware_data.h` başlık dosyasında C dizisi olarak gömülüdür. Bu sayede gönderici düğüm dosyayı diskten okumak yerine doğrudan Flash (ROM) belleğinden pointer ile erişir:

```c
#include "firmware_data.h"
// new_firmware_z1[] dizisi Flash'ta statik olarak durur
memcpy(packet.payload, &new_firmware_z1[current_seq_no * CHUNK_SIZE], packet.length);
```

---

## 3. Güvenilir Aktarım Stratejisi

### 3.1 Stop-and-Wait Protokolü

Projede **Stop-and-Wait** (dur-bekle) stratejisi uygulanmıştır. Gönderici her blok için şu sırayı izler:

```
Bloğu gönder → ACK/NACK bekle → ACK geldi mi?
    → Evet: sonraki bloğa geç
    → Hayır (timeout): aynı bloğu tekrar gönder
```

Kod karşılığı:

```c
simple_udp_sendto(&udp_conn, &packet, sizeof(ota_packet_t), &dest_ipaddr);

got_ack = false;
etimer_set(&periodic_timer, SEND_INTERVAL);  // 3 saniyelik timeout
PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer) || got_ack);

if(!got_ack && etimer_expired(&periodic_timer)) {
    LOG_INFO("TIMEOUT! Blok %u kayboldu. Yeniden iletiliyor...\n", current_seq_no);
    // current_seq_no değişmedi; döngü aynı bloğu tekrar gönderir
}
```

### 3.2 Blok Bazlı CRC16 Doğrulaması

Her blok gönderilmeden önce CRC16 hesaplanır ve paket başlığına eklenir:

```c
packet.crc = crc16_data(packet.payload, packet.length, 0);
```

Alıcı taraf gelen bloğun CRC'sini yeniden hesaplar ve karşılaştırır:

```c
uint16_t calc_crc = crc16_data(packet->payload, packet->length, 0);

if(calc_crc == packet->crc && packet->seq_no < MAX_CHUNKS) {
    // Blok geçerli: diske yaz
} else {
    // CRC uyuşmuyor: reddet
    ack.status = 0;  // Gönderici bu bloğu tekrar gönderecek
}
```

### 3.3 Eksik Blok Tespiti ve Aktif NACK

Alıcı taraf bitmap veri yapısıyla hangi blokların geldiğini takip eder. Sıra dışı bir blok geldiğinde ya da bir blok atlandığında NACK (status=2) göndererek eksik bloğu ister:

```c
/* Bitmap tanımı — 131 KB için yalnızca ~256 bayt RAM harcar */
#define BITMAP_SIZE ((MAX_CHUNKS / 8) + 1)
static uint8_t received_chunks[BITMAP_SIZE] = {0};

#define SET_CHUNK_RECEIVED(seq) \
    (received_chunks[(seq) / 8] |= (1 << ((seq) % 8)))
#define IS_CHUNK_RECEIVED(seq)  \
    (received_chunks[(seq) / 8] & (1 << ((seq) % 8)))
```

Her blok alındığında eksik blok taraması yapılır:

```c
uint16_t missing_block = 0xFFFF;
for(uint16_t i = 0; i < highest_seq_no; i++) {
    if(!IS_CHUNK_RECEIVED(i)) {
        missing_block = i;
        break;
    }
}

if(missing_block != 0xFFFF) {
    ack.ack_seq_no = missing_block;
    ack.status = 2;   // NACK: bu bloğu tekrar gönder
} else {
    ack.ack_seq_no = packet->seq_no;
    ack.status = 1;   // ACK: blok tamam
}
```

Gönderici NACK aldığında `current_seq_no`'yu eksik bloğa ayarlayıp hemen gönderir:

```c
else if(ack->status == 2) {
    LOG_INFO("ALICIDAN ISTEK: Blok %u eksik, aninda iletiliyor!\n", ack->ack_seq_no);
    current_seq_no = ack->ack_seq_no;
    got_ack = true;
    process_poll(&udp_client_process);
}
```

### 3.4 ACK / NACK Durum Kodları

| Status | Anlamı | Gönderici Tepkisi |
|--------|--------|-----------------|
| 0 | CRC hatası — blok bozuk | Aynı bloğu tekrar gönder |
| 1 | ACK — blok başarıyla alındı | Sonraki bloğa geç |
| 2 | NACK — bu blok eksik, tekrar gönder | Belirtilen bloğa geri dön |
| 3 | Tam imaj CRC doğrulaması başarısız | Transfer iptal et |
| 4 | Alıcı disk alanı doldu | Transfer sonlandır |

---

## 4. Tüm İmaj Doğrulaması (EOF Mekanizması)

Tüm bloklar gönderildikten sonra gönderici özel bir EOF (End of File) paketi gönderir. Bu paketin `seq_no` alanı `0xFFFF` olarak işaretlenmiştir; `crc` alanına ise tüm firmware'in kümülatif CRC16 değeri yerleştirilir. `payload` alanına dosya boyutu gömülür.

**Gönderici tarafı — EOF paketi oluşturma:**

```c
} else if(current_seq_no == TOTAL_CHUNKS) {
    packet.seq_no = 0xFFFF;          // EOF sinyali
    packet.length = 0;
    packet.crc = full_image_crc;     // Tüm firmware'in CRC16'sı

    uint32_t fw_size_meta = (uint32_t)TOTAL_FW_SIZE;
    memcpy(packet.payload, &fw_size_meta, sizeof(fw_size_meta));
}
```

Tam imaj CRC'si tüm bloklar üzerinden hesaplanır:

```c
static uint16_t calculate_full_file_crc() {
    uint16_t total_crc = 0;
    for(uint16_t j = 0; j < TOTAL_CHUNKS; j++) {
        uint16_t len = (j == TOTAL_CHUNKS - 1)
                       ? (TOTAL_FW_SIZE % CHUNK_SIZE)
                       : CHUNK_SIZE;
        if(len == 0) len = CHUNK_SIZE;
        total_crc = crc16_data(&new_firmware_z1[j * CHUNK_SIZE], len, total_crc);
    }
    return total_crc;
}
```

**Alıcı tarafı — EOF işleme:**

```c
if(packet->seq_no == 0xFFFF) {
    // 1. Payload'dan dosya boyutunu çıkar
    uint32_t actual_fw_size;
    memcpy(&actual_fw_size, packet->payload, sizeof(actual_fw_size));
    uint16_t actual_total_chunks = (actual_fw_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // 2. Bitmap üzerinde eksik blok taraması yap
    for(uint16_t i = 0; i < actual_total_chunks; i++) {
        if(!IS_CHUNK_RECEIVED(i)) {
            // Eksik blok var: NACK gönder
            ack.status = 2;
            return;
        }
    }

    // 3. Diskten okuyarak kümülatif CRC hesapla
    uint16_t downloaded_file_crc = 0;
    uint8_t buf[CHUNK_SIZE];
    for(uint16_t j = 0; j < actual_total_chunks; j++) {
        cfs_read(fw_fd, buf, len);
        downloaded_file_crc = crc16_data(buf, len, downloaded_file_crc);
    }

    // 4. Göndericinin CRC'siyle karşılaştır
    if(downloaded_file_crc == packet->crc) {
        LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
        ack.status = 1;  // EOF ACK
    } else {
        cfs_remove("new_fw.bin");  // Bozuk dosyayı sil
        ack.status = 3;            // Fatal error
    }
}
```

---

## 5. Kalıcı Depolama — Coffee File System (CFS)

Alıcı düğüm, gelen blokları Contiki-NG'nin **Coffee File System (CFS)** dosya sistemiyle kalıcı depolama alanına yazar. CFS, Flash bellek üzerinde çalışır; MSP430F2617'de bu alan EEPROM benzeri bir simülasyon ortamında Cooja tarafından sağlanır.

**Dosya açma ve hazırlık:**

```c
cfs_remove("new_fw.bin");                        // Varsa eski dosyayı temizle
fw_fd = cfs_open("new_fw.bin", CFS_WRITE);       // Yazma modunda aç

if(fw_fd < 0) {
    LOG_INFO("HATA: Disk (CFS) erisimi saglanamadi!\n");
}
```

**Blok yazma — sıralı erişim için seek:**

```c
cfs_seek(fw_fd, packet->seq_no * CHUNK_SIZE, CFS_SEEK_SET);
int written_bytes = cfs_write(fw_fd, packet->payload, packet->length);
```

`cfs_seek` ile her blok doğru konuma yazılır. Bu sayede bloklar sıra dışı gelse bile dosya doğru biçimde oluşur.

**Disk doluluk kontrolü:**

```c
if(written_bytes != packet->length) {
    transfer_aborted = true;
    LOG_INFO("Depolama alani doldu, OTA guvenli sekilde sonlandirildi.\n");
    ack.status = 4;
    return;
}
```

**Doğrulama için okuma:**

```c
cfs_close(fw_fd);
fw_fd = cfs_open("new_fw.bin", CFS_READ);  // Doğrulama için yeniden aç
cfs_read(fw_fd, buf, len);
```

---

## 6. Hata Yönetimi ve Alınan Önlemler

### 6.1 Blok Boyutu Sınır Kontrolü

Alıcı, gelen paketin uzunluğunun `CHUNK_SIZE` sınırını aşıp aşmadığını kontrol eder. Buffer taşması riskini önler:

```c
if(packet->length > CHUNK_SIZE) {
    return;   // Paketi sessizce düşür
}
```

### 6.2 Dizi Sınırı Kontrolü

Sıra numarası maksimum kapasitenin dışına çıkarsa blok işlenmez:

```c
if(packet->seq_no < MAX_CHUNKS) {
    // İşle
}
```

### 6.3 Tekrarlı Blok Gönderiminde Çift Yazma Önlemi

Aynı blok ikinci kez geldiğinde diske yeniden yazılmaz; bitmap kontrolü sayesinde bu durum atlanır:

```c
if(!IS_CHUNK_RECEIVED(packet->seq_no)) {
    cfs_write(...);              // Sadece ilk kez yaz
    SET_CHUNK_RECEIVED(packet->seq_no);
}
```

### 6.4 Bozuk İmaj Temizleme

Tam imaj CRC doğrulaması başarısız olursa dosya diskten silinir ve gönderici bilgilendirilir:

```c
cfs_close(fw_fd);
cfs_remove("new_fw.bin");   // Bozuk dosyayı temizle
ack.status = 3;              // Fatal error bildir
```

### 6.5 Transfer İptal Bayrağı

Geri dönülmez bir hata durumunda (disk dolu, bozuk imaj) `transfer_aborted` bayrağı set edilir ve sonraki tüm paketler sessizce düşürülür:

```c
static bool transfer_aborted = false;

static void udp_rx_callback(...) {
    if(transfer_aborted) return;   // Daha fazla paket işleme
    ...
}
```

### 6.6 RPL Ağ Hazır Olma Kontrolü

Gönderici, RPL ağı kurulmadan paket göndermez. Bu kontrol her döngüde yapılır:

```c
if(NETSTACK_ROUTING.node_is_reachable() &&
   NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
    // Ağ hazır, gönder
} else {
    LOG_INFO("Ag hazir degil, RPL aginin kurulmasi bekleniyor...\n");
    etimer_set(&periodic_timer, SEND_INTERVAL);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
}
```

---

## 7. Durum Yönetimi

Alıcı düğüm iki ayrı mekanizmayla durum yönetimi yapar:

**Bitmap ile blok takibi:** Her blok için 1 bit ayrılır. 131 KB'lık maksimum kapasite için yalnızca ~256 bayt RAM harcanır:

```
Blok 0    → received_chunks[0] bit 0
Blok 1    → received_chunks[0] bit 1
...
Blok 7    → received_chunks[0] bit 7
Blok 8    → received_chunks[1] bit 0
```

**`highest_seq_no` değişkeni:** Şimdiye kadar görülen en yüksek blok numarasını tutar. Eksik blok taraması yalnızca bu sınıra kadar yapılır; bu sayede gereksiz döngüden kaçınılır:

```c
if(packet->seq_no >= highest_seq_no) {
    highest_seq_no = packet->seq_no + 1;
}

for(uint16_t i = 0; i < highest_seq_no; i++) {
    if(!IS_CHUNK_RECEIVED(i)) { ... }
}
```

---

## 8. Sistem Akışı

```
[Düğüm 2 - Gönderici]                    [Düğüm 1 - Alıcı]
        |                                         |
        | RPL ağına katıl                         | DODAG kök ol
        | Tam imaj CRC hesapla                    | CFS dosyası aç
        |                                         |
        |──── Blok 0 (seq=0, crc=X) ─────────────>|
        |                                         | CRC kontrol
        |                                         | Diske yaz
        |<─── ACK (seq=0, status=1) ──────────────|
        |                                         |
        |──── Blok 1 (seq=1, crc=Y) ─────────────>|
        |<─── ACK (seq=1, status=1) ──────────────|
        |            ...                          |
        |──── Blok N (CRC hatalı) ───────────────>|
        |<─── NACK (seq=N, status=0) ─────────────| CRC uyuşmadı
        |──── Blok N (tekrar) ───────────────────>|
        |<─── ACK (seq=N, status=1) ──────────────|
        |            ...                          |
        |──── EOF (seq=0xFFFF, crc=toplam) ──────>|
        |                                         | Eksik blok tara
        |                                         | Disk CRC hesapla
        |<─── EOF ACK (status=1) ─────────────────| "Firmware tamamlandi"
        |                                         |
      Bitti                                     Bitti
```

---

## 9. Kullanılan Contiki-NG API'leri

| API | Kullanım Amacı |
|-----|---------------|
| `simple_udp_register()` | UDP soket kaydı ve RX callback bağlama |
| `simple_udp_sendto()` | UDP paketi gönderme |
| `NETSTACK_ROUTING.root_start()` | RPL DODAG kök düğümü olma |
| `NETSTACK_ROUTING.node_is_reachable()` | RPL ağ hazırlık kontrolü |
| `crc16_data()` | CRC16 hesaplama |
| `cfs_open()` / `cfs_close()` | CFS dosya açma/kapama |
| `cfs_read()` / `cfs_write()` | CFS okuma/yazma |
| `cfs_seek()` | CFS dosya konumu belirleme |
| `cfs_remove()` | CFS dosya silme |
| `etimer_set()` | Timeout zamanlayıcısı |
| `process_poll()` | Contiki sürecini uyandırma |
| `node_id` | Düğüm kimliği sorgulama |

---

## 10. Bellek Kullanımı

| Yapı | Boyut | Açıklama |
|------|-------|---------|
| `ota_packet_t` | 70 bayt | Gönderim paketi (6 başlık + 64 payload) |
| `ota_ack_t` | 3 bayt | ACK/NACK yanıt paketi |
| `received_chunks[]` | ~256 bayt | Bitmap (131 KB için blok takibi) |
| `full_image_crc` | 2 bayt | Tüm imajın CRC16 değeri |
| `buf[CHUNK_SIZE]` | 64 bayt | CFS okuma tamponu |

Gönderici düğümde `new_firmware_z1[]` dizisi Flash (ROM) belleğinde durur; RAM'e kopyalanmaz. Yalnızca `memcpy` ile paket payload'ına kopyalanır.

---

## 11. Simülasyon Koşulları

- **Simülatör:** Cooja (Contiki-NG ile birlikte)
- **Senaryo dosyası:** `BIL304-OS-Project-1.csc`
- **Gönderim aralığı:** 3 saniye (`SEND_INTERVAL = 3 * CLOCK_SECOND`)
- **Blok boyutu:** 64 bayt
- **Taşıma protokolü:** UDP üzeri RPL/6LoWPAN/IEEE 802.15.4
- **MAC katmanı:** CSMA/CA
- **Dosya sistemi:** Coffee File System (CFS)
- **Reboot:** Bu aşamada beklenmemektedir (ödev tanımı gereği)

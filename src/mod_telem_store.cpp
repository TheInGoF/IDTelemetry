#include "mod_telem_store.h"
#include "mod_logs.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

// ============================================================
//  Layout
//    Partition:    5 MB, Subtype 0x40, Label "telem"
//    Erase-Block:  4 KB (Flash-Sector)
//    Slot:         128 B  (32 Slots pro Block)
//    Total Slots:  5*1024*1024 / 128 = 40960
//
//  Status-Byte (1->0 Bit-Flips ohne Erase moeglich):
//    0xFF = leer (Default nach Erase, alles 1en)
//    0xFE = geschrieben, ungesendet
//    0xFC = gesendet (ack'd)
//
//  Append-Pfad (atomar):
//    1. Inhalt in leeren Slot schreiben (Status bleibt 0xFF)
//    2. Status auf 0xFE setzen  ← einziger Commit-Schritt
//    Crash vor Schritt 2 → Slot bleibt 0xFF, halb-geschriebener
//    Inhalt wird beim naechsten Write einfach ueberschrieben.
//
//  Ack-Pfad:
//    Status von 0xFE auf 0xFC setzen — 1 Bit-Flip, atomar.
//    Crash dazwischen → Slot bleibt 0xFE, Re-Sending → Duplikat
//    moeglich aber kein Datenverlust.
//
//  Block-Recycling:
//    Wenn der naechste zu schreibende Slot in einem Block liegt
//    der nicht komplett 0xFF ist, wird der Block erased.
//    Falls dieser Block ungesendete 0xFE-Slots enthielt → Datenverlust
//    (Buffer voll, aelteste Zeilen werden eingeholt). Wird geloggt.
// ============================================================

#define SLOT_SIZE          128u
#define SECTOR_SIZE        4096u
#define SLOTS_PER_SECTOR   (SECTOR_SIZE / SLOT_SIZE)   // 32

#define STATUS_EMPTY       0xFF
#define STATUS_WRITTEN     0xFE
#define STATUS_SENT        0xFC

// Auf-Disk Layout fuer eine Zeile.
// Felder MUESSEN exakt zur TelemetryRow passen.
struct __attribute__((packed)) Slot {
    uint8_t  status;                          // 1
    uint8_t  reserved[3];                     // 3   → 4
    uint32_t unix_s;                          // 4   → 8
    float    values[TELEM_FIELD_COUNT];       // 88  → 96
    uint8_t  valid[TELEM_FIELD_COUNT];        // 22  → 118
    uint8_t  pad[SLOT_SIZE - 118];            // 10  → 128
};
static_assert(sizeof(Slot) == SLOT_SIZE, "Slot size must be 128");

static const esp_partition_t* s_part        = nullptr;
static uint32_t                s_total_slots = 0;
static uint32_t                s_head        = 0;   // naechster Schreibslot
static uint32_t                s_tail        = 0;   // aeltester ungesendeter Slot
static uint32_t                s_pending     = 0;
static SemaphoreHandle_t       s_mtx         = nullptr;

// Hilfe: Slot-Index → Byte-Offset in Partition
static inline uint32_t slot_offset(uint32_t idx) { return idx * SLOT_SIZE; }

// Status-Byte eines Slots lesen
static uint8_t read_status(uint32_t idx) {
    uint8_t s = 0xFF;
    if (esp_partition_read(s_part, slot_offset(idx), &s, 1) != ESP_OK) return 0xFF;
    return s;
}

// Status-Byte ueberschreiben (nur 1→0 Bit-Flips, kein Erase noetig)
static bool write_status(uint32_t idx, uint8_t new_status) {
    return esp_partition_write(s_part, slot_offset(idx), &new_status, 1) == ESP_OK;
}

// Block erasen, der den Slot enthaelt
static bool erase_block_for_slot(uint32_t idx) {
    uint32_t block_start = (idx / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
    return esp_partition_erase_range(s_part, slot_offset(block_start), SECTOR_SIZE) == ESP_OK;
}

// Boot-Scan: liest alle Status-Bytes, bestimmt tail/head/pending.
// Algorithmus: zwei Zeiger laufen rund, finde laengste Folge von 0xFE.
// Wenn keine 0xFE existieren → Buffer leer, head=tail=0.
// Wenn alle 0xFE → Buffer voll, tail=head=0.
static void boot_scan() {
    s_pending = 0;

    // Status-Bytes effizient lesen: sektorweise 4 KB lesen, 32 Status extrahieren.
    uint8_t* sec_buf = (uint8_t*)malloc(SECTOR_SIZE);
    if (!sec_buf) {
        syslog("STORE", "FEHLER: malloc fuer Sector-Buffer");
        return;
    }

    uint32_t num_sectors = s_total_slots / SLOTS_PER_SECTOR;
    uint8_t* status = (uint8_t*)malloc(s_total_slots);
    if (!status) {
        free(sec_buf);
        syslog("STORE", "FEHLER: malloc fuer Status-Array");
        return;
    }

    for (uint32_t sec = 0; sec < num_sectors; sec++) {
        if (esp_partition_read(s_part, sec * SECTOR_SIZE, sec_buf, SECTOR_SIZE) != ESP_OK) {
            for (uint32_t s = 0; s < SLOTS_PER_SECTOR; s++) status[sec * SLOTS_PER_SECTOR + s] = 0xFF;
            continue;
        }
        for (uint32_t s = 0; s < SLOTS_PER_SECTOR; s++) {
            status[sec * SLOTS_PER_SECTOR + s] = sec_buf[s * SLOT_SIZE];
        }
    }
    free(sec_buf);

    // Strategie: finde Tail = erster 0xFE nach einem nicht-0xFE Slot.
    // Falls alle 0xFE → tail=0. Falls keine 0xFE → empty, tail=head=0.
    uint32_t count_written = 0;
    for (uint32_t i = 0; i < s_total_slots; i++) {
        if (status[i] == STATUS_WRITTEN) count_written++;
    }

    if (count_written == 0) {
        s_head = s_tail = 0;
        s_pending = 0;
        free(status);
        char m[64]; snprintf(m, sizeof(m), "Boot-Scan: leer (0/%u Slots)", s_total_slots);
        syslog("STORE", m);
        return;
    }

    // Tail finden: gehe rund herum, suche Stelle wo "nicht-FE → FE" wechselt.
    // Wenn solche Stelle existiert → tail dort. Sonst (alle FE oder vermischt
    // ohne klare Bloecke nach Crash) → nimm Slot 0 als Tail.
    s_tail = 0;
    for (uint32_t i = 0; i < s_total_slots; i++) {
        uint32_t prev_i = (i + s_total_slots - 1) % s_total_slots;
        if (status[i] == STATUS_WRITTEN && status[prev_i] != STATUS_WRITTEN) {
            s_tail = i;
            break;
        }
    }

    // Head finden: ab Tail vorwaerts, erster Slot der nicht 0xFE ist.
    s_head = s_tail;
    for (uint32_t k = 0; k < s_total_slots; k++) {
        uint32_t i = (s_tail + k) % s_total_slots;
        if (status[i] != STATUS_WRITTEN) {
            s_head = i;
            break;
        }
        // Bei k == s_total_slots-1 ohne Treffer → Buffer voll, Head = Tail
        if (k == s_total_slots - 1) s_head = s_tail;
    }

    s_pending = count_written;

    free(status);

    char m[96];
    snprintf(m, sizeof(m), "Boot-Scan: %u Rows pending (tail=%u head=%u capacity=%u)",
             s_pending, s_tail, s_head, s_total_slots);
    syslog("STORE", m);
}

void telem_store_init() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40, "telem");
    if (!s_part) {
        syslog("STORE", "FEHLER: telem-Partition nicht gefunden!");
        return;
    }

    s_total_slots = s_part->size / SLOT_SIZE;

    char m[96];
    snprintf(m, sizeof(m), "Partition gefunden: %u KB · %u Slots à %u B",
             s_part->size / 1024, s_total_slots, SLOT_SIZE);
    syslog("STORE", m);

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    boot_scan();
    xSemaphoreGive(s_mtx);
}

bool telem_store_append(const TelemetryRow& row) {
    if (!s_part || !s_mtx) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    // Wenn Buffer voll: aeltesten Block opfern (32 Rows weg) damit wir weiter aufnehmen.
    // Das passiert nur nach > 40k Rows ohne Upload (sehr selten).
    if (s_pending >= s_total_slots) {
        uint32_t lost_block_start = (s_tail / SLOTS_PER_SECTOR) * SLOTS_PER_SECTOR;
        erase_block_for_slot(s_tail);
        s_tail = (lost_block_start + SLOTS_PER_SECTOR) % s_total_slots;
        s_pending -= SLOTS_PER_SECTOR;
        syslog("STORE", "Buffer voll · aeltester Block geloescht (32 Rows verloren)");
    }

    // Pruefen ob Block-Erase noetig bevor wir schreiben.
    // Wenn head am Anfang eines Blocks steht und der Block nicht jungfraeulich ist (alte 0xFC) → erase.
    if (s_head % SLOTS_PER_SECTOR == 0) {
        uint8_t first_status = read_status(s_head);
        if (first_status != STATUS_EMPTY) {
            erase_block_for_slot(s_head);
        }
    }

    // Slot zusammenbauen: Status erst NACH dem Inhalt schreiben (atomare Commit-Sequenz)
    Slot slot;
    memset(&slot, 0xFF, sizeof(slot));  // alles 1en damit Bit-Writes spaeter funktionieren
    slot.status = STATUS_EMPTY;          // wird gleich von 0xFF auf 0xFE geflippt
    slot.unix_s = row.unix_s;
    memcpy(slot.values, row.values, sizeof(slot.values));
    for (int i = 0; i < TELEM_FIELD_COUNT; i++) slot.valid[i] = row.valid[i] ? 0x01 : 0x00;

    // Inhalt schreiben (Bytes 1..127, Status bleibt vorerst 0xFF)
    esp_err_t e = esp_partition_write(s_part,
                                      slot_offset(s_head) + 1,
                                      ((uint8_t*)&slot) + 1,
                                      SLOT_SIZE - 1);
    if (e != ESP_OK) {
        xSemaphoreGive(s_mtx);
        syslog("STORE", "Append: Write Body fehlgeschlagen");
        return false;
    }

    // Commit: Status 0xFF → 0xFE
    if (!write_status(s_head, STATUS_WRITTEN)) {
        xSemaphoreGive(s_mtx);
        syslog("STORE", "Append: Status-Commit fehlgeschlagen");
        return false;
    }

    s_head = (s_head + 1) % s_total_slots;
    s_pending++;
    xSemaphoreGive(s_mtx);
    return true;
}

bool telem_store_peek(TelemetryRow& out) {
    if (!s_part || !s_mtx) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    if (s_pending == 0) { xSemaphoreGive(s_mtx); return false; }

    Slot slot;
    esp_err_t e = esp_partition_read(s_part, slot_offset(s_tail), &slot, SLOT_SIZE);
    if (e != ESP_OK || slot.status != STATUS_WRITTEN) {
        // Sollte nicht passieren — wenn doch, defensive: tail vorruecken
        if (slot.status != STATUS_WRITTEN) {
            s_tail = (s_tail + 1) % s_total_slots;
            if (s_pending > 0) s_pending--;
        }
        xSemaphoreGive(s_mtx);
        return false;
    }

    out.unix_s = slot.unix_s;
    memcpy(out.values, slot.values, sizeof(out.values));
    for (int i = 0; i < TELEM_FIELD_COUNT; i++) out.valid[i] = (slot.valid[i] != 0);

    xSemaphoreGive(s_mtx);
    return true;
}

void telem_store_ack() {
    if (!s_part || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (s_pending == 0) { xSemaphoreGive(s_mtx); return; }

    // Status auf 0xFC setzen
    write_status(s_tail, STATUS_SENT);

    s_tail = (s_tail + 1) % s_total_slots;
    s_pending--;

    xSemaphoreGive(s_mtx);
}

uint32_t telem_store_pending() {
    return s_pending;  // lesender single-word access, ok ohne Lock
}

uint32_t telem_store_capacity() {
    return s_total_slots;
}

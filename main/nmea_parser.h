/*
 * nmea_parser.h — NMEA 0183 sentence parser for GPS/GNSS data
 *
 * Extracts:
 *  - Position (lat/lon/alt) from $GNGGA or $GNRMC
 *  - Satellite count (used + in-view) from $GNGGA / $GPGSV / $BDGSV
 *  - Signal strength (SNR max/avg) from $GPGSV / $BDGSV
 *  - Fix quality & DOP from $GNGSA
 */

#ifndef NMEA_PARSER_H
#define NMEA_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Parsed GPS data ────────────────────────────────────── */
typedef struct {
    /* ── Position ── */
    bool     has_position;
    double   latitude;        /* decimal degrees, +N/+E, -S/-W */
    double   longitude;
    float    altitude_m;      /* meters above MSL */

    /* ── Fix ── */
    int      fix_quality;     /* 0=invalid, 1=GPS, 2=DGPS, 4=RTK fixed, 5=RTK float */
    int      satellites_used; /* from GGA (sats used in solution) */
    int      satellites_in_view; /* total from GSV sentences */
    int      gps_sats_in_view;
    int      bd_sats_in_view;

    /* ── Signal strength (SNR dBHz) ── */
    int      max_snr;         /* best SNR across all visible sats */
    float    avg_snr;         /* average SNR of visible sats */
    int      snr_sample_count;/* how many sats contributed SNR */

    /* ── DOP (dilution of precision) ── */
    float    pdop;
    float    hdop;
    float    vdop;

    /* ── Time/Date ── */
    char     utc_time[12];    /* hhmmss.ss */
    char     date[8];         /* ddmmyy   */

    /* ── Flags ── */
    bool     updated;         /* set true when new data arrives */
    bool     antenna_ok;      /* false if ANTENNA OPEN */
} gps_data_t;

/* ── API ────────────────────────────────────────────────── */

/**
 * Feed one byte of NMEA data.
 * When a complete sentence is accumulated and checksum-validated,
 * it is parsed and gps_data is updated.
 */
void nmea_feed_byte(char c);

/**
 * Atomically copy the latest parsed data.
 * Returns true if new data was available (clears `updated` flag).
 */
bool nmea_get_data(gps_data_t *out);

/**
 * Format parsed data into a compact JSON string.
 * Returns length written (excluding null terminator).
 * Returns 0 if buffer too small.
 */
int nmea_to_json(const gps_data_t *d, char *buf, size_t buf_size);

/**
 * Reset internal parser state.
 */
void nmea_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NMEA_PARSER_H */

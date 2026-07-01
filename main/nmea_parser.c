/*
 * nmea_parser.c — NMEA 0183 sentence parser implementation
 *
 * Thread-safe: nmea_feed_byte() and nmea_get_data() use a spinlock.
 */

#include "nmea_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Constants ──────────────────────────────────────────── */
#define NMEA_MAX_FIELDS     32
#define NMEA_BUF_SIZE       256
#define NMEA_MAX_SNR_SATS   64    /* max tracked sats for SNR stats */

/* ── Internal state ─────────────────────────────────────── */
static gps_data_t g_gps;                     /* latest parsed data */
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

/* Sentence assembly buffer */
static char  nmea_buf[NMEA_BUF_SIZE];
static int   nmea_pos = 0;
static bool  nmea_in_sentence = false;

/* SNR accumulator (running stats across GSV sentences) */
static int   snr_values[NMEA_MAX_SNR_SATS];
static int   snr_count = 0;

/* ── Forward declarations ───────────────────────────────── */
static void parse_gga(const char *fields[], int nf);
static void parse_gsa(const char *fields[], int nf);
static void parse_gsv(const char *fields[], int nf, char constellation);
static void parse_rmc(const char *fields[], int nf);
static int  nmea_checksum_ok(const char *sentence);

/* ── NMEA checksum validation ────────────────────────────── */
static int nmea_checksum_ok(const char *sentence)
{
    /* sentence looks like "$GNGGA,...*4F" */
    if (!sentence || sentence[0] != '$') return 0;

    const char *star = strchr(sentence, '*');
    if (!star || star == sentence + 1) return 0;

    /* XOR all chars between '$' and '*' (exclusive) */
    uint8_t cs = 0;
    for (const char *p = sentence + 1; p < star; p++) {
        cs ^= (uint8_t)*p;
    }

    /* Parse the hex checksum after '*' */
    char hex[3] = {0};
    hex[0] = star[1];
    hex[1] = (star[2] && star[2] != '\r' && star[2] != '\n') ? star[2] : '\0';
    uint8_t expected = (uint8_t)strtol(hex, NULL, 16);

    return (cs == expected) ? 1 : 0;
}

/* ── Field splitting helper ──────────────────────────────── */
static int split_fields(char *sentence, const char *fields[], int max_fields)
{
    int n = 0;
    char *tok = sentence;
    fields[n++] = tok;

    for (char *p = sentence; *p && n < max_fields; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

/* ── Convert NMEA lat/lon to decimal degrees ─────────────── */
static double nmea_to_degrees(const char *coord, const char *hemi)
{
    if (!coord || !coord[0] || !hemi || !hemi[0]) return NAN;

    double val = atof(coord);
    if (val == 0.0) return NAN;

    /* NMEA format: ddmm.mmmm or dddmm.mmmm */
    /* Degrees = integer part of (val / 100) */
    /* Minutes = remainder / 60 */
    int deg = (int)(val / 100.0);
    double minutes = val - (deg * 100.0);
    double result = (double)deg + minutes / 60.0;

    if (hemi[0] == 'S' || hemi[0] == 'W') result = -result;
    return result;
}

/* ── Parse $--GGA ────────────────────────────────────────── */
static void parse_gga(const char *fields[], int nf)
{
    /* $GNGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh */
    /*  0        1         2       3  4         5 6 7  8   9   10  11  12   13  */
    if (nf < 8) return;

    /* UTC time */
    if (fields[1] && fields[1][0]) {
        strlcpy(g_gps.utc_time, fields[1], sizeof(g_gps.utc_time));
    }

    /* Latitude */
    double lat = nmea_to_degrees(fields[2], fields[3]);
    double lon = nmea_to_degrees(fields[4], fields[5]);

    if (!isnan(lat) && !isnan(lon)) {
        g_gps.latitude = lat;
        g_gps.longitude = lon;
        g_gps.has_position = true;
    }

    /* Fix quality: 0=invalid, 1=GPS, 2=DGPS, 4=RTK fixed, 5=RTK float */
    if (fields[6] && fields[6][0]) {
        g_gps.fix_quality = atoi(fields[6]);
    } else {
        g_gps.fix_quality = 0;
    }

    /* Satellites used */
    if (fields[7] && fields[7][0]) {
        g_gps.satellites_used = atoi(fields[7]);
    }

    /* HDOP */
    if (fields[8] && fields[8][0]) {
        g_gps.hdop = atof(fields[8]);
    }

    /* Altitude */
    if (nf > 9 && fields[9] && fields[9][0]) {
        g_gps.altitude_m = atof(fields[9]);
    }

    g_gps.updated = true;
}

/* ── Parse $--GSA ────────────────────────────────────────── */
static void parse_gsa(const char *fields[], int nf)
{
    /* $GNGSA,a,x,xx,xx,...(12 PRNs),x.x,x.x,x.x,x*hh */
    /*  0      1 2  3..14            15  16  17  18 */
    if (nf < 17) return;

    /* PDOP, HDOP, VDOP */
    if (fields[15] && fields[15][0]) g_gps.pdop = atof(fields[15]);
    if (fields[16] && fields[16][0]) g_gps.hdop = atof(fields[16]);
    if (fields[17] && fields[17][0]) g_gps.vdop = atof(fields[17]);
}

/* ── Parse $--GSV (GPS / BD / GL / GA) ───────────────────── */
static void parse_gsv(const char *fields[], int nf, char constellation)
{
    /* $GPGSV,t,m,ss,sv,elev,az,snr,sv,elev,az,snr,sv,elev,az,snr,sv,elev,az,snr*hh */
    /*  0      1 2 3  4  5    6  7   8   9   10 11  12  13  14  15  16  17  18  19 */
    if (nf < 4) return;

    /* int total_msgs = atoi(fields[1]); */ /* total GSV messages (unused) */
    int msg_num      = atoi(fields[2]);  /* this message number */
    int total_sats   = atoi(fields[3]);  /* total satellites in view */

    /* On first message of a constellation, record the total */
    if (msg_num == 1) {
        if (constellation == 'P') {        /* GPS */
            g_gps.gps_sats_in_view = total_sats;
        } else if (constellation == 'D') { /* BeiDou */
            g_gps.bd_sats_in_view = total_sats;
        }
        /* Sum of all constellations */
        g_gps.satellites_in_view = g_gps.gps_sats_in_view + g_gps.bd_sats_in_view;
    }

    /* Extract SNR from each satellite block (fields 4-7, 8-11, 12-15, 16-19) */
    for (int blk = 0; blk < 4; blk++) {
        int base = 4 + blk * 4;
        if (base + 3 >= nf) break;

        /* SNR is the 4th field in each block */
        const char *snr_str = fields[base + 3];
        if (snr_str && snr_str[0]) {
            int snr = atoi(snr_str);
            if (snr > 0 && snr_count < NMEA_MAX_SNR_SATS) {
                snr_values[snr_count++] = snr;
            }
        }
    }
}

/* ── Parse $--RMC ────────────────────────────────────────── */
static void parse_rmc(const char *fields[], int nf)
{
    /* $GNRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a,m*hh */
    /*  0      1         2  3       4  5         6 7   8   9     10  11 12 */
    if (nf < 10) return;

    /* Only parse if position not already set from GGA, and RMC is valid */
    if (!g_gps.has_position && fields[2] && fields[2][0] == 'A') {
        double lat = nmea_to_degrees(fields[3], fields[4]);
        double lon = nmea_to_degrees(fields[5], fields[6]);
        if (!isnan(lat) && !isnan(lon)) {
            g_gps.latitude = lat;
            g_gps.longitude = lon;
            g_gps.has_position = true;
        }
    }

    /* Date */
    if (fields[9] && fields[9][0]) {
        strlcpy(g_gps.date, fields[9], sizeof(g_gps.date));
    }
}

/* ── Parse a complete NMEA sentence ──────────────────────── */
static void nmea_dispatch(char *sentence)
{
    /* Validate checksum */
    if (!nmea_checksum_ok(sentence)) {
        return;
    }

    /* Split into fields */
    const char *fields[NMEA_MAX_FIELDS];
    int nf = split_fields(sentence, fields, NMEA_MAX_FIELDS);
    if (nf < 2) return;

    const char *talker = fields[0]; /* e.g. "$GNGGA" */

    /* ── Route by sentence type ── */
    if (strstr(talker, "GGA")) {
        parse_gga(fields, nf);
    }
    else if (strstr(talker, "GSA")) {
        parse_gsa(fields, nf);
    }
    else if (strstr(talker, "GSV")) {
        /* Determine constellation from talker ID */
        /* $GPGSV=GPS, $BDGSV=BeiDou, $GLGSV=GLONASS, $GAGSV=Galileo */
        char constellation = '?';
        if (talker[1] == 'P' || talker[2] == 'P') constellation = 'P'; /* GP=GPS */
        if (talker[1] == 'D' || talker[2] == 'D') constellation = 'D'; /* BD=BeiDou */
        if (talker[1] == 'L') constellation = 'L';  /* GL=GLONASS */
        if (talker[1] == 'A') constellation = 'A';  /* GA=Galileo */
        parse_gsv(fields, nf, constellation);
    }
    else if (strstr(talker, "RMC")) {
        parse_rmc(fields, nf);
    }
    else if (strstr(talker, "TXT")) {
        /* Check for ANTENNA OPEN warning */
        if (nf >= 5 && strstr(fields[4], "ANTENNA OPEN")) {
            g_gps.antenna_ok = false;
        } else if (nf >= 5 && strstr(fields[4], "ANTENNA OK")) {
            g_gps.antenna_ok = true;
        }
    }
}

/* ── Compute SNR stats ───────────────────────────────────── */
static void compute_snr_stats(void)
{
    if (snr_count == 0) {
        g_gps.max_snr = 0;
        g_gps.avg_snr = 0.0f;
        g_gps.snr_sample_count = 0;
        return;
    }

    int max = 0;
    int sum = 0;
    for (int i = 0; i < snr_count; i++) {
        if (snr_values[i] > max) max = snr_values[i];
        sum += snr_values[i];
    }

    g_gps.max_snr = max;
    g_gps.avg_snr = (float)sum / (float)snr_count;
    g_gps.snr_sample_count = snr_count;

    /* Reset for next cycle */
    snr_count = 0;
}

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */

void nmea_feed_byte(char c)
{
    /* ── Sentence start ── */
    if (c == '$') {
        nmea_in_sentence = true;
        nmea_pos = 0;
        nmea_buf[nmea_pos++] = c;
        return;
    }

    if (!nmea_in_sentence) {
        /* Discard bytes outside of NMEA sentences (e.g. timestamps) */
        return;
    }

    /* ── End of sentence (LF or CR) ── */
    if (c == '\n' || c == '\r') {
        if (nmea_pos > 0) {
            nmea_buf[nmea_pos] = '\0';

            /* Acquire lock and update global data */
            portENTER_CRITICAL(&g_mux);
            nmea_dispatch(nmea_buf);

            /* If this was a GGA sentence, finalize SNR stats */
            if (strstr(nmea_buf, "GGA")) {
                compute_snr_stats();
            }
            portEXIT_CRITICAL(&g_mux);
        }
        nmea_in_sentence = false;
        nmea_pos = 0;
        return;
    }

    /* ── Buffer the byte ── */
    if (nmea_pos < (int)(sizeof(nmea_buf) - 1)) {
        nmea_buf[nmea_pos++] = c;
    } else {
        /* Overflow — discard this sentence */
        nmea_in_sentence = false;
        nmea_pos = 0;
    }
}

bool nmea_get_data(gps_data_t *out)
{
    if (!out) return false;

    portENTER_CRITICAL(&g_mux);
    bool has_new = g_gps.updated;
    if (has_new) {
        memcpy(out, &g_gps, sizeof(gps_data_t));
        g_gps.updated = false;
    }
    portEXIT_CRITICAL(&g_mux);

    return has_new;
}

int nmea_to_json(const gps_data_t *d, char *buf, size_t buf_size)
{
    if (!d || !buf || buf_size < 64) return 0;

    /* Format lat/lon safely */
    char lat_str[32] = "null";
    char lon_str[32] = "null";

    if (d->has_position) {
        snprintf(lat_str, sizeof(lat_str), "%.6f", d->latitude);
        snprintf(lon_str, sizeof(lon_str), "%.6f", d->longitude);
    }

    return snprintf(buf, buf_size,
        "{"
        "\"type\":\"gps\","
        "\"time\":\"%s\","
        "\"date\":\"%s\","
        "\"lat\":%s,"
        "\"lon\":%s,"
        "\"alt\":%.1f,"
        "\"fix\":%d,"
        "\"sat_used\":%d,"
        "\"sat_view\":%d,"
        "\"gps_sat\":%d,"
        "\"bd_sat\":%d,"
        "\"snr_max\":%d,"
        "\"snr_avg\":%.1f,"
        "\"snr_cnt\":%d,"
        "\"pdop\":%.1f,"
        "\"hdop\":%.1f,"
        "\"vdop\":%.1f,"
        "\"antenna\":%s"
        "}",
        d->utc_time,
        d->date,
        lat_str,
        lon_str,
        (double)d->altitude_m,
        d->fix_quality,
        d->satellites_used,
        d->satellites_in_view,
        d->gps_sats_in_view,
        d->bd_sats_in_view,
        d->max_snr,
        (double)d->avg_snr,
        d->snr_sample_count,
        (double)d->pdop,
        (double)d->hdop,
        (double)d->vdop,
        d->antenna_ok ? "true" : "false"
    );
}

void nmea_reset(void)
{
    portENTER_CRITICAL(&g_mux);
    memset(&g_gps, 0, sizeof(g_gps));
    g_gps.antenna_ok = true;  /* assume OK until proven otherwise */
    snr_count = 0;
    nmea_in_sentence = false;
    nmea_pos = 0;
    portEXIT_CRITICAL(&g_mux);
}

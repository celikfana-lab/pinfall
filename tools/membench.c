/* membench.c - PinFall RAM-cache evidence benchmark (v4).
 *
 * Fetches real Pinterest thumbnails through the production image pipeline and
 * measures, per image: JPEG bytes, decoded RGBA bytes, decode time, LZ4 pack
 * time, LZ4 unpack time and packed/raw ratio.  Prints a human table plus
 * machine-parseable `key=value` summary lines (and RAM extrapolations for
 * 100/500/1000 pins for the JPEG, LZ4 and decoded-RGBA policies).
 *
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lz4.h"
#include "pc.h"

#define MEMBENCH_MAX 24

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *query = (argc > 1 && argv[1][0]) ? argv[1] : "minimalist bedroom";
    int         want = (argc > 2) ? atoi(argv[2]) : 12;
    PcClient   *client;
    PcPinList   list;
    char       *err = NULL;
    const char *urls[MEMBENCH_MAX];
    int         nurls = 0;
    int         i;
    int         n = 0;
    int         mismatches = 0;
    double      sum_jpeg = 0, sum_rgba = 0, sum_packed = 0;
    double      sum_decode = 0, sum_pack = 0, sum_unpack = 0, sum_fetch = 0;

    if (want < 1)
        want = 1;
    if (want > MEMBENCH_MAX)
        want = MEMBENCH_MAX;

    client = pc_client_new(NULL);
    if (!client) {
        fprintf(stderr, "membench: could not create client\n");
        return 2;
    }
    if (!pc_bootstrap(client)) {
        fprintf(stderr, "membench: bootstrap failed: %s\n",
                pc_last_error(client));
        pc_client_free(client);
        return 2;
    }
    memset(&list, 0, sizeof list);
    if (!pc_search(client, query, NULL, &list, &err)) {
        fprintf(stderr, "membench: search failed: %s\n",
                err ? err : pc_last_error(client));
        pc_free(err);
        pc_client_free(client);
        return 2;
    }
    for (i = 0; (size_t)i < list.count && nurls < want; i++) {
        const PcPin *p = &list.items[i];

        if (p->thumb_url[0])
            urls[nurls++] = p->thumb_url;
    }

    printf("# PinFall membench - RAM cache evidence (v4)\n");
    printf("query=%s\n", query);
    printf("urls_found=%d\n", nurls);
    printf("# idx | jpeg_bytes | rgba_bytes | decode_ms | pack_ms | unpack_ms"
           " | ratio | fetch_ms\n");

    for (i = 0; i < nurls; i++) {
        uint8_t *bytes = NULL;
        size_t   blen = 0;
        uint8_t *rgba = NULL;
        int      rw = 0, rh = 0;
        uint8_t *unpacked = NULL;
        int      uw = 0, uh = 0;
        uint8_t *packed = NULL;
        size_t   plen = 0;
        char    *e = NULL;
        double   t0, t_fetch, t_decode, t_pack, t_unpack;
        double   raw_bytes, ratio;

        t0 = now_ms();
        if (!pc_image_fetch_bytes(client, urls[i], &bytes, &blen, &e)) {
            fprintf(stderr, "membench: skip image %d: %s\n", i,
                    e ? e : pc_last_error(client));
            pc_free(e);
            continue;
        }
        t_fetch = now_ms() - t0;

        t0 = now_ms();
        if (!pc_image_decode(bytes, blen, 0, &rgba, &rw, &rh, &e)) {
            fprintf(stderr, "membench: skip image %d decode: %s\n", i,
                    e ? e : "decode failed");
            pc_free(e);
            pc_free(bytes);
            continue;
        }
        t_decode = now_ms() - t0;

        t0 = now_ms();
        if (!pc_image_pack(rgba, rw, rh, &packed, &plen, &e)) {
            fprintf(stderr, "membench: skip image %d pack: %s\n", i,
                    e ? e : "pack failed");
            pc_free(e);
            pc_free(rgba);
            pc_free(bytes);
            continue;
        }
        t_pack = now_ms() - t0;

        t0 = now_ms();
        if (!pc_image_unpack(packed, plen, &unpacked, &uw, &uh, &e)) {
            fprintf(stderr, "membench: unpack failed: %s\n",
                    e ? e : "unpack failed");
            pc_free(e);
            pc_free(packed);
            pc_free(rgba);
            pc_free(bytes);
            continue;
        }
        t_unpack = now_ms() - t0;

        raw_bytes = (double)rw * (double)rh * 4.0;
        ratio = (raw_bytes > 0.0) ? (double)plen / raw_bytes : 0.0;
        if (uw != rw || uh != rh || memcmp(unpacked, rgba, (size_t)raw_bytes) != 0)
            mismatches++;

        printf("%d | %zu | %.0f | %.3f | %.3f | %.3f | %.3f | %.1f\n", i,
               blen, raw_bytes, t_decode, t_pack, t_unpack, ratio, t_fetch);
        sum_jpeg += (double)blen;
        sum_rgba += raw_bytes;
        sum_packed += (double)plen;
        sum_decode += t_decode;
        sum_pack += t_pack;
        sum_unpack += t_unpack;
        sum_fetch += t_fetch;
        n++;

        pc_free(unpacked);
        pc_free(packed);
        pc_free(rgba);
        pc_free(bytes);
        pc_free(e);
    }

    pc_pinlist_free(&list);

    if (n == 0) {
        fprintf(stderr, "membench: no images measured\n");
        pc_client_free(client);
        return 1;
    }

    {
        double avg_jpeg = sum_jpeg / n;
        double avg_rgba = sum_rgba / n;
        double avg_packed = sum_packed / n;
        double avg_ratio = (avg_rgba > 0.0) ? avg_packed / avg_rgba : 0.0;
        int    pins[3] = {100, 500, 1000};
        int    k;

        printf("\n");
        printf("images=%d\n", n);
        printf("roundtrip_mismatches=%d\n", mismatches);
        printf("avg_jpeg_bytes=%.0f\n", avg_jpeg);
        printf("avg_rgba_bytes=%.0f\n", avg_rgba);
        printf("avg_packed_bytes=%.0f\n", avg_packed);
        printf("avg_decode_ms=%.3f\n", sum_decode / n);
        printf("avg_pack_ms=%.3f\n", sum_pack / n);
        printf("avg_unpack_ms=%.3f\n", sum_unpack / n);
        printf("avg_fetch_ms=%.1f\n", sum_fetch / n);
        printf("avg_ratio_packed_raw=%.3f\n", avg_ratio);
        printf("avg_ratio_rgba_jpeg=%.2f\n",
               (avg_jpeg > 0.0) ? avg_rgba / avg_jpeg : 0.0);
        printf("revisit_ms_jpeg_mode=%.3f\n", sum_decode / n);
        printf("revisit_ms_lz4_mode=%.3f\n", sum_unpack / n);
        printf("lz4_version=%s\n", LZ4_versionString());
        printf("cache_budget_default_mb=64\n");

        for (k = 0; k < 3; k++) {
            double p = (double)pins[k];

            printf("ram_jpeg_%d_mb=%.2f\n", pins[k],
                   p * avg_jpeg / (1024.0 * 1024.0));
            printf("ram_lz4_%d_mb=%.2f\n", pins[k],
                   p * avg_packed / (1024.0 * 1024.0));
            printf("ram_rgba_%d_mb=%.2f\n", pins[k],
                   p * avg_rgba / (1024.0 * 1024.0));
        }
        printf("ram_jpeg_per_pin_kb=%.1f\n", avg_jpeg / 1024.0);
        printf("ram_lz4_per_pin_kb=%.1f\n", avg_packed / 1024.0);
        printf("ram_rgba_per_pin_kb=%.1f\n", avg_rgba / 1024.0);
    }

    pc_client_free(client);
    return (mismatches == 0) ? 0 : 1;
}

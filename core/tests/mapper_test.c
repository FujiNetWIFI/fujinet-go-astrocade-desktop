/*
 * mapper_test -- the staged astromap classifies image sizes the way the
 * cart device relies on: FLAT (<=8K), exact 256K/512K games, and claimed
 * APPBANK images. Also checks the FLAT mirror the served window shows.
 * astromap.c is a shared firmware source (identical-by-construction, staged
 * by cmake/StageFujiProto.cmake); this guards that the staging is intact and
 * links.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astromap.h"
#include "fuji_mailbox.h"

static int failures;

static void expect_kind(const char *what, uint32_t size, int has_claim,
                        astromap_kind_t want)
{
    uint8_t *img = calloc(1, size ? size : 1);
    astromap_plan_t plan;
    /* a claimed image carries FN_R_CLAIM_SIG at FN_R_CLAIM */
    if (has_claim && size >= FN_R_CLAIM + 4)
        memcpy(img + FN_R_CLAIM, FN_R_CLAIM_SIG, 4);
    astromap_err_t err = astromap_plan(img, size, &plan);
    if (err != ASTROMAP_OK)
    {
        fprintf(stderr, "FAIL %s: plan err %d\n", what, err);
        failures++;
    }
    else if (plan.kind != want)
    {
        fprintf(stderr, "FAIL %s: kind %d want %d\n", what, plan.kind, want);
        failures++;
    }
    free(img);
}

int main(void)
{
    /* 2K/4K/8K images are FLAT regardless of claim */
    expect_kind("2K flat", 0x0800, 0, ASTROMAP_FLAT);
    expect_kind("8K flat", 0x2000, 0, ASTROMAP_FLAT);

    /* exact 256K / 512K with no claim -> game mappers */
    expect_kind("256K game", ASTROMAP_GAME256_SIZE, 0, ASTROMAP_GAME256);
    expect_kind("512K game", ASTROMAP_GAME512_SIZE, 0, ASTROMAP_GAME512);

    /* a claimed app-shaped image (8K + k*4K) -> APPBANK */
    expect_kind("32K appbank", 0x8000, 1, ASTROMAP_APPBANK);

    /* the FLAT mirror: a 2K image repeats through the 8K window */
    {
        uint8_t img[0x800];
        uint8_t window[ASTROMAP_WINDOW];
        astromap_plan_t plan;
        for (int i = 0; i < 0x800; i++)
            img[i] = (uint8_t)i;
        if (astromap_plan(img, sizeof img, &plan) == ASTROMAP_OK)
        {
            astromap_apply(img, &plan, window);
            if (window[0x000] != window[0x800] ||
                window[0x000] != window[0x1000] ||
                window[0x123] != window[0x1923])
            {
                fprintf(stderr, "FAIL mirror: 2K image not repeated in window\n");
                failures++;
            }
        }
        else
        {
            fprintf(stderr, "FAIL mirror: plan failed\n");
            failures++;
        }
    }

    fprintf(stderr, "mapper_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
